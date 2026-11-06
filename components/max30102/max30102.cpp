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

    // Validate the config BEFORE touching the chip (A_FULL is a 4 bit field, 0 to 15)
    ESP_RETURN_ON_FALSE(cfg.fifo_a_full <= 15, ESP_ERR_INVALID_ARG, MAX30102_TAG,
        "fifo_a_full out of range: %u (max 15)", cfg.fifo_a_full);

    // First, check if it is a MAX30102
    uint8_t v{0};
    ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_PART_ID, &v), MAX30102_TAG, "Could not read PART_ID");

    ESP_RETURN_ON_FALSE(v == MAX30102_EXPECTED_PART_ID, ESP_ERR_NOT_FOUND, MAX30102_TAG,
        "Unexpected PART_ID: 0x%02X (expected 0x%02X)", v, MAX30102_EXPECTED_PART_ID
    );

    ESP_LOGI(MAX30102_TAG, "MAX30102 detected, PART_ID = 0x%02X", v);

    // Try a reset
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_RESET), MAX30102_TAG, "Reset write failed");

    // Now after the reset, we need to read that the reset bit turns into 0
    int tries = 0;
    do{
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_MODE_CONFIG, &v), MAX30102_TAG, "Reset poll failed");
        tries++;
    } while ((v & MAX30102_MODE_RESET) && tries < 100); // AND mask: looks only at bit 6
    ESP_RETURN_ON_FALSE(!(v & MAX30102_MODE_RESET), ESP_ERR_TIMEOUT, MAX30102_TAG, "Reset did not finish");

    // Clear the FIFO pointers (page 14)
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_WRITE_PTR, 0x00), MAX30102_TAG, "WR_PTR");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_OVERFLOW_COUNTER, 0x00), MAX30102_TAG, "OVF");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_READ_PTR, 0x00), MAX30102_TAG, "RD_PTR");

    // Configuration: the register bytes are built by OR-ing the fields
    // The ternary goes in parentheses: ?: has lower priority than |
    uint8_t fifo_cfg = cfg.smp_ave
                     | (cfg.fifo_rollover ? MAX30102_FIFO_ROLLOVER_ENABLE : 0)
                     | MAX30102_FIFO_A_FULL(cfg.fifo_a_full);
    uint8_t spo2_cfg = cfg.adc_range | cfg.sample_rate | cfg.pulse_width;

    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_FIFO_CONFIG, fifo_cfg), MAX30102_TAG, "FIFO_CONFIG");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_SPO2_CONFIG, spo2_cfg), MAX30102_TAG, "SPO2_CONFIG");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_LED1_PA, cfg.led_red_pa), MAX30102_TAG, "LED1_PA");
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_LED2_PA, cfg.led_ir_pa), MAX30102_TAG, "LED2_PA");

    // Now set to SPO2 mode for default
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_SPO2), MAX30102_TAG, "MODE_CONFIG");

    ESP_LOGI(MAX30102_TAG, "Configured: FIFO_CONFIG=0x%02X SPO2_CONFIG=0x%02X", fifo_cfg, spo2_cfg);

    return ESP_OK;
}

esp_err_t MAX30102::begin(i2c_master_bus_handle_t bus, const Config &cfg){

    // 1. Validate
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_ARG, MAX30102_TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(dev_ == nullptr, ESP_ERR_INVALID_STATE, MAX30102_TAG, "already started");

    // 2. Probe
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, MAX30102_ADDRESS, MAX30102_I2C_TIMEOUT_MS),
        MAX30102_TAG, "No ACK at 0x%02X", MAX30102_ADDRESS);

    // 3. Add the device
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = MAX30102_ADDRESS;
    dev_cfg.scl_speed_hz = MAX30102_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev_), MAX30102_TAG, "Could not add the device");

    // 4 and 5. Configure, and clean up if it failed
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
        // FIFO is full, not empty
        available = MAX30102_FIFO_DEPTH;
    }
    // Empty
    if (available == 0) return 0;
    
    // clamp the output
    if (available > maxSamples) {
        available = maxSamples;
    }

    uint8_t raw[MAX30102_FIFO_DEPTH * MAX30102_BYTES_PER_SAMPLE]; //192 bytes
    size_t total =  (size_t)available * MAX30102_BYTES_PER_SAMPLE;

    if (readRegs(MAX30102_REG_FIFO_DATA, raw, total) != ESP_OK) return -1;

    for(int i = 0; i < available; i++){
        //start
        const uint8_t *s = &raw[i * MAX30102_BYTES_PER_SAMPLE];
        // bytes 1-3 led1 (RED)
        out[i].red = build18Bits(s[0], s[1], s[2]);
        //bytes 4-6  led2 (IR)
        out[i].ir  = build18Bits(s[3], s[4], s[5]);
    }

    return available;
}

esp_err_t MAX30102::readTemperature(float *out_c){
    ESP_RETURN_ON_FALSE(dev_ != nullptr && out_c != nullptr, ESP_ERR_INVALID_STATE, MAX30102_TAG, "not started");

    // Start one conversion. It takes ~29 ms (page 4), so wait that long before the first check:
    // polling earlier can see TEMP_EN still at 0 and read an old (or empty) result
    ESP_RETURN_ON_ERROR(writeReg(MAX30102_REG_DIE_TEMP_CONFIG, MAX30102_TEMP_EN), MAX30102_TAG, "TEMP_EN");
    vTaskDelay(pdMS_TO_TICKS(30));

    // TEMP_EN goes back to 0 when the result is ready
    uint8_t v{0};
    int tries = 0;
    while (true) {
        ESP_RETURN_ON_ERROR(readReg(MAX30102_REG_DIE_TEMP_CONFIG, &v), MAX30102_TAG, "Temp poll failed");
        if (!(v & MAX30102_TEMP_EN) || ++tries >= 14) break; // 30 + 14 x 5 ms = 100 ms max
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_RETURN_ON_FALSE(!(v & MAX30102_TEMP_EN), ESP_ERR_TIMEOUT, MAX30102_TAG, "Temp conversion did not finish");

    // Integer part is signed (two's complement), fraction is 4 bits of 1/16 C
    uint8_t t[2];
    ESP_RETURN_ON_ERROR(readRegs(MAX30102_REG_DIE_TEMP_INT, t, sizeof(t)), MAX30102_TAG, "Temp read failed");
    *out_c = (float)(int8_t)t[0] + (t[1] & 0x0F) * MAX30102_TEMP_FRAC_STEP;
    return ESP_OK;
}

esp_err_t MAX30102::writeReg(uint8_t reg, uint8_t value){
    //REGISTER + VALUE
    uint8_t buffer[2] = {reg, value};

    // Writes a Register, pass the device, the buffer (message), the size (2) and the timeout
    return i2c_master_transmit(dev_, buffer, sizeof(buffer), MAX30102_I2C_TIMEOUT_MS);
}

esp_err_t MAX30102::readRegs(uint8_t reg, uint8_t *data, size_t len){
    // Reads regions, pass the devide, the adress of the region, the size of the bytes you write before reading (The register adress)
    // now data is where it writes the value, and len how many bytes we read
    return i2c_master_transmit_receive(dev_, &reg,  1, data, len, MAX30102_I2C_TIMEOUT_MS);
}

esp_err_t MAX30102::readReg(uint8_t reg, uint8_t *value){
    return readRegs(reg, value, 1); // reads one byte (8bits) of a register and stores it on value
}

uint32_t MAX30102::build18Bits(uint8_t b1, uint8_t b2, uint8_t b3){
    return (((uint32_t)b1 << 16) | (b2 << 8) | b3 ) & MAX30102_ADC_MASK_18BITS;
}
