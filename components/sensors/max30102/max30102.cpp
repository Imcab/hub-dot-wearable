#include "max30102.hpp"
 
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

MAX30102::~MAX30102(){
    release();
}

void MAX30102::release(){
    if (dev_ != nullptr){
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t MAX30102::configure(const Config &cfg){

    // Valida la configuracion ANTES de tocar el chip (A_FULL es un campo de 4 bits, 0 a 15)
    ESP_RETURN_ON_FALSE(cfg.fifo_a_full <= 15, ESP_ERR_INVALID_ARG, MAX30102_TAG,
        "fifo_a_full out of range: %u (max 15)", cfg.fifo_a_full);

    // Primero, revisa que si sea un MAX30102
    uint8_t v{0};
    ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_PART_ID, &v), MAX30102_TAG, "Could not read PART_ID");

    ESP_RETURN_ON_FALSE(v == MAX30102_EXPECTED_PART_ID, ESP_ERR_NOT_FOUND, MAX30102_TAG,
        "Unexpected PART_ID: 0x%02X (expected 0x%02X)", v, MAX30102_EXPECTED_PART_ID
    );

    ESP_LOGI(MAX30102_TAG, "MAX30102 detected, PART_ID = 0x%02X", v);

    // Intenta un reset
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_RESET), MAX30102_TAG, "Reset write failed");

    // Despues del reset hay que esperar a que el bit de reset vuelva a 0
    int tries = 0;
    do{
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_MODE_CONFIG, &v), MAX30102_TAG, "Reset poll failed");
        tries++;
    } while ((v & MAX30102_MODE_RESET) && tries < 100); // la mascara AND solo mira el bit 6
    ESP_RETURN_ON_FALSE(!(v & MAX30102_MODE_RESET), ESP_ERR_TIMEOUT, MAX30102_TAG, "Reset did not finish");

    // Limpia los apuntadores del FIFO (pagina 14)
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_WRITE_PTR, 0x00), MAX30102_TAG, "WR_PTR");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_OVERFLOW_COUNTER, 0x00), MAX30102_TAG, "OVF");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_READ_PTR, 0x00), MAX30102_TAG, "RD_PTR");

    // Configuracion: cada byte de registro se arma juntando los campos con OR
    // El ternario va entre parentesis: ?: tiene menor prioridad que |
    uint8_t fifo_cfg = cfg.smp_ave
                     | (cfg.fifo_rollover ? MAX30102_FIFO_ROLLOVER_ENABLE : 0)
                     | MAX30102_FIFO_A_FULL(cfg.fifo_a_full);
    uint8_t spo2_cfg = cfg.adc_range | cfg.sample_rate | cfg.pulse_width;

    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_CONFIG, fifo_cfg), MAX30102_TAG, "FIFO_CONFIG");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_SPO2_CONFIG, spo2_cfg), MAX30102_TAG, "SPO2_CONFIG");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_LED1_PA, cfg.led_red_pa), MAX30102_TAG, "LED1_PA");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_LED2_PA, cfg.led_ir_pa), MAX30102_TAG, "LED2_PA");

    // Por ultimo, modo SpO2
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_SPO2), MAX30102_TAG, "MODE_CONFIG");

    ESP_LOGI(MAX30102_TAG, "Configured: FIFO_CONFIG=0x%02X SPO2_CONFIG=0x%02X", fifo_cfg, spo2_cfg);

    return ESP_OK;
}

esp_err_t MAX30102::begin(i2c_master_bus_handle_t bus, const Config &cfg){

    // 1. Validar
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_ARG, MAX30102_TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(dev_ == nullptr, ESP_ERR_INVALID_STATE, MAX30102_TAG, "already started");

    // 2. Ver si responde en el bus
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, MAX30102_ADDRESS, MAX30102_I2C_TIMEOUT_MS),
        MAX30102_TAG, "No ACK at 0x%02X", MAX30102_ADDRESS);

    // 3. Agregar el dispositivo
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = MAX30102_ADDRESS;
    dev_cfg.scl_speed_hz = MAX30102_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev_), MAX30102_TAG, "Could not add the device");

    // 4 y 5. Configurar, y limpiar si fallo
    esp_err_t err = configure(cfg);
    if (err != ESP_OK) {
        release();
    }
    return err;
}

int MAX30102::readSamples(Sample *out, int maxSamples){

    if(dev_ == nullptr || out == nullptr || maxSamples <= 0) return -1;

    uint8_t ptrs[3];
    if(readRegs(MAX30102_REG_FIFO_WRITE_PTR, ptrs, sizeof(ptrs)) != ESP_OK) return -1;

    uint8_t wr  = ptrs[0] & 0x1F;
    uint8_t ovf = ptrs[1] & 0x1F;
    uint8_t rd  = ptrs[2] & 0x1F;

    int available = (wr - rd) & (MAX30102_FIFO_DEPTH - 1);

    if (available == 0 && ovf > 0) {
        // el FIFO esta lleno, no vacio
        available = MAX30102_FIFO_DEPTH;
    }
    // Vacio
    if (available == 0) return 0;
    
    // no pasar de lo que cabe en la salida
    if (available > maxSamples) {
        available = maxSamples;
    }

    uint8_t raw[MAX30102_FIFO_DEPTH * MAX30102_BYTES_PER_SAMPLE]; // 192 bytes
    size_t total =  (size_t)available * MAX30102_BYTES_PER_SAMPLE;

    if (readRegs(MAX30102_REG_FIFO_DATA, raw, total) != ESP_OK) return -1;

    for(int i = 0; i < available; i++){
        const uint8_t *s = &raw[i * MAX30102_BYTES_PER_SAMPLE];
        // bytes 1-3: LED1 (rojo)
        out[i].red = build18Bits(s[0], s[1], s[2]);
        // bytes 4-6: LED2 (IR)
        out[i].ir  = build18Bits(s[3], s[4], s[5]);
    }

    return available;
}

esp_err_t MAX30102::readTemperature(float *out_c){
    ESP_RETURN_ON_FALSE(dev_ != nullptr && out_c != nullptr, ESP_ERR_INVALID_STATE, MAX30102_TAG, "not started");

    // Inicia una conversion. Tarda ~29 ms (pagina 4), asi que espera eso antes de revisar:
    // si revisa antes puede ver TEMP_EN todavia en 0 y leer un resultado viejo (o vacio)
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_DIE_TEMP_CONFIG, MAX30102_TEMP_EN), MAX30102_TAG, "TEMP_EN");
    vTaskDelay(pdMS_TO_TICKS(30));

    // TEMP_EN vuelve a 0 cuando el resultado esta listo
    uint8_t v{0};
    int tries = 0;
    while (true) {
        ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_DIE_TEMP_CONFIG, &v), MAX30102_TAG, "Temp poll failed");
        if (!(v & MAX30102_TEMP_EN) || ++tries >= 14) break; // 30 + 14 x 5 ms = 100 ms maximo
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_RETURN_ON_FALSE(!(v & MAX30102_TEMP_EN), ESP_ERR_TIMEOUT, MAX30102_TAG, "Temp conversion did not finish");

    // La parte entera tiene signo (complemento a dos), la fraccion son 4 bits de 1/16 C
    uint8_t t[2];
    ESP_RETURN_ON_ERROR(readRegs(MAX30102_REG_DIE_TEMP_INT, t, sizeof(t)), MAX30102_TAG, "Temp read failed");
    *out_c = (float)(int8_t)t[0] + (t[1] & 0x0F) * MAX30102_TEMP_FRAC_STEP;
    return ESP_OK;
}

esp_err_t MAX30102::writeReg(uint8_t reg, uint8_t value){
    // registro + valor
    uint8_t buffer[2] = {reg, value};

    return i2c_master_transmit(dev_, buffer, sizeof(buffer), MAX30102_I2C_TIMEOUT_MS);
}

esp_err_t MAX30102::readRegs(uint8_t reg, uint8_t *data, size_t len){
    // Escribe la direccion del registro (1 byte) y lee len bytes en data
    return i2c_master_transmit_receive(dev_, &reg,  1, data, len, MAX30102_I2C_TIMEOUT_MS);
}

esp_err_t MAX30102::readReg(uint8_t reg, uint8_t *value){
    return readRegs(reg, value, 1); // lee un solo byte
}

uint32_t MAX30102::build18Bits(uint8_t b1, uint8_t b2, uint8_t b3){
    return (((uint32_t)b1 << 16) | (b2 << 8) | b3 ) & MAX30102_ADC_MASK_18BITS;
}
