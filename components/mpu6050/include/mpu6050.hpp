// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef MPU6050_HPP
#define MPU6050_HPP

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MPU6050_TAG "[MPU6050]"

#define MPU6050_ADDRESS 0x68 // AD0 low
#define MPU6050_I2C_FREQ_HZ 100000
#define MPU6050_I2C_TIMEOUT_MS 100

// Register map (RM-MPU-6000A-00)
#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_FIFO_EN 0x23
#define MPU6050_REG_USER_CTRL 0x6A
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_PWR_MGMT_2 0x6C
#define MPU6050_REG_FIFO_COUNTH 0x72
#define MPU6050_REG_FIFO_R_W 0x74
#define MPU6050_REG_WHO_AM_I 0x75

#define MPU6050_EXPECTED_WHO_AM_I 0x68

#define MPU6050_PWR1_DEVICE_RESET (1 << 7)
#define MPU6050_PWR1_TEMP_DIS (1 << 3)    // the die temperature is not used
#define MPU6050_PWR2_STBY_GYRO 0x07       // the gyro draws far more than the accelerometer
#define MPU6050_DLPF_44HZ 0x03            // 1 kHz internal rate, 44 Hz accelerometer bandwidth
#define MPU6050_ACCEL_FS_16G (0x03 << 3)  // an impact can go past 8 g
#define MPU6050_FIFO_EN_ACCEL (1 << 3)
#define MPU6050_USER_FIFO_EN (1 << 6)
#define MPU6050_USER_FIFO_RESET (1 << 2)

#define MPU6050_BYTES_PER_SAMPLE 6        // X, Y, Z, 16 bits each
#define MPU6050_FIFO_FULL_BYTES 1020      // 170 whole samples of the 1024 byte FIFO

// Reads the accelerometer through the FIFO: the sensor keeps the timing, so a slow loop or a
// busy WiFi stack does not bend the signal
class MPU6050 {
public:
    static constexpr float SAMPLE_RATE_HZ = 100.0f;
    static constexpr float LSB_PER_G = 2048.0f; // at +-16 g

    // Raw accelerometer sample, LSB_PER_G counts per g
    struct Sample {
        int16_t x;
        int16_t y;
        int16_t z;
    };

    MPU6050() = default;
    ~MPU6050(); // removes the device from the bus

    // The object owns the bus device handle, so copying it makes no sense
    MPU6050(const MPU6050 &) = delete;
    MPU6050 &operator=(const MPU6050 &) = delete;

    // Adds the sensor to the I2C bus, resets it and starts sampling at SAMPLE_RATE_HZ
    esp_err_t begin(i2c_master_bus_handle_t bus);

    // Removes the device from the bus, so begin() can be called again
    void end() { release(); }

    // Reads the samples available in the FIFO (up to maxSamples).
    // Returns how many were read, or -1 on error. A full FIFO is emptied and counts as 0:
    // it no longer holds whole samples, so it cannot be trusted
    int readSamples(Sample *out, int maxSamples);

private:
    i2c_master_dev_handle_t dev_ = nullptr;

    esp_err_t configure();
    esp_err_t resetFifo(); // empties the FIFO and leaves it enabled
    void release();

    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readRegs(uint8_t reg, uint8_t *data, size_t len);
};

#endif
