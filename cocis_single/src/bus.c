#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bus.h"

#define MAX_SUBS      16
#define MAX_RETAINED  12
#define TOPIC_LEN     64
#define DATA_LEN      192

typedef struct { char topic[TOPIC_LEN]; char data[DATA_LEN]; bool retain; int target; } msg_t;
typedef struct { char filter[TOPIC_LEN]; bus_cb_t cb; void *arg; } sub_t;
typedef struct { char topic[TOPIC_LEN]; char data[DATA_LEN]; bool used; } retained_t;

static const char *TAG = "bus";
static QueueHandle_t q;
static SemaphoreHandle_t mx;          // guards subs[] and retained[]
static sub_t subs[MAX_SUBS];
static int nsubs;
static retained_t retained[MAX_RETAINED];

static bool matches(const char *filter, const char *topic)
{
    size_t n = strlen(filter);
    if (n >= 2 && filter[n - 1] == '#' && filter[n - 2] == '/')
        return strncmp(filter, topic, n - 1) == 0;
    return strcmp(filter, topic) == 0;
}

static void bus_task(void *p)
{
    msg_t m;
    while (1) {
        if (xQueueReceive(q, &m, portMAX_DELAY) != pdTRUE) continue;

        bus_cb_t cbs[MAX_SUBS];
        void *args[MAX_SUBS];
        int n = 0;

        xSemaphoreTake(mx, portMAX_DELAY);
        if (m.retain && m.target < 0) {
            retained_t *slot = NULL;
            for (int i = 0; i < MAX_RETAINED; i++) {
                if (retained[i].used && !strcmp(retained[i].topic, m.topic)) { slot = &retained[i]; break; }
                if (!retained[i].used && !slot) slot = &retained[i];
            }
            if (slot) {
                snprintf(slot->topic, sizeof slot->topic, "%s", m.topic);
                snprintf(slot->data, sizeof slot->data, "%s", m.data);
                slot->used = true;
            } else {
                ESP_LOGW(TAG, "retained store is full");
            }
        }
        for (int i = 0; i < nsubs; i++) {
            if ((m.target < 0 || m.target == i) && matches(subs[i].filter, m.topic)) {
                cbs[n] = subs[i].cb;
                args[n] = subs[i].arg;
                n++;
            }
        }
        xSemaphoreGive(mx);

        for (int i = 0; i < n; i++) cbs[i](args[i], m.topic, m.data);   // callbacks run outside the lock
    }
}

void bus_init(void)
{
    mx = xSemaphoreCreateMutex();
    q = xQueueCreate(16, sizeof(msg_t));
    xTaskCreate(bus_task, "bus", 4096, NULL, 5, NULL);
}

int bus_subscribe(const char *filter, bus_cb_t cb, void *arg)
{
    xSemaphoreTake(mx, portMAX_DELAY);
    if (nsubs >= MAX_SUBS) { xSemaphoreGive(mx); return -1; }
    int id = nsubs++;
    snprintf(subs[id].filter, sizeof subs[id].filter, "%s", filter);
    subs[id].cb = cb;
    subs[id].arg = arg;

    // a new subscriber immediately receives the retained messages it matches
    for (int i = 0; i < MAX_RETAINED; i++) {
        if (retained[i].used && matches(filter, retained[i].topic)) {
            msg_t m = { .retain = false, .target = id };
            snprintf(m.topic, sizeof m.topic, "%s", retained[i].topic);
            snprintf(m.data, sizeof m.data, "%s", retained[i].data);
            xQueueSend(q, &m, 0);
        }
    }
    xSemaphoreGive(mx);
    return id;
}

bool bus_publish(const char *topic, const char *payload, bool retain)
{
    if (!q) return false;
    msg_t m = { .retain = retain, .target = -1 };
    snprintf(m.topic, sizeof m.topic, "%s", topic);
    snprintf(m.data, sizeof m.data, "%s", payload);
    return xQueueSend(q, &m, 0) == pdTRUE;
}