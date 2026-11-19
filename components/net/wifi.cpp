// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#include "wifi.hpp"

#include <string.h>

#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

static constexpr int RETRY_DELAY_MS = 5000; // espera entre intentos de conexion
static constexpr EventBits_t CONNECTED_BIT = BIT0;

static EventGroupHandle_t s_events = nullptr;
static TimerHandle_t s_retry_timer = nullptr;

static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND (wrong SSID, out of range, or a 5 GHz network)";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL (wrong password/user)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT (usually a wrong password)";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT (signal lost)";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD (security mode mismatch)";
    default: return "see esp_wifi_types_generic.h";
    }
}

static void retry_timer_cb(TimerHandle_t)
{
    ESP_LOGI(TAG, "Connecting to \"%s\"...", CONFIG_HUBDOT_WIFI_SSID);
    esp_wifi_connect();
}

static void event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retry_timer_cb(nullptr);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected, reason %u: %s. Retrying in %d s",
                 ev->reason, reason_str(ev->reason), RETRY_DELAY_MS / 1000);
        xTimerStart(s_retry_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected to \"%s\", IP " IPSTR "  Gateway " IPSTR, CONFIG_HUBDOT_WIFI_SSID,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    }
}

bool wifi_configured()
{
    return strlen(CONFIG_HUBDOT_WIFI_SSID) > 0;
}

bool wifi_wait_connected(int timeout_ms)
{
    if (s_events == nullptr) return false;
    return xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & CONNECTED_BIT;
}

void wifi_start(const char *hostname)
{
    if (!wifi_configured()) {
        ESP_LOGW(TAG, "WiFi disabled: set the SSID in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    // El driver del WiFi necesita NVS (datos de calibracion)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_DELAY_MS), pdFALSE, nullptr, retry_timer_cb);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr));

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_HUBDOT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // acepta la seguridad que use la red
#if !CONFIG_HUBDOT_WIFI_ENTERPRISE
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_HUBDOT_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

#if CONFIG_HUBDOT_WIFI_ENTERPRISE
    // WPA2-Enterprise PEAP/MSCHAPv2, sin validar el certificado del servidor
    const char *user = CONFIG_HUBDOT_WIFI_EAP_USERNAME;
    const char *pass = CONFIG_HUBDOT_WIFI_EAP_PASSWORD;
    ESP_ERROR_CHECK(esp_eap_client_set_identity((const unsigned char *)user, strlen(user)));
    ESP_ERROR_CHECK(esp_eap_client_set_username((const unsigned char *)user, strlen(user)));
    ESP_ERROR_CHECK(esp_eap_client_set_password((const unsigned char *)pass, strlen(pass)));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
    ESP_LOGI(TAG, "Mode: WPA2-Enterprise, user \"%s\"", user);
#else
    ESP_LOGI(TAG, "Mode: WPA2-Personal");
#endif

    ESP_ERROR_CHECK(esp_wifi_start()); // WIFI_EVENT_STA_START -> esp_wifi_connect()
}
