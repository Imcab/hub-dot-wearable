#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "max30102.hpp"
#include "pulse_ox.hpp"
#include "vitals.hpp"
#include "wifi.hpp"

static const char *TAG = "main";

static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_6; // D4
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_7; // D5
static constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0; // the C3 only has one I2C controller

static constexpr int READ_PERIOD_MS = 100; // FIFO holds 32 samples, at 25 effective sps it never overflows
static constexpr int MAX_CONSECUTIVE_ERRORS = 10; // 10 x 100 ms = 1 s failing in a row -> restart the sensor
static constexpr int RETRY_DELAY_MS = 1000; // wait between attempts if begin() keeps failing

// Effective sample rate = SPO2_SR / SMP_AVE (100 sps / 4 = 25 sps). Keep it in sync with cfg below
static constexpr float EFFECTIVE_SPS = 100.0f / 4.0f;
static constexpr int REPORT_EVERY_SAMPLES = 25; // print the result once per second
static constexpr bool PRINT_RAW = false; // true = print "red,ir" CSV (for the serial plotter) instead of the result

static SemaphoreHandle_t s_readings_mutex = nullptr;
static Readings s_readings = {};
static int64_t s_readings_us = -1; // when they were published, negative = never

// Stores a copy of the readings for the HTTP server, stamped with the current time
static void publish(const Readings &r)
{
    xSemaphoreTake(s_readings_mutex, portMAX_DELAY);
    s_readings = r;
    s_readings_us = esp_timer_get_time();
    xSemaphoreGive(s_readings_mutex);
}

static void readings_json(char *buf, size_t len)
{
    xSemaphoreTake(s_readings_mutex, portMAX_DELAY);
    const Readings r = s_readings;
    const int64_t taken_us = s_readings_us;
    xSemaphoreGive(s_readings_mutex);

    const long age_ms = (taken_us < 0) ? -1 : (long)((esp_timer_get_time() - taken_us) / 1000);
    readings_to_json(buf, len, r, age_ms);
}

// Keeps retrying until the sensor answers (e.g. a loose wire that gets reconnected).
// Nobody is there to reboot a device worn by a patient, so it never gives up
static void start_sensor(MAX30102 &sensor, i2c_master_bus_handle_t bus, const MAX30102::Config &cfg)
{
    esp_err_t err;
    while ((err = sensor.begin(bus, cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 begin failed: %s, retrying in %d ms", esp_err_to_name(err), RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}

extern "C" void app_main(void)
{
    // WiFi first: it does not block, and with no SSID configured it only prints the MAC
    s_readings_mutex = xSemaphoreCreateMutex();
    wifi_start("hub-dot-hub", readings_json);

    // A. Create the I2C bus
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

    static MAX30102 sensor;

    MAX30102::Config cfg = {};
    cfg.smp_ave = MAX30102_SMP_AVE_4;
    cfg.adc_range = MAX30102_ADC_RGE_16384NA;
    cfg.sample_rate = MAX30102_SR_100SPS;
    cfg.pulse_width = MAX30102_PW_411US;
    cfg.led_red_pa = MAX30102_LED_PA_FROM_MA(12.0);
    cfg.led_ir_pa = MAX30102_LED_PA_FROM_MA(12.0);
    cfg.fifo_a_full = 15;
    cfg.fifo_rollover = true;

    start_sensor(sensor, bus, cfg);

    static PulseOximeter ox(EFFECTIVE_SPS);

    MAX30102::Sample samples[MAX30102_FIFO_DEPTH];
    int consecutive_errors = 0;
    int samples_since_report = 0;
    uint64_t ir_sum = 0, red_sum = 0; // for the mean IR/RED of each second
    while (true) {
        int n = sensor.readSamples(samples, MAX30102_FIFO_DEPTH);
        if (n < 0) {
            consecutive_errors++;
            if (consecutive_errors == 1) {
                ESP_LOGW(TAG, "readSamples failed"); // only the first one, so the monitor is not flooded
            }
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "%d errors in a row, restarting the sensor", consecutive_errors);
                sensor.end();
                start_sensor(sensor, bus, cfg);
                ESP_LOGI(TAG, "Sensor restarted");
                ox.reset();
                consecutive_errors = 0;
            }
        } else {
            if (consecutive_errors > 0) {
                ESP_LOGI(TAG, "Recovered after %d failed reads", consecutive_errors);
            }
            consecutive_errors = 0;
        }
        for (int i = 0; i < n; i++) {
            ox.addSample(samples[i].red, samples[i].ir);
            ir_sum += samples[i].ir;
            red_sum += samples[i].red;
            if (PRINT_RAW) {
                printf("%lu,%lu\n", (unsigned long)samples[i].red, (unsigned long)samples[i].ir);
            }
        }

        samples_since_report += (n > 0) ? n : 0;
        if (samples_since_report >= REPORT_EVERY_SAMPLES) {
            Readings rd = {};
            PulseOximeter::Result r = ox.result();
            rd.finger = r.finger;
            rd.hr_valid = r.hr_valid;
            rd.spo2_valid = r.spo2_valid;
            rd.bpm = r.bpm;
            rd.spo2 = r.spo2;
            rd.ir = (uint32_t)(ir_sum / samples_since_report);
            rd.red = (uint32_t)(red_sum / samples_since_report);
            rd.temp_valid = (sensor.readTemperature(&rd.temp_c) == ESP_OK);
            samples_since_report = 0;
            ir_sum = red_sum = 0;
            publish(rd);

            if (!PRINT_RAW) {
                char temp[16] = "--";
                char hr[16] = "--";
                char sp[16] = "--";
                if (rd.temp_valid) fmt_centi(temp, sizeof(temp), rd.temp_c);
                if (r.hr_valid) snprintf(hr, sizeof(hr), "%d", (int)(r.bpm + 0.5f));
                if (r.spo2_valid) snprintf(sp, sizeof(sp), "%d", (int)(r.spo2 + 0.5f));
                const char *note = !r.finger ? "  (no finger)"
                                 : (r.hr_valid && r.spo2_valid) ? "" : "  (measuring...)";
                printf("IR: %6lu | RED: %6lu | Temp: %s C | HR: %s bpm | SpO2: %s %%%s\n",
                       (unsigned long)rd.ir, (unsigned long)rd.red, temp, hr, sp, note);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}
