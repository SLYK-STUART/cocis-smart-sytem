#include <string.h>
#include <stdio.h>
#include "net.h"
#include "messages.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#ifndef BROKER_URI
#define BROKER_URI "mqtt://broker.hivemq.com"
#endif
#define MAX_SUBS 8

static const char *TAG = "net";
static esp_mqtt_client_handle_t client;
static net_msg_cb_t user_cb;
static const char *subs[MAX_SUBS];
static int nsubs;
static volatile bool connected;
static bool mqtt_started;
static char client_id[48];
static char status_topic[64];

static void mqtt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        connected = true;
        ESP_LOGI(TAG, "MQTT connected as %s", client_id);
        esp_mqtt_client_publish(client, status_topic, "online", 0, 1, 1);
        for (int i = 0; i < nsubs; i++) esp_mqtt_client_subscribe(client, subs[i], 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (user_cb) {
            char t[96], d[256];
            int tl = e->topic_len < 95 ? e->topic_len : 95;
            int dl = e->data_len < 255 ? e->data_len : 255;
            memcpy(t, e->topic, tl); t[tl] = 0;
            memcpy(d, e->data, dl);  d[dl] = 0;
            user_cb(t, d);
        }
        break;
    default:
        break;
    }
}

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        if (!mqtt_started) { mqtt_started = true; esp_mqtt_client_start(client); }
    }
}

void net_init(const char *node_name, net_msg_cb_t cb)
{
    user_cb = cb;
    snprintf(client_id, sizeof client_id, "cocis_%s_%s", GROUP_ID, node_name);
    snprintf(status_topic, sizeof status_topic, T_STATUS_BASE "%s", node_name);

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = BROKER_URI,
        .credentials.client_id = client_id,
        .session.keepalive = 15,
        .session.last_will = {
            .topic = status_topic,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };
    client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_handler, NULL);

    wifi_config_t wc = { .sta = { .ssid = "Wokwi-GUEST", .password = "", .channel = 6 } };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
}

void net_subscribe(const char *topic)
{
    if (nsubs < MAX_SUBS) subs[nsubs++] = topic;
    if (connected) esp_mqtt_client_subscribe(client, topic, 1);
}

int net_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!client) return -1;
    return esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
}

bool net_is_connected(void) { return connected; }