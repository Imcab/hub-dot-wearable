// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca
//
// Driver del MAX30102 (oximetro de la muñeca, placa hub). Solo entrega las lecturas crudas
// de los LEDs rojo e IR: el pulso y el SpO2 los calcula PulseOximeter (algorithms/pulse_ox)

#ifndef MAX30102_HPP
#define MAX30102_HPP

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MAX30102_TAG "[MAX30102]" // El tag que sale en la telemetria de ESPLOGI (Log information)
// Ejemplo: I (312) [MAX30102]: MAX30102 detected, PART_ID = 0x15

#define MAX30102_ADDRESS 0x57 // La dirección I2C del MAX, convertido de la pagina 29 de la datasheet

// I2C configuraciones por defecto (los pines se escogen en el cpp cuando se cree el sensor)
#define MAX30102_I2C_FREQ_HZ 100000   // 100 kHz
#define MAX30102_I2C_TIMEOUT_MS 100

// Mapa de registros, paginas 10-11
#define MAX30102_REG_INT_STATUS_1 0x00
#define MAX30102_REG_INT_STATUS_2 0x01
#define MAX30102_REG_INT_ENABLE_1 0x02
#define MAX30102_REG_INT_ENABLE_2 0x03
#define MAX30102_REG_FIFO_WRITE_PTR 0x04
#define MAX30102_REG_OVERFLOW_COUNTER 0x05
#define MAX30102_REG_FIFO_READ_PTR 0x06
#define MAX30102_REG_FIFO_DATA 0x07
#define MAX30102_REG_FIFO_CONFIG 0x08
#define MAX30102_REG_MODE_CONFIG 0x09
#define MAX30102_REG_SPO2_CONFIG 0x0A
#define MAX30102_REG_LED1_PA 0x0C   // LED rojo
#define MAX30102_REG_LED2_PA 0x0D   // LED infrarrojo
#define MAX30102_REG_DIE_TEMP_INT 0x1F
#define MAX30102_REG_DIE_TEMP_FRAC 0x20
#define MAX30102_REG_DIE_TEMP_CONFIG 0x21
#define MAX30102_REG_REV_ID 0xFE
#define MAX30102_REG_PART_ID 0xFF

#define MAX30102_EXPECTED_PART_ID 0x15

// DIE_TEMP_CONFIG (0x21), pagina 22: TEMP_EN inicia una conversion y se borra solo al terminar
#define MAX30102_TEMP_EN 0x01
#define MAX30102_TEMP_FRAC_STEP 0.0625f   // DIE_TEMP_FRAC bits 3:0, 1/16 C por paso

// MODE_CONFIG (0x09), pagina 18
#define MAX30102_MODE_RESET (1 << 6)
#define MAX30102_MODE_SHUTDOWN (1 << 7)
#define MAX30102_MODE_HEARTRATE 0x02   // 010 [solo LED rojo]
#define MAX30102_MODE_SPO2 0x03   // 011 [LEDs rojo + IR]

// FIFO_CONFIG (0x08), pagina 17
// SMP_AVE bits 7:5 (Tabla 3)
#define MAX30102_SMP_AVE_1 (0x00 << 5)   // sin promediar
#define MAX30102_SMP_AVE_2 (0x01 << 5)
#define MAX30102_SMP_AVE_4 (0x02 << 5)
#define MAX30102_SMP_AVE_8 (0x03 << 5)
#define MAX30102_SMP_AVE_16 (0x04 << 5)
#define MAX30102_SMP_AVE_32 (0x05 << 5)
// FIFO_ROLLOVER_EN bit 4
#define MAX30102_FIFO_ROLLOVER_ENABLE (1 << 4)
// FIFO_A_FULL bits 3:0 (cualquier valor de 0x0 a 0xF): n = lugares libres cuando se dispara INT
#define MAX30102_FIFO_A_FULL(n) ((n) & 0x0F)

// SPO2_CONFIG (0x0A), paginas 18-19
// SPO2_ADC_RGE bits 6:5 (Tabla 5)
#define MAX30102_ADC_RGE_2048NA (0x00 << 5)
#define MAX30102_ADC_RGE_4096NA (0x01 << 5)
#define MAX30102_ADC_RGE_8192NA (0x02 << 5)
#define MAX30102_ADC_RGE_16384NA (0x03 << 5)
// SPO2_SR bits 4:2 (Tabla 6). Las combinaciones permitidas estan en la Tabla 11 (pagina 23)
#define MAX30102_SR_50SPS (0x00 << 2)
#define MAX30102_SR_100SPS (0x01 << 2)
#define MAX30102_SR_200SPS (0x02 << 2)
#define MAX30102_SR_400SPS (0x03 << 2)
#define MAX30102_SR_800SPS (0x04 << 2)
#define MAX30102_SR_1000SPS (0x05 << 2)
#define MAX30102_SR_1600SPS (0x06 << 2)
#define MAX30102_SR_3200SPS (0x07 << 2)
// LED_PW bits 1:0 (Tabla 7), tambien fija la resolucion del ADC
#define MAX30102_PW_69US 0x00   // 15 bit
#define MAX30102_PW_118US 0x01   // 16 bit
#define MAX30102_PW_215US 0x02   // 17 bit
#define MAX30102_PW_411US 0x03   // 18 bit

// Datos del FIFO, paginas 14-15
#define MAX30102_FIFO_DEPTH 32
#define MAX30102_BYTES_PER_SAMPLE 6      // 3 red + 3 IR (SpO2 mode)
#define MAX30102_ADC_MASK_18BITS 0x3FFFF

// Corriente de los LEDs, pagina 20 (0.2 mA por paso, maximo ~51 mA)
// Usalo con constantes: MAX30102_LED_PA_FROM_MA(6.2) se calcula al compilar
#define MAX30102_LED_PA_FROM_MA(ma) ((uint8_t)((ma) * 5.0f + 0.5f))

class MAX30102 {
public:
    // Configuracion del sensor
    struct Config {
        uint8_t smp_ave; // MAX30102_SMP_AVE_x
        uint8_t adc_range; // MAX30102_ADC_RGE_x
        uint8_t sample_rate; // MAX30102_SR_x
        uint8_t pulse_width; // MAX30102_PW_x
        uint8_t led_red_pa; // MAX30102_LED_PA_FROM_MA(...)
        uint8_t led_ir_pa; // MAX30102_LED_PA_FROM_MA(...)
        uint8_t fifo_a_full;
        bool fifo_rollover;
    };
 
    // Una muestra: valor de 18 bits de cada LED
    struct Sample {
        uint32_t red;
        uint32_t ir;
    };
 
    MAX30102() = default;
    ~MAX30102(); // quita el dispositivo del bus
 
    // El objeto es dueño del handle del bus, copiarlo no tiene sentido
    MAX30102(const MAX30102 &) = delete;
    MAX30102 &operator=(const MAX30102 &) = delete;
 
    // Agrega el sensor al bus I2C, revisa PART_ID, lo resetea y lo configura (modo SpO2)
    esp_err_t begin(i2c_master_bus_handle_t bus, const Config &cfg);

    // Quita el dispositivo del bus, para poder llamar begin() otra vez (por ejemplo, tras errores)
    void end() { release(); }

    // Lee las muestras que haya en el FIFO (hasta maxSamples).
    // Devuelve cuantas leyo, o -1 si hubo error
    int readSamples(Sample *out, int maxSamples);

    // Lee la temperatura del chip en Celsius (pagina 22). Bloquea ~30 ms mientras convierte
    esp_err_t readTemperature(float *out_c);
 
private:
    i2c_master_dev_handle_t dev_ = nullptr; // el _ al final = miembro privado
 
    esp_err_t configure(const Config &cfg); // revisa PART_ID, resetea y escribe los registros
    void release(); // quita el dispositivo del bus
 
    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readRegs(uint8_t reg, uint8_t *data, size_t len);
    esp_err_t readReg(uint8_t reg, uint8_t *value);
 
    static uint32_t build18Bits(uint8_t b1, uint8_t b2, uint8_t b3);
};

#endif