#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "room.h"
#include "bus.h"
#include "messages.h"
#include "jsonlite.h"

#define HEARTBEAT_MS 5000
enum { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 };

typedef struct {
    room_cfg_t cfg;
    char state_topic[64], cmd_topic[64];
    SemaphoreHandle_t mx;                 // guards mode and emergency (set by the bus task)
    int mode;
    bool emergency;
    volatile bool failed;                 // simulated crash: no light changes, no reports
    volatile bool need_pub;
    char name[12];
} room_t;

static const char *TAG = "room";
static room_t R[2];

static void publish_state(room_t *r, int occ, int light, int m)
{
    char buf[96];
    snprintf(buf, sizeof buf, "{\"room\":%d,\"occupied\":%d,\"light\":%d,\"mode\":%d}",
             r->cfg.room_id, occ, light, m);
    bus_publish(r->state_topic, buf, true);
}

// Runs in the bus task
static void room_msg(void *arg, const char *topic, const char *data)
{
    room_t *r = (room_t *)arg;

    if (!strcmp(topic, r->cmd_topic)) {
        char v[12];
        if (jl_get_str(data, "light", v, sizeof v)) {
            int m = -1;
            if (!strcmp(v, "auto")) m = MODE_AUTO;
            else if (!strcmp(v, "on")) m = MODE_ON;
            else if (!strcmp(v, "off")) m = MODE_OFF;
            if (m >= 0) {
                xSemaphoreTake(r->mx, portMAX_DELAY);
                r->mode = m;
                xSemaphoreGive(r->mx);
                ESP_LOGI(TAG, "Room %d light mode set to %s by the entrance", r->cfg.room_id, v);
            }
        }
    } else if (!strcmp(topic, T_SYSTEM_STATE)) {
        int em;
        if (jl_get_int(data, "emergency", &em)) {
            xSemaphoreTake(r->mx, portMAX_DELAY);
            bool changed = (r->emergency != (em != 0));
            r->emergency = (em != 0);
            xSemaphoreGive(r->mx);
            if (changed) ESP_LOGW(TAG, "Room %d: EMERGENCY %s", r->cfg.room_id, em ? "ON, lights forced on" : "cleared");
        }
    }
}

static void room_task(void *p)
{
    room_t *r = (room_t *)p;
    TickType_t last_motion = 0, last_pub = 0;
    bool seen_motion = false;
    int occ = 0, light = 0, last_mode = MODE_AUTO;

    while (1) {
        if (r->failed) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // a crashed controller does nothing

        TickType_t now = xTaskGetTickCount();
        if (gpio_get_level(r->cfg.pir_gpio)) { last_motion = now; seen_motion = true; }
        int new_occ = seen_motion &&
                      (now - last_motion) < pdMS_TO_TICKS(r->cfg.vacancy_timeout_s * 1000);
        if (new_occ != occ) {
            occ = new_occ;
            ESP_LOGI(TAG, "Room %d is now %s", r->cfg.room_id, occ ? "OCCUPIED" : "vacant");
            r->need_pub = true;
        }

        xSemaphoreTake(r->mx, portMAX_DELAY);
        int m = r->mode;
        bool em = r->emergency;
        xSemaphoreGive(r->mx);

        // priority: emergency, then manual override, then occupancy
        int want = em ? 1 : (m == MODE_ON ? 1 : (m == MODE_OFF ? 0 : occ));
        if (want != light) {
            light = want;
            gpio_set_level(r->cfg.light_gpio, light);
            ESP_LOGI(TAG, "Room %d light %s%s", r->cfg.room_id, light ? "ON" : "off", em ? " (emergency)" : "");
            r->need_pub = true;
        }
        if (m != last_mode) { last_mode = m; r->need_pub = true; }
        if ((now - last_pub) >= pdMS_TO_TICKS(HEARTBEAT_MS)) r->need_pub = true;

        if (r->need_pub) {
            r->need_pub = false;
            publish_state(r, occ, light, m);
            last_pub = now;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void room_fault(int room_id, bool failed)
{
    if (room_id < 1 || room_id > 2) return;
    R[room_id - 1].failed = failed;
    if (!failed) R[room_id - 1].need_pub = true;
    ESP_LOGW(TAG, "Room %d controller %s (simulated)", room_id, failed ? "CRASHED" : "recovered");
}

void room_start(const room_cfg_t *c)
{
    room_t *r = &R[c->room_id - 1];
    memset(r, 0, sizeof *r);
    r->cfg = *c;
    r->need_pub = true;
    r->mx = xSemaphoreCreateMutex();
    snprintf(r->state_topic, sizeof r->state_topic, TOPIC_ROOT "/room%d/state", c->room_id);
    snprintf(r->cmd_topic, sizeof r->cmd_topic, TOPIC_ROOT "/room%d/cmd", c->room_id);
    snprintf(r->name, sizeof r->name, "room%d", c->room_id);

    gpio_config_t out = { .pin_bit_mask = 1ULL << c->light_gpio, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(c->light_gpio, 0);

    gpio_config_t in = { .pin_bit_mask = 1ULL << c->pir_gpio, .mode = GPIO_MODE_INPUT };
    if (c->pir_gpio < 34) in.pull_down_en = GPIO_PULLDOWN_ENABLE;   // pins 34 to 39 have no internal pull resistors
    gpio_config(&in);

    bus_subscribe(r->cmd_topic, room_msg, r);
    bus_subscribe(T_SYSTEM_STATE, room_msg, r);
    xTaskCreate(room_task, r->name, 4096, r, 5, NULL);
}