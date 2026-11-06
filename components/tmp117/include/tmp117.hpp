// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef TMP117_HPP
#define TMP117_HPP

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define TMP117_TAG "[TMP117]"

#define TMP117_ADDRESS 0x48 // ADD0 to GND
#define TMP117_I2C_FREQ_HZ 100000
#define TMP117_I2C_TIMEOUT_MS 100

// Registers (16 bits, MSB first)
#define TMP117_REG_TEMP_RESULT 0x00
#define TMP117_REG_CONFIG 0x01
#define TMP117_REG_DEVICE_ID 0x0F

#define TMP117_DEVICE_ID 0x0117
#define TMP117_DEVICE_ID_MASK 0x0FFF // bits 15:12 are the revision

// CONFIG register
#define TMP117_CFG_DATA_READY (1 << 13)
#define TMP117_CFG_MOD_SHUTDOWN (0x01 << 10)
#define TMP117_CFG_MOD_ONE_SHOT (0x03 << 10)
#define TMP117_CFG_AVG_8 (0x01 << 5)  // 8 averaged conversions, about 125 ms
#define TMP117_CFG_SOFT_RESET (1 << 1)

#define TMP117_RESOLUTION_C 0.0078125f // 1/128 C per count
#define TMP117_RESULT_INVALID 0x8000   // power-on value, no conversion has finished

// Stays in shutdown and only converts when asked: it spends almost nothing between readings
class TMP117 {
public:
    TMP117() = default;
    ~TMP117(); // removes the device from the bus

    // The object owns the bus device handle, so copying it makes no sense
    TMP117(const TMP117 &) = delete;
    TMP117 &operator=(const TMP117 &) = delete;

    // Adds the sensor to the I2C bus, checks DEVICE_ID, resets it and leaves it in shutdown
    esp_err_t begin(i2c_master_bus_handle_t bus);

    // Removes the device from the bus, so begin() can be called again
    void end() { release(); }

    bool started() const { return dev_ != nullptr; }

    // Runs one conversion and returns the temperature in Celsius. Blocks about 150 ms
    esp_err_t readOneShot(float *out_c);

private:
    i2c_master_dev_handle_t dev_ = nullptr;

    esp_err_t configure();
    void release();

    esp_err_t writeReg(uint8_t reg, uint16_t value);
    esp_err_t readReg(uint8_t reg, uint16_t *value);
};

#endif
