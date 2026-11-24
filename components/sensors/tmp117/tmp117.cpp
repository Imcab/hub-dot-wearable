#include "tmp117.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int RESET_WAIT_MS = 10;       // la hoja de datos pide 2 ms, un tick son 10 ms
static constexpr uint16_t CFG_SHUTDOWN = TMP117_CFG_MOD_SHUTDOWN | TMP117_CFG_AVG_8 | TMP117_CFG_DR_ALERT |
                                        TMP117_CFG_POL_ACTIVE_LOW;
static constexpr uint16_t CFG_ONE_SHOT = TMP117_CFG_MOD_ONE_SHOT | TMP117_CFG_AVG_8 | TMP117_CFG_DR_ALERT |
                                        TMP117_CFG_POL_ACTIVE_LOW;

TMP117::~TMP117()
{
    release();
}

void TMP117::release()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t TMP117::configure()
{
    uint16_t v{0};
    ESP_RETURN_ON_ERROR(readReg(TMP117_REG_DEVICE_ID, &v), TMP117_TAG, "Could not read DEVICE_ID");
    ESP_RETURN_ON_FALSE((v & TMP117_DEVICE_ID_MASK) == TMP117_DEVICE_ID, ESP_ERR_NOT_FOUND, TMP117_TAG,
        "Unexpected DEVICE_ID: 0x%04X", v);
    ESP_LOGI(TMP117_TAG, "TMP117 detected, DEVICE_ID = 0x%04X", v);

    ESP_RETURN_ON_ERROR(writeReg(TMP117_REG_CONFIG, TMP117_CFG_SOFT_RESET), TMP117_TAG, "Reset write failed");
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS));
    ESP_RETURN_ON_ERROR(writeReg(TMP117_REG_CONFIG, CFG_SHUTDOWN), TMP117_TAG, "CONFIG");
    converting_ = false;
    return ESP_OK;
}

esp_err_t TMP117::begin(i2c_master_bus_handle_t bus, gpio_num_t alert_pin)
{
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_ARG, TMP117_TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(dev_ == nullptr, ESP_ERR_INVALID_STATE, TMP117_TAG, "already started");

    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TMP117_ADDRESS, TMP117_I2C_TIMEOUT_MS),
        TMP117_TAG, "No ACK at 0x%02X", TMP117_ADDRESS);

    alert_pin_ = alert_pin;
    if (alert_pin_ != GPIO_NUM_NC) {
        // Open-drain: necesita pull-up. El interno basta para una senal que cambia cada 30 s
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << alert_pin_;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&io), TMP117_TAG, "ALERT pin config failed");
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TMP117_ADDRESS;
    dev_cfg.scl_speed_hz = TMP117_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev_), TMP117_TAG, "Could not add the device");

    esp_err_t err = configure();
    if (err != ESP_OK) {
        release();
    }
    return err;
}

esp_err_t TMP117::startOneShot()
{
    ESP_RETURN_ON_FALSE(dev_ != nullptr, ESP_ERR_INVALID_STATE, TMP117_TAG, "not started");
    ESP_RETURN_ON_ERROR(writeReg(TMP117_REG_CONFIG, CFG_ONE_SHOT), TMP117_TAG, "One-shot");
    converting_ = true;
    start_us_ = esp_timer_get_time();
    return ESP_OK;
}

bool TMP117::conversionDone() const
{
    if (!converting_) return false;
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us_) / 1000;
    if (alert_pin_ == GPIO_NUM_NC) return elapsed_ms >= CONVERSION_MS;
    return gpio_get_level(alert_pin_) == 0 || elapsed_ms >= ALERT_TIMEOUT_MS;
}

esp_err_t TMP117::readResult(float *out_c)
{
    ESP_RETURN_ON_FALSE(dev_ != nullptr && out_c != nullptr && converting_, ESP_ERR_INVALID_STATE, TMP117_TAG,
                        "no conversion in progress");
    converting_ = false;
    const bool pin_low = alert_pin_ != GPIO_NUM_NC && gpio_get_level(alert_pin_) == 0;

    // Leer CONFIG borra Data_Ready y suelta el pin ALERT
    uint16_t cfg{0};
    ESP_RETURN_ON_ERROR(readReg(TMP117_REG_CONFIG, &cfg), TMP117_TAG, "CONFIG read failed");
    ESP_RETURN_ON_FALSE(cfg & TMP117_CFG_DATA_READY, ESP_ERR_TIMEOUT, TMP117_TAG, "Conversion did not finish");
    if (alert_pin_ != GPIO_NUM_NC && !pin_low && !alert_warned_) {
        alert_warned_ = true; // la conversion termino pero el pin nunca bajo
        ESP_LOGW(TMP117_TAG, "ALERT pin (GPIO %d) did not go low, check the wire. Reading over I2C instead",
                 alert_pin_);
    }

    uint16_t raw{0};
    ESP_RETURN_ON_ERROR(readReg(TMP117_REG_TEMP_RESULT, &raw), TMP117_TAG, "Read failed");
    ESP_RETURN_ON_FALSE(raw != TMP117_RESULT_INVALID, ESP_ERR_INVALID_RESPONSE, TMP117_TAG, "No valid result");

    *out_c = (float)(int16_t)raw * TMP117_RESOLUTION_C;
    return ESP_OK;
}

esp_err_t TMP117::writeReg(uint8_t reg, uint16_t value)
{
    uint8_t buffer[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(dev_, buffer, sizeof(buffer), TMP117_I2C_TIMEOUT_MS);
}

esp_err_t TMP117::readReg(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, data, sizeof(data), TMP117_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = (uint16_t)((data[0] << 8) | data[1]);
    }
    return err;
}
