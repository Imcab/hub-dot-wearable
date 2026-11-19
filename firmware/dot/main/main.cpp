// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca
//
// DOT: placa de la axila. Cada 100 ms:
//   MPU6050 (acelerometro) -> FallDetector -> si hay caida: registrarCaida.php
//   TMP117 (cada 30 s)     -> AxillaryTemp -> si ya es estable: insertarTemperatura.php
// Las caidas van primero: nada de lo demas (temperatura, red) puede detener su lectura.

#include <stdio.h>

#include "axillary_temp.hpp"
#include "db_upload.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fall_detect.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mpu6050.hpp"
#include "tmp117.hpp"
#include "wifi.hpp"

static const char *TAG = "main";

static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_6; // D4
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_7; // D5
static constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0; // el C3 solo tiene un controlador I2C

static constexpr int LOOP_PERIOD_MS = 100;       // el FIFO del MPU aguanta 1.7 s, lo que mas tarda aqui es leer el TMP117
static constexpr int SENSOR_RETRY_MS = 1000;     // espera entre intentos de arrancar un sensor que no responde
static constexpr int MAX_CONSECUTIVE_ERRORS = 10;
static constexpr int MOTION_SILENCE_MS = 1000;   // sin muestras por este tiempo -> reiniciar el MPU6050
static constexpr int TEMP_PERIOD_MS = 30000;
static constexpr int READ_CHUNK = 32;
static constexpr bool PRINT_RAW = false; // true = imprime CSV "ms,x,y,z" para grabar movimientos, 2048 cuentas por g

// Sensor.numSensor de esta placa en la base de datos (la placa se identifica por su MAC)
static constexpr int NUM_SENSOR_TEMP = 1;   // TMP117
static constexpr int NUM_SENSOR_MOTION = 2; // MPU6050

// ---------- Caidas: van primero, nada mas puede detenerlas ----------

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
            char peak[16];
            fmt_centi(peak, sizeof(peak), rep.peak_g);
            ESP_LOGW(TAG, "FALL: peak %s g, posture changed %d degrees", peak, (int)lroundf(rep.angle_deg));

            // Una sola peticion guarda la Alerta (pico en g) y la LecturaMov con esta muestra, la
            // postura despues de la caida. Ejes en mg para no formatear floats negativos. Solo la
            // encola, la peticion HTTP la hace la tarea de db_upload
            db_upload_send("registrarCaida.php", true, "pico=%s&ax=%d&ay=%d&az=%d&numSensor=%d", peak,
                           (int)(s.x * 1000 / MPU6050::LSB_PER_G), (int)(s.y * 1000 / MPU6050::LSB_PER_G),
                           (int)(s.z * 1000 / MPU6050::LSB_PER_G), NUM_SENSOR_MOTION);
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
        }
    } while (n == READ_CHUNK);

    // El bus responde pero no llega nada: una baja de voltaje deja al sensor dormido sin ningun error
    if (now_ms - s_last_sample_ms > MOTION_SILENCE_MS) {
        ESP_LOGE(TAG, "No samples for %d ms, restarting the MPU6050", MOTION_SILENCE_MS);
        restart_mpu();
    }
}

// ---------- Temperatura ----------

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

    char temp[16];
    fmt_centi(temp, sizeof(temp), c);
    if (!PRINT_RAW) {
        printf("Temp: %s C%s\n", temp, stable ? "" : "  (not stable yet)");
    }

    // Hasta que la lectura se estabiliza es la piel calentandose, no la temperatura corporal
    if (stable) {
        db_upload_send("insertarTemperatura.php", false, "tempC=%s&numSensor=%d", temp, NUM_SENSOR_TEMP);
    }
}

extern "C" void app_main(void)
{
    // WiFi primero: no bloquea, y sin SSID configurado no hace nada
    wifi_start("hub-dot-dot");
    db_upload_start();

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
