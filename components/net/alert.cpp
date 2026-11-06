#include "alert.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "vitals.hpp"
#include "wifi.hpp"

static const char *TAG = "alert";

static constexpr int QUEUE_LEN = 8;
static constexpr int TASK_STACK = 8192;     // TLS needs it for https:// URLs
static constexpr int TASK_PRIORITY = 4;
static constexpr int HTTP_TIMEOUT_MS = 10000;
static constexpr int FIRST_RETRY_MS = 2000;
static constexpr int MAX_RETRY_MS = 30000;
static constexpr int WIFI_WAIT_MS = 5000;

struct Alert {
    uint32_t seq;
    int64_t time_us;
    float peak_g;
    float angle_deg;
};

static QueueHandle_t s_queue = nullptr;
static uint32_t s_seq = 0;
static uint32_t s_boot_id = 0; // seq restarts with the board, this keeps event ids unique
static char s_device[18];

static bool post(const char *json)
{
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_HUBDOT_ALERT_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Could not create the HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Server answered %d", status);
        return false;
    }
    return true;
}

static void deliver(const Alert &a)
{
    char event_id[24];
    snprintf(event_id, sizeof(event_id), "%08lx-%lu", (unsigned long)s_boot_id, (unsigned long)a.seq);

    int retry_ms = FIRST_RETRY_MS;
    while (true) {
        if (wifi_wait_connected(WIFI_WAIT_MS)) {
            // Built on every attempt, age_ms has to be right when it finally goes out
            const long age_ms = (long)((esp_timer_get_time() - a.time_us) / 1000);
            char json[192];
            fall_alert_to_json(json, sizeof(json), s_device, event_id, age_ms, a.peak_g, a.angle_deg);
            if (post(json)) {
                ESP_LOGI(TAG, "Alert %s delivered, %ld ms after the fall", event_id, age_ms);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > MAX_RETRY_MS) ? MAX_RETRY_MS : retry_ms * 2;
        } else {
            ESP_LOGW(TAG, "No network, alert %s still pending", event_id);
        }
    }
}

static void alert_task(void *)
{
    Alert a;
    while (true) {
        if (xQueueReceive(s_queue, &a, portMAX_DELAY) == pdTRUE) {
            deliver(a);
        }
    }
}

void alert_start()
{
    if (!wifi_configured() || strlen(CONFIG_HUBDOT_ALERT_URL) == 0) {
        ESP_LOGW(TAG, "Alerts disabled: set the SSID and the alert URL in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device, sizeof(s_device), MACSTR, MAC2STR(mac));
    s_boot_id = esp_random();

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(Alert));
    xTaskCreate(alert_task, "alert", TASK_STACK, nullptr, TASK_PRIORITY, nullptr);
    ESP_LOGI(TAG, "Fall alerts go to %s", CONFIG_HUBDOT_ALERT_URL);
}

void alert_fall(float peak_g, float angle_deg)
{
    char peak[16];
    fmt_centi(peak, sizeof(peak), peak_g);
    ESP_LOGW(TAG, "FALL: peak %s g, posture changed %d degrees", peak, (int)lroundf(angle_deg));

    if (s_queue == nullptr) return;

    const Alert a = {++s_seq, esp_timer_get_time(), peak_g, angle_deg};
    if (xQueueSend(s_queue, &a, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Alert queue full, this alert is lost");
    }
}
