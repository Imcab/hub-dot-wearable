#include "wifi.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_eap_client.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

static const char *TAG = "net";

static constexpr int RETRY_DELAY_MS = 5000;  // wait between connection attempts
static constexpr int PING_COUNT = 5;

static constexpr int JSON_MAX_LEN = 192;
static constexpr EventBits_t CONNECTED_BIT = BIT0;

static JsonProvider s_provider = nullptr;
static EventGroupHandle_t s_events = nullptr;
static TimerHandle_t s_retry_timer = nullptr;
static esp_netif_t *s_netif = nullptr;

// ---------- HTTP server: GET / -> JSON from the provider ----------

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char json[JSON_MAX_LEN];
    s_provider(json, sizeof(json));

    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "HTTP GET / from client -> 200");
    return httpd_resp_sendstr(req, json);
}

static void start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the HTTP server");
        return;
    }
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_get_handler;
    httpd_register_uri_handler(server, &root);
    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
}

// ---------- Connectivity self-test: ping the gateway + HTTP GET ----------

struct PingStats {
    SemaphoreHandle_t done;
    uint32_t total_ms;
};

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq;
    uint8_t ttl;
    uint32_t elapsed_ms, size;
    ip_addr_t addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    static_cast<PingStats *>(args)->total_ms += elapsed_ms;
    ESP_LOGI(TAG, "%lu bytes from %s: icmp_seq=%u ttl=%u time=%lu ms",
             (unsigned long)size, ipaddr_ntoa(&addr), seq, ttl, (unsigned long)elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    ESP_LOGW(TAG, "icmp_seq=%u timeout", seq);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    PingStats *st = static_cast<PingStats *>(args);
    uint32_t sent, received;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    ESP_LOGI(TAG, "Ping: %lu sent, %lu received, %lu%% loss, avg %lu ms",
             (unsigned long)sent, (unsigned long)received,
             (unsigned long)(sent ? (sent - received) * 100 / sent : 0),
             (unsigned long)(received ? st->total_ms / received : 0));
    xSemaphoreGive(st->done);
}

static void ping_gateway(const esp_ip4_addr_t &gw)
{
    PingStats stats = {xSemaphoreCreateBinary(), 0};

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type = IPADDR_TYPE_V4;
    ip4_addr_set_u32(ip_2_ip4(&cfg.target_addr), gw.addr);
    cfg.count = PING_COUNT;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = &stats;
    cbs.on_ping_success = on_ping_success;
    cbs.on_ping_timeout = on_ping_timeout;
    cbs.on_ping_end = on_ping_end;

    esp_ping_handle_t ping = nullptr;
    ESP_LOGI(TAG, "PING " IPSTR " (gateway), %d packets", IP2STR(&gw), PING_COUNT);
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        esp_ping_start(ping);
        xSemaphoreTake(stats.done, portMAX_DELAY);
        esp_ping_delete_session(ping);
    } else {
        ESP_LOGE(TAG, "Could not create the ping session");
    }
    vSemaphoreDelete(stats.done);
}

static void http_get_test()
{
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_HUBDOT_CONNECTIVITY_URL;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Could not create the HTTP client");
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET %s -> %d %s", cfg.url, status, status == 200 ? "OK" : "");
    } else {
        ESP_LOGE(TAG, "HTTP GET %s failed: %s", cfg.url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void connectivity_task(void *arg)
{
    esp_ip4_addr_t gw;
    gw.addr = (uint32_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "----- Connectivity test -----");
    ping_gateway(gw);
    http_get_test();
    ESP_LOGI(TAG, "-----------------------------");
    vTaskDelete(nullptr);
}

// ---------- WiFi events ----------

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
        ESP_LOGI(TAG, "=============================================");
        ESP_LOGI(TAG, "Connected to \"%s\"", CONFIG_HUBDOT_WIFI_SSID);
        ESP_LOGI(TAG, "IP: " IPSTR "  Gateway: " IPSTR "  Mask: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));
        ESP_LOGI(TAG, "Test it from your laptop: ping " IPSTR "   |   curl -i http://" IPSTR "/",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "=============================================");
        xTaskCreate(connectivity_task, "conn_test", 4096,
                    (void *)(uintptr_t)ev->ip_info.gw.addr, 3, nullptr);
    }
}

// ---------- Start ----------

bool wifi_configured()
{
    return strlen(CONFIG_HUBDOT_WIFI_SSID) > 0;
}

bool wifi_wait_connected(int timeout_ms)
{
    if (s_events == nullptr) return false;
    return xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & CONNECTED_BIT;
}

void wifi_start(const char *hostname, JsonProvider provider)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "WiFi MAC: " MACSTR, MAC2STR(mac));

    if (!wifi_configured()) {
        ESP_LOGW(TAG, "WiFi disabled: set the SSID in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    // NVS is required by the WiFi driver (calibration data)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_provider = provider;
    s_events = xEventGroupCreate();
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_DELAY_MS), pdFALSE, nullptr, retry_timer_cb);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr));

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_HUBDOT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept whatever security the network uses
#if !CONFIG_HUBDOT_WIFI_ENTERPRISE
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_HUBDOT_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

#if CONFIG_HUBDOT_WIFI_ENTERPRISE
    // WPA2-Enterprise PEAP/MSCHAPv2, without validating the server certificate
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
    start_http_server();
}
