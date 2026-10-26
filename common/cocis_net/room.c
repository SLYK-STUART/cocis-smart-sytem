#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "room.h"
#include "net.h"
#include "messages.h"
#include "jsonlite.h"

#define HEARTBEAT_MS 30000
enum { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 };

static const char *TAG = "room";
static room_cfg_t cfg;
static char state_topic[64], cmd_topic[64];
static SemaphoreHandle_t mx;                 // guards mode and emergency (set by the MQTT task)
static int mode = MODE_AUTO;
static bool emergency = false;
static volatile bool need_pub = true;
static bool entrance_online = true;

static void publish_state(int occ, int light, int m)
{
    char buf[96];
    snprintf(buf, sizeof buf, "{\"room\":%d,\"occupied\":%d,\"light\":%d,\"mode\":%d}",
             cfg.room_id, occ, light, m);
    net_publish(state_topic, buf, 1, 1);     // retained
}

// Runs in the MQTT task
static void room_msg(const char *topic, const char *data)
{
    if (!strcmp(topic, cmd_topic)) {
        char v[12];
        if (jl_get_str(data, "light", v, sizeof v)) {
            int m = -1;
            if (!strcmp(v, "auto")) m = MODE_AUTO;
            else if (!strcmp(v, "on")) m = MODE_ON;
            else if (!strcmp(v, "off")) m = MODE_OFF;
            if (m >= 0) {
                xSemaphoreTake(mx, portMAX_DELAY);
                mode = m;
                xSemaphoreGive(mx);
                ESP_LOGI(TAG, "Room %d light mode set to %s by the entrance", cfg.room_id, v);
            }
        }
    } else if (!strcmp(topic, T_SYSTEM_STATE)) {
        int em;
        if (jl_get_int(data, "emergency", &em)) {
            xSemaphoreTake(mx, portMAX_DELAY);
            bool changed = (emergency != (em != 0));
            emergency = (em != 0);
            xSemaphoreGive(mx);
            if (changed) ESP_LOGW(TAG, "EMERGENCY %s", em ? "ON: lights forced on" : "cleared");
        }
    } else if (!strcmp(topic, T_STATUS_BASE "entrance")) {
        bool on = !strcmp(data, "online");
        if (on != entrance_online) {
            entrance_online = on;
            ESP_LOGW(TAG, "Entrance is %s%s", data, on ? "" : " - running autonomously");
        }
    }
}

static void room_task(void *p)
{
    TickType_t last_motion = 0, last_pub = 0;
    bool seen_motion = false;
    int occ = 0, light = 0, last_mode = MODE_AUTO;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (gpio_get_level(cfg.pir_gpio)) { last_motion = now; seen_motion = true; }
        int new_occ = seen_motion &&
                      (now - last_motion) < pdMS_TO_TICKS(cfg.vacancy_timeout_s * 1000);
        if (new_occ != occ) {
            occ = new_occ;
            ESP_LOGI(TAG, "Room %d is now %s", cfg.room_id, occ ? "OCCUPIED" : "vacant");
            need_pub = true;
        }

        xSemaphoreTake(mx, portMAX_DELAY);
        int m = mode;
        bool em = emergency;
        xSemaphoreGive(mx);

        // priority: emergency, then manual override, then occupancy
        int want = em ? 1 : (m == MODE_ON ? 1 : (m == MODE_OFF ? 0 : occ));
        if (want != light) {
            light = want;
            gpio_set_level(cfg.light_gpio, light);
            ESP_LOGI(TAG, "Room %d light %s%s", cfg.room_id, light ? "ON" : "off",
                     em ? " (emergency)" : "");
            need_pub = true;
        }
        if (m != last_mode) { last_mode = m; need_pub = true; }
        if ((now - last_pub) >= pdMS_TO_TICKS(HEARTBEAT_MS)) need_pub = true;

        if (need_pub && net_is_connected()) {
            need_pub = false;
            publish_state(occ, light, m);
            last_pub = now;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void room_start(const room_cfg_t *c)
{
    cfg = *c;
    mx = xSemaphoreCreateMutex();
    snprintf(state_topic, sizeof state_topic, TOPIC_ROOT "/room%d/state", cfg.room_id);
    snprintf(cmd_topic, sizeof cmd_topic, TOPIC_ROOT "/room%d/cmd", cfg.room_id);

    gpio_config_t out = { .pin_bit_mask = 1ULL << cfg.light_gpio, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(cfg.light_gpio, 0);

    gpio_config_t in = { .pin_bit_mask = 1ULL << cfg.pir_gpio, .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&in);

    net_init(cfg.node_name, room_msg);
    net_subscribe(cmd_topic);
    net_subscribe(T_SYSTEM_STATE);
    net_subscribe(T_STATUS_BASE "entrance");
    xTaskCreate(room_task, "room", 4096, NULL, 5, NULL);
}