#include "mpu6050.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int RESET_WAIT_MS = 100;
static constexpr int MAX_FIFO_READ_SAMPLES = 32;

// Sample rate = 1 kHz / (1 + SMPLRT_DIV)
static constexpr uint8_t SMPLRT_DIV_100HZ = 9;

MPU6050::~MPU6050()
{
    release();
}

void MPU6050::release()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t MPU6050::configure()
{
    uint8_t v{0};
    ESP_RETURN_ON_ERROR(readRegs(MPU6050_REG_WHO_AM_I, &v, 1), MPU6050_TAG, "Could not read WHO_AM_I");
    // Many cheap modules carry a compatible clone with another ID: warn, but keep going
    if (v != MPU6050_EXPECTED_WHO_AM_I) {
        ESP_LOGW(MPU6050_TAG, "WHO_AM_I = 0x%02X (expected 0x%02X), it may be a clone", v, MPU6050_EXPECTED_WHO_AM_I);
    } else {
        ESP_LOGI(MPU6050_TAG, "MPU6050 detected, WHO_AM_I = 0x%02X", v);
    }

    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR1_DEVICE_RESET), MPU6050_TAG, "Reset write failed");
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS));
    ESP_RETURN_ON_ERROR(readRegs(MPU6050_REG_PWR_MGMT_1, &v, 1), MPU6050_TAG, "Reset poll failed");
    ESP_RETURN_ON_FALSE(!(v & MPU6050_PWR1_DEVICE_RESET), ESP_ERR_TIMEOUT, MPU6050_TAG, "Reset did not finish");

    // CLKSEL = 0 (internal oscillator, the gyro is off) and SLEEP = 0: it wakes up asleep after a reset
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR1_TEMP_DIS), MPU6050_TAG, "PWR_MGMT_1");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_PWR_MGMT_2, MPU6050_PWR2_STBY_GYRO), MPU6050_TAG, "PWR_MGMT_2");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_CONFIG, MPU6050_DLPF_44HZ), MPU6050_TAG, "CONFIG");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_SMPLRT_DIV, SMPLRT_DIV_100HZ), MPU6050_TAG, "SMPLRT_DIV");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_FS_16G), MPU6050_TAG, "ACCEL_CONFIG");

    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_FIFO_EN, MPU6050_FIFO_EN_ACCEL), MPU6050_TAG, "FIFO_EN");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_RESET), MPU6050_TAG, "FIFO reset");
    ESP_RETURN_ON_ERROR(writeReg(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_EN), MPU6050_TAG, "USER_CTRL");

    ESP_LOGI(MPU6050_TAG, "Configured: accelerometer at %d Hz, +-16 g", (int)SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t MPU6050::begin(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_ARG, MPU6050_TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(dev_ == nullptr, ESP_ERR_INVALID_STATE, MPU6050_TAG, "already started");

    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, MPU6050_ADDRESS, MPU6050_I2C_TIMEOUT_MS),
        MPU6050_TAG, "No ACK at 0x%02X", MPU6050_ADDRESS);

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = MPU6050_ADDRESS;
    dev_cfg.scl_speed_hz = MPU6050_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev_), MPU6050_TAG, "Could not add the device");

    esp_err_t err = configure();
    if (err != ESP_OK) {
        release();
    }
    return err;
}

int MPU6050::readSamples(Sample *out, int maxSamples)
{
    if (dev_ == nullptr || out == nullptr || maxSamples <= 0) return -1;

    uint8_t cnt[2];
    if (readRegs(MPU6050_REG_FIFO_COUNTH, cnt, sizeof(cnt)) != ESP_OK) return -1;
    const int bytes = (cnt[0] << 8) | cnt[1];

    if (bytes >= MPU6050_FIFO_FULL_BYTES) {
        ESP_LOGW(MPU6050_TAG, "FIFO full, samples were lost");
        if (writeReg(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_EN | MPU6050_USER_FIFO_RESET) != ESP_OK) return -1;
        return 0;
    }

    int available = bytes / MPU6050_BYTES_PER_SAMPLE;
    if (available == 0) return 0;
    if (available > maxSamples) available = maxSamples;
    if (available > MAX_FIFO_READ_SAMPLES) available = MAX_FIFO_READ_SAMPLES;

    uint8_t raw[MAX_FIFO_READ_SAMPLES * MPU6050_BYTES_PER_SAMPLE];
    if (readRegs(MPU6050_REG_FIFO_R_W, raw, (size_t)available * MPU6050_BYTES_PER_SAMPLE) != ESP_OK) return -1;

    for (int i = 0; i < available; i++) {
        const uint8_t *s = &raw[i * MPU6050_BYTES_PER_SAMPLE];
        out[i].x = (int16_t)((s[0] << 8) | s[1]);
        out[i].y = (int16_t)((s[2] << 8) | s[3]);
        out[i].z = (int16_t)((s[4] << 8) | s[5]);
    }
    return available;
}

esp_err_t MPU6050::writeReg(uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(dev_, buffer, sizeof(buffer), MPU6050_I2C_TIMEOUT_MS);
}

esp_err_t MPU6050::readRegs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_, &reg, 1, data, len, MPU6050_I2C_TIMEOUT_MS);
}
