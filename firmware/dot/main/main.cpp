#include <stdio.h>

#include "alert.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fall_detect.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpu6050.hpp"
#include "tmp117.hpp"
#include "vitals.hpp"
#include "wifi.hpp"

static const char *TAG = "main";

static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_6; // D4
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_7; // D5
static constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0; // the C3 only has one I2C controller

static constexpr int LOOP_PERIOD_MS = 100;       // the MPU FIFO holds 1.7 s, the longest stop here is a TMP117 reading
static constexpr int SENSOR_RETRY_MS = 1000;     // wait between attempts to start a missing sensor
static constexpr int MAX_CONSECUTIVE_ERRORS = 10;
static constexpr int MOTION_SILENCE_MS = 1000;   // no samples for this long -> restart the MPU6050
static constexpr int TEMP_PERIOD_MS = 30000;
static constexpr int READ_CHUNK = 32;
static constexpr bool PRINT_RAW = false; // true = print "ms,x,y,z" CSV to record movements, 2048 counts per g

static SemaphoreHandle_t s_mutex = nullptr;
static DotReadings s_readings = {};
static int64_t s_motion_us = -1; // last sample from the accelerometer, negative = never
static int64_t s_temp_us = -1;   // last good temperature, negative = never

static long age_ms(int64_t since_us)
{
    return (since_us < 0) ? -1 : (long)((esp_timer_get_time() - since_us) / 1000);
}

static void readings_json(char *buf, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const DotReadings r = s_readings;
    const int64_t motion_us = s_motion_us;
    const int64_t temp_us = s_temp_us;
    xSemaphoreGive(s_mutex);

    dot_readings_to_json(buf, len, r, age_ms(motion_us), age_ms(temp_us));
}

// ---------- Falls: this comes first, nothing else may stop it ----------

static MPU6050 s_mpu;
static FallDetector s_detector(MPU6050::SAMPLE_RATE_HZ);
static bool s_mpu_started = false;
static int s_mpu_errors = 0;
static int64_t s_mpu_retry_ms = 0;
static int64_t s_last_sample_ms = 0;
static uint32_t s_sample_count = 0;

static void restart_mpu()
{
    s_mpu.end();
    s_mpu_started = false;
    s_mpu_errors = 0;
    s_detector.reset();
}

static void on_samples(const MPU6050::Sample *samples, int n)
{
    for (int i = 0; i < n; i++) {
        const MPU6050::Sample &s = samples[i];
        if (PRINT_RAW) {
            printf("%lu,%d,%d,%d\n", (unsigned long)(s_sample_count * 10), s.x, s.y, s.z);
        }
        s_sample_count++;

        const FallDetector::Event ev = s_detector.addSample(s.x / MPU6050::LSB_PER_G, s.y / MPU6050::LSB_PER_G,
                                                            s.z / MPU6050::LSB_PER_G);
        if (ev == FallDetector::Event::Impact) {
            ESP_LOGI(TAG, "Impact, watching the posture");
        } else if (ev == FallDetector::Event::Cancelled) {
            ESP_LOGI(TAG, "Not a fall: posture changed %d degrees", (int)s_detector.report().angle_deg);
        } else if (ev == FallDetector::Event::Fall) {
            const FallDetector::Report rep = s_detector.report();
            alert_fall(rep.peak_g, rep.angle_deg);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_readings.falls++;
            xSemaphoreGive(s_mutex);
        }
    }
}

static void service_motion(i2c_master_bus_handle_t bus, int64_t now_ms)
{
    if (!s_mpu_started) {
        if (now_ms < s_mpu_retry_ms) return;
        s_mpu_retry_ms = now_ms + SENSOR_RETRY_MS;
        const esp_err_t err = s_mpu.begin(bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MPU6050 begin failed: %s, FALLS ARE NOT BEING DETECTED", esp_err_to_name(err));
            return;
        }
        s_mpu_started = true;
        s_last_sample_ms = now_ms;
    }

    MPU6050::Sample samples[READ_CHUNK];
    int n;
    do {
        n = s_mpu.readSamples(samples, READ_CHUNK);
        if (n < 0) {
            if (++s_mpu_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "%d errors in a row, restarting the MPU6050", s_mpu_errors);
                restart_mpu();
            }
            return;
        }
        s_mpu_errors = 0;
        if (n > 0) {
            on_samples(samples, n);
            s_last_sample_ms = now_ms;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_motion_us = esp_timer_get_time();
            xSemaphoreGive(s_mutex);
        }
    } while (n == READ_CHUNK);

    // The bus answers but nothing arrives: a brownout leaves the sensor asleep with no error at all
    if (now_ms - s_last_sample_ms > MOTION_SILENCE_MS) {
        ESP_LOGE(TAG, "No samples for %d ms, restarting the MPU6050", MOTION_SILENCE_MS);
        restart_mpu();
    }
}

// ---------- Temperature ----------

static TMP117 s_tmp;
static AxillaryTemp s_axillary;
static int64_t s_next_temp_ms = 0;

static void service_temp(i2c_master_bus_handle_t bus, int64_t now_ms)
{
    if (now_ms < s_next_temp_ms) return;

    if (!s_tmp.started()) {
        const esp_err_t err = s_tmp.begin(bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TMP117 begin failed: %s", esp_err_to_name(err));
            s_next_temp_ms = now_ms + SENSOR_RETRY_MS;
            return;
        }
    }
    s_next_temp_ms = now_ms + TEMP_PERIOD_MS;

    float c = 0;
    const esp_err_t err = s_tmp.readOneShot(&c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TMP117 read failed: %s, restarting it", esp_err_to_name(err));
        s_tmp.end();
        s_axillary.reset();
        s_next_temp_ms = now_ms + SENSOR_RETRY_MS;
        return;
    }

    s_axillary.add(c);
    const bool stable = s_axillary.stable();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readings.temp_c = c;
    s_readings.temp_stable = stable;
    s_temp_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);

    if (!PRINT_RAW) {
        char temp[16];
        fmt_centi(temp, sizeof(temp), c);
        printf("Temp: %s C%s\n", temp, stable ? "" : "  (not stable yet)");
    }
}

extern "C" void app_main(void)
{
    s_mutex = xSemaphoreCreateMutex();

    // WiFi first: it does not block, and with no SSID configured it only prints the MAC
    wifi_start("hub-dot-dot", readings_json);
    alert_start();

    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port = I2C_PORT;
    bus_cfg.sda_io_num = I2C_SDA_PIN;
    bus_cfg.scl_io_num = I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not create the I2C bus: %s", esp_err_to_name(err));
        return;
    }

    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        service_motion(bus, now_ms);
        service_temp(bus, now_ms);
        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}
