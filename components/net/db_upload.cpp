// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#include "db_upload.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi.hpp"

static const char *TAG = "db_upload";

static constexpr int QUEUE_LEN = 16;
static constexpr int PATH_LEN = 160;  // "script.php?query"
static constexpr int URL_LEN = 256;
static constexpr int BODY_LEN = 128;  // parte de la respuesta que se guarda para imprimir errores de PHP
static constexpr int HTTP_TIMEOUT_MS = 5000;
static constexpr int NO_IP_WAIT_MS = 1000;
static constexpr int RETRY_DELAY_MS = 5000;
static constexpr int64_t IMPORTANT_MAX_AGE_US = 10LL * 60 * 1000 * 1000; // una caida se abandona despues de 10 min
static constexpr uint32_t TASK_STACK = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 1; // igual que app_main: comparte el CPU, nunca se lo quita

struct Item {
    int64_t queued_us;
    bool important;
    char path[PATH_LEN];
};

enum class Result { Saved, Retry, Drop };

static QueueHandle_t s_queue = nullptr;

struct Body {
    char text[BODY_LEN];
    int len;
};

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    Body *body = static_cast<Body *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && body != nullptr) {
        const int room = BODY_LEN - 1 - body->len;
        const int n = (evt->data_len < room) ? evt->data_len : room;
        if (n > 0) {
            memcpy(body->text + body->len, evt->data, n);
            body->len += n;
            body->text[body->len] = '\0';
        }
    }
    return ESP_OK;
}

static Result send_one(const Item &it)
{
    const long delay_s = (long)((esp_timer_get_time() - it.queued_us) / 1000000);
    char url[URL_LEN];
    const int len = snprintf(url, sizeof(url), "%s/%s&serie=%s&retraso=%ld", CONFIG_HUBDOT_DB_URL, it.path, board_serial(),
                             delay_s);
    if (len < 0 || len >= URL_LEN) {
        ESP_LOGE(TAG, "URL too long, dropped: %s", it.path);
        return Result::Drop;
    }

    Body body = {};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.event_handler = on_http_event;
    cfg.user_data = &body;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return Result::Retry;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        // Servidor apagado, IP equivocada o un firewall: puede funcionar despues
        ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
        return Result::Retry;
    }
    if (status == 200) {
        ESP_LOGI(TAG, "Saved: %s", it.path);
        return Result::Saved;
    }
    ESP_LOGW(TAG, "%s: HTTP %d %s", url, status, body.text);
    // 4xx = la peticion esta mal (falta un parametro, no existe el script): repetirla no sirve
    return (status >= 400 && status < 500) ? Result::Drop : Result::Retry;
}

static void upload_task(void *)
{
    Item it;
    while (true) {
        xQueueReceive(s_queue, &it, portMAX_DELAY);
        while (true) {
            const bool ip = wifi_wait_connected(0); // sin IP todas las peticiones fallan
            const Result res = ip ? send_one(it) : Result::Retry;
            if (res != Result::Retry) break;
            if (!it.important) {
                ESP_LOGD(TAG, "Not sent, dropped: %s", it.path);
                break;
            }
            if (esp_timer_get_time() - it.queued_us > IMPORTANT_MAX_AGE_US) {
                ESP_LOGE(TAG, "Could not be saved in 10 min, dropped: %s", it.path);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ip ? RETRY_DELAY_MS : NO_IP_WAIT_MS));
        }
    }
}

void db_upload_start()
{
    if (s_queue != nullptr) return;

    ESP_LOGI(TAG, "Register this board in Dispositivo with numSerie = %s", board_serial());

    if (!wifi_configured() || strlen(CONFIG_HUBDOT_DB_URL) == 0) {
        ESP_LOGW(TAG, "Upload disabled: set the SSID and the DB URL in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(Item));
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "Could not create the queue, nothing will be uploaded");
        return;
    }
    if (xTaskCreate(upload_task, "db_upload", TASK_STACK, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the task, nothing will be uploaded");
        vQueueDelete(s_queue);
        s_queue = nullptr;
    }
}

bool db_upload_send(const char *script, bool important, const char *query_fmt, ...)
{
    if (s_queue == nullptr) return false;

    Item it;
    it.queued_us = esp_timer_get_time();
    it.important = important;

    const int n = snprintf(it.path, sizeof(it.path), "%s?", script);
    if (n < 0 || n >= PATH_LEN) {
        ESP_LOGE(TAG, "Script name too long: %s", script);
        return false;
    }
    va_list args;
    va_start(args, query_fmt);
    const int m = vsnprintf(it.path + n, sizeof(it.path) - n, query_fmt, args);
    va_end(args);
    if (m < 0 || n + m >= PATH_LEN) {
        ESP_LOGE(TAG, "Query too long for %s", script);
        return false;
    }

    // Nunca esperar aqui: esto corre en el loop de los sensores
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropped: %s", it.path);
        return false;
    }
    return true;
}
