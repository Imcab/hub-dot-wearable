// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca
//
// HUB: placa de la muñeca. Cada 100 ms:
//   MAX30102 (LEDs rojo + IR) -> PulseOximeter -> BPM y SpO2
// Una vez por segundo imprime el resultado. Cada 15 s sube el SpO2 valido a insertarOximetria.php,
// y cada 5 s publica el payload JSON (spo2, hr, alarma) por MQTT en reto2026/<equipo>/data.

#include <math.h>
#include <stdio.h>

#include "ArduinoJson.h"

#include "db_upload.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max30102.hpp"
#include "mqtt_pub.hpp"
#include "pulse_ox.hpp"
#include "vital_ranges.hpp"
#include "wifi.hpp"

static const char *TAG = "main";

static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_6; // D4
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_7; // D5
static constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0; // el C3 solo tiene un controlador I2C

static constexpr int READ_PERIOD_MS = 100; // el FIFO guarda 32 muestras, a 25 sps efectivas nunca se llena
static constexpr int MAX_CONSECUTIVE_ERRORS = 10; // 10 x 100 ms = 1 s fallando seguido -> reiniciar el sensor
static constexpr int RETRY_DELAY_MS = 1000; // espera entre intentos si begin() sigue fallando

// Muestreo efectivo = SPO2_SR / SMP_AVE (100 sps / 4 = 25 sps). Debe coincidir con cfg mas abajo
static constexpr float EFFECTIVE_SPS = 100.0f / 4.0f;
static constexpr int REPORT_EVERY_SAMPLES = 25; // imprime el resultado una vez por segundo
static constexpr bool PRINT_RAW = false; // true = imprime CSV "red,ir" (para el serial plotter) en vez del resultado

// Sensor.numSensor de esta placa en la base de datos (la placa se identifica por su MAC)
static constexpr int NUM_SENSOR_OXI = 1; // MAX30102
// El SpO2 se calcula cada segundo; una fila cada 15 s es suficiente para la base de datos
static constexpr int64_t UPLOAD_PERIOD_US = 15LL * 1000 * 1000;
static constexpr int64_t PUBLISH_PERIOD_US = 5LL * 1000 * 1000; // payload MQTT "cada pocos segundos"
static constexpr size_t PAYLOAD_LEN = 256;

// Arma el payload del esquema de datos (README -> Esquema de datos). Lo que todavia no es
// valido (sin dedo, o midiendo) va como null, no como 0: un 0 se graficaria como lectura real
static size_t build_payload(const PulseOximeter::Result &r, char *out, size_t len)
{
    JsonDocument doc;
    doc["device_id"] = mqtt_device_id();
    doc["serie"] = board_serial();            // Dispositivo.numSerie, para guardarlo en la BD
    doc["ts"] = mqtt_timestamp();             // Unix UTC (segundos)
    doc["ts_sync"] = mqtt_time_synced();      // false = ts son segundos desde el arranque
    doc["finger"] = r.finger;
    if (r.finger && r.spo2_valid) {
        doc["spo2"] = round(r.spo2 * 10.0) / 10.0; // %, un decimal
    } else {
        doc["spo2"] = nullptr;
    }
    if (r.finger && r.hr_valid) {
        doc["hr"] = (int)lroundf(r.bpm);           // lpm
    } else {
        doc["hr"] = nullptr;
    }

    doc["alarm"] = false;
    JsonArray reasons = doc["alarm_reasons"].to<JsonArray>();
    if (r.finger && r.spo2_valid && r.spo2 < vital::SPO2_MIN) reasons.add("spo2_low");
    if (r.finger && r.hr_valid && r.bpm < vital::HR_MIN) reasons.add("hr_low");
    if (r.finger && r.hr_valid && r.bpm > vital::HR_MAX) reasons.add("hr_high");
    doc["alarm"] = reasons.size() > 0;

    return serializeJson(doc, out, len);
}

// Reintenta hasta que el sensor responda (por ejemplo, un cable flojo que se vuelve a conectar).
// Nadie va a reiniciar una placa que trae puesta un paciente, asi que nunca se rinde
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
    // WiFi primero: no bloquea, y sin SSID configurado no hace nada
    wifi_start("hub-dot-hub");
    db_upload_start();
    mqtt_pub_start("hub");

    // Bus I2C
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
    uint64_t ir_sum = 0, red_sum = 0; // para el promedio IR/RED de cada segundo
    int64_t last_upload_us = -UPLOAD_PERIOD_US; // para que el primer resultado valido se mande de inmediato
    int64_t last_publish_us = -PUBLISH_PERIOD_US;
    while (true) {
        int n = sensor.readSamples(samples, MAX30102_FIFO_DEPTH);
        if (n < 0) {
            consecutive_errors++;
            if (consecutive_errors == 1) {
                ESP_LOGW(TAG, "readSamples failed"); // solo el primero, para no llenar el monitor
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
            const PulseOximeter::Result r = ox.result();
            const unsigned long ir = (unsigned long)(ir_sum / samples_since_report);
            const unsigned long red = (unsigned long)(red_sum / samples_since_report);
            samples_since_report = 0;
            ir_sum = red_sum = 0;

            // Solo un SpO2 valido con el dedo puesto. LecturaOxi no tiene columna para el pulso,
            // y la temperatura del MAX30102 es la del chip, no la del paciente
            const int64_t now_us = esp_timer_get_time();
            if (r.finger && r.spo2_valid && now_us - last_upload_us >= UPLOAD_PERIOD_US) {
                if (db_upload_send("insertarOximetria.php", false, "spo2=%d&numSensor=%d",
                                   (int)(r.spo2 + 0.5f), NUM_SENSOR_OXI)) {
                    last_upload_us = now_us;
                }
            }

            if (now_us - last_publish_us >= PUBLISH_PERIOD_US) {
                last_publish_us = now_us;
                char payload[PAYLOAD_LEN];
                const size_t len = build_payload(r, payload, sizeof(payload));
                if (!PRINT_RAW) printf("%s\n", payload); // evidencia del payload en el monitor serial
                mqtt_pub_publish(payload, len);
            }

            if (!PRINT_RAW) {
                char temp[16] = "--";
                char hr[16] = "--";
                char sp[16] = "--";
                float die_c = 0;
                if (sensor.readTemperature(&die_c) == ESP_OK) fmt_centi(temp, sizeof(temp), die_c);
                if (r.hr_valid) snprintf(hr, sizeof(hr), "%d", (int)(r.bpm + 0.5f));
                if (r.spo2_valid) snprintf(sp, sizeof(sp), "%d", (int)(r.spo2 + 0.5f));
                const char *note = !r.finger ? "  (no finger)"
                                 : (r.hr_valid && r.spo2_valid) ? "" : "  (measuring...)";
                printf("IR: %6lu | RED: %6lu | Temp: %s C | HR: %s bpm | SpO2: %s %%%s\n",
                       ir, red, temp, hr, sp, note);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}
