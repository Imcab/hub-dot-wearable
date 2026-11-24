// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#include "mqtt_pub.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "wifi.hpp"

static const char *TAG = "mqtt";

static constexpr int QOS = 1;                      // ver README: una lectura perdida en un corte de WiFi si importa
static constexpr int KEEPALIVE_S = 15;             // el broker declara muerta la placa (Last Will) a ~1.5x esto
static constexpr time_t VALID_EPOCH = 1735689600;  // 2025-01-01: antes de esto el reloj no se ha sincronizado

static esp_mqtt_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static char s_device_id[40] = "";
static char s_data_topic[64] = "";
static char s_status_topic[80] = "";
static char s_online_msg[96] = "";
static char s_offline_msg[96] = "";

static void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to the broker %s as %s, publishing to %s", CONFIG_HUBDOT_MQTT_BROKER_URI,
                 s_device_id, s_data_topic);
        // Reemplaza el "online":false que haya dejado el Last Will de la conexion anterior
        esp_mqtt_client_enqueue(s_client, s_status_topic, s_online_msg, 0, QOS, 1, true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "Disconnected from the broker, reconnecting...");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "Broker refused the connection (code %d): check the user and password",
                     ev->error_handle->connect_return_code);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "Could not reach the broker: %s", esp_err_to_name(ev->error_handle->esp_tls_last_esp_err));
        }
        break;
    default:
        break;
    }
}

const char *mqtt_device_id() { return s_device_id; }

bool mqtt_time_synced() { return time(nullptr) >= VALID_EPOCH; }

uint32_t mqtt_timestamp()
{
    const time_t now = time(nullptr);
    if (now >= VALID_EPOCH) return (uint32_t)now;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void mqtt_pub_start(const char *board)
{
    if (s_client != nullptr) return;
    snprintf(s_device_id, sizeof(s_device_id), "%s-%s", CONFIG_HUBDOT_TEAM_ID, board);

    if (!wifi_configured()) {
        ESP_LOGW(TAG, "MQTT disabled: set the SSID in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    // Hora real para "ts", aunque no haya broker (el payload se imprime igual). Se sincroniza
    // sola en cuanto hay IP, y se repite cada hora
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.wait_for_sync = false;
    esp_netif_sntp_init(&sntp_cfg);

    if (strlen(CONFIG_HUBDOT_MQTT_BROKER_URI) == 0) {
        ESP_LOGW(TAG, "MQTT disabled: set the broker URI in `idf.py menuconfig` -> hub-dot WiFi");
        return;
    }

    snprintf(s_data_topic, sizeof(s_data_topic), "reto2026/%s/data", CONFIG_HUBDOT_TEAM_ID);
    snprintf(s_status_topic, sizeof(s_status_topic), "reto2026/%s/status/%s", CONFIG_HUBDOT_TEAM_ID, board);
    snprintf(s_online_msg, sizeof(s_online_msg), "{\"device_id\":\"%s\",\"online\":true}", s_device_id);
    snprintf(s_offline_msg, sizeof(s_offline_msg), "{\"device_id\":\"%s\",\"online\":false}", s_device_id);

    static char client_id[48];
    snprintf(client_id, sizeof(client_id), "ESP32-%s", s_device_id);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = CONFIG_HUBDOT_MQTT_BROKER_URI;
    cfg.credentials.client_id = client_id;
    if (strlen(CONFIG_HUBDOT_MQTT_USERNAME) > 0) {
        cfg.credentials.username = CONFIG_HUBDOT_MQTT_USERNAME;
        cfg.credentials.authentication.password = CONFIG_HUBDOT_MQTT_PASSWORD;
    }
    cfg.session.keepalive = KEEPALIVE_S;
    cfg.session.last_will.topic = s_status_topic;
    cfg.session.last_will.msg = s_offline_msg;
    cfg.session.last_will.qos = QOS;
    cfg.session.last_will.retain = 1;

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed, nothing will be published");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, nullptr);
    esp_mqtt_client_start(s_client);
}

bool mqtt_pub_publish(const char *payload, size_t len)
{
    if (s_client == nullptr || !s_connected) return false;
    // enqueue (no publish): solo copia el mensaje, la tarea de esp-mqtt lo manda
    return esp_mqtt_client_enqueue(s_client, s_data_topic, payload, (int)len, QOS, 0, true) >= 0;
}
