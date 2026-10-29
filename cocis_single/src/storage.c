#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "config.h"
#include "storage.h"

#define MOUNT     "/sdcard"
#define LINE_MAX_ 160
#define SD_HOST   SPI2_HOST

enum { K_ACCESS, K_ROOMS, K_SYSTEM, K_COUNT };
static const char *kind_name[K_COUNT] = { "access", "rooms", "system" };
static const char *kind_file[K_COUNT] = { MOUNT "/access.csv", MOUNT "/rooms.csv", MOUNT "/system.csv" };
static const char *kind_hdr[K_COUNT]  = { "time,dir,uid,name,result,inside",
                                          "time,room,occupied,light",
                                          "time,event" };

typedef struct { uint8_t kind; char line[LINE_MAX_]; } log_item_t;

static const char *TAG = "storage";
static QueueHandle_t log_q;
static SemaphoreHandle_t sd_mx;            // guards every file access on the card
static sdmmc_card_t *card;
static volatile bool ready;
static volatile uint32_t written, dropped;
static int last_occ[3] = { -1, -1, -1 }, last_light[3] = { -1, -1, -1 };

static void stamp(char *out, size_t n)
{
    time_t t = time(NULL);
    if (t > 1700000000) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(out, n, "boot+%lus", (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ));
    }
}

static void enqueue(int kind, const char *line)
{
    if (!log_q) return;
    log_item_t it = { .kind = (uint8_t)kind };
    snprintf(it.line, sizeof it.line, "%s", line);
    if (xQueueSend(log_q, &it, 0) != pdTRUE) dropped++;
}

void log_access(const char *dir, const char *uid, const char *name, const char *result, int inside)
{
    char ts[24], line[LINE_MAX_];
    stamp(ts, sizeof ts);
    snprintf(line, sizeof line, "%s,%s,%s,%s,%s,%d", ts, dir, uid, name, result, inside);
    enqueue(K_ACCESS, line);
}

void log_room(int room, int occupied, int light)
{
    if (room < 1 || room > 2) return;
    if (last_occ[room] == occupied && last_light[room] == light) return;   // heartbeat, no change
    last_occ[room] = occupied;
    last_light[room] = light;
    char ts[24], line[LINE_MAX_];
    stamp(ts, sizeof ts);
    snprintf(line, sizeof line, "%s,%d,%d,%d", ts, room, occupied, light);
    enqueue(K_ROOMS, line);
}

void log_systemf(const char *fmt, ...)
{
    char msg[110], ts[24], line[LINE_MAX_];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    stamp(ts, sizeof ts);
    snprintf(line, sizeof line, "%s,%s", ts, msg);
    enqueue(K_SYSTEM, line);
}

static void logger_task(void *p)
{
    log_item_t it;
    while (1) {
        if (xQueueReceive(log_q, &it, portMAX_DELAY) != pdTRUE) continue;
        if (!ready) { dropped++; continue; }

        xSemaphoreTake(sd_mx, portMAX_DELAY);
        FILE *f = fopen(kind_file[it.kind], "a");
        if (f) {
            fputs(it.line, f);
            fputc('\n', f);
            fclose(f);
            written++;
        } else {
            dropped++;
        }
        xSemaphoreGive(sd_mx);
    }
}

bool storage_ready(void) { return ready; }

bool storage_init(void)
{
    sd_mx = xSemaphoreCreateMutex();
    log_q = xQueueCreate(16, sizeof(log_item_t));
    xTaskCreate(logger_task, "logger", 6144, NULL, 3, NULL);

    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t r = spi_bus_initialize(SD_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (r != ESP_OK) { ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(r)); return false; }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    r = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mc, &card);
    if (r != ESP_OK) { ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(r)); return false; }

    xSemaphoreTake(sd_mx, portMAX_DELAY);
    for (int k = 0; k < K_COUNT; k++) {
        struct stat st;
        if (stat(kind_file[k], &st) != 0) {          // new file: write the header row
            FILE *f = fopen(kind_file[k], "w");
            if (f) { fputs(kind_hdr[k], f); fputc('\n', f); fclose(f); }
        }
    }
    xSemaphoreGive(sd_mx);

    ready = true;
    ESP_LOGI(TAG, "SD card mounted");
    log_systemf("boot: SD card mounted");
    return true;
}

void storage_tail(const char *kind, int n)
{
    int k = -1;
    for (int i = 0; i < K_COUNT; i++) if (!strcasecmp(kind, kind_name[i])) k = i;
    if (k < 0) { printf("Unknown log. Use: access | rooms | system\n"); return; }
    if (!ready) { printf("SD card not available\n"); return; }
    if (n < 1) n = 1;
    if (n > 20) n = 20;

    char (*ring)[LINE_MAX_] = malloc((size_t)n * LINE_MAX_);
    if (!ring) { printf("Out of memory\n"); return; }

    int count = 0;
    xSemaphoreTake(sd_mx, portMAX_DELAY);
    FILE *f = fopen(kind_file[k], "r");
    if (f) {
        char buf[LINE_MAX_];
        bool header = true;
        while (fgets(buf, sizeof buf, f)) {
            if (header) { header = false; continue; }
            strcpy(ring[count % n], buf);
            count++;
        }
        fclose(f);
    }
    xSemaphoreGive(sd_mx);

    printf("%s\n", kind_hdr[k]);
    if (count == 0) printf("(no records)\n");
    for (int i = (count > n ? count - n : 0); i < count; i++) fputs(ring[i % n], stdout);
    free(ring);
}

void storage_status(void)
{
    printf("SD card: %s, records written %u, dropped %u\n",
           ready ? "mounted" : "NOT available", (unsigned)written, (unsigned)dropped);
}

#define USERS_FILE MOUNT "/users.csv"

int storage_load_users(user_rec_t *out, int max)
{
    if (!ready) return -1;
    int n = 0;
    bool opened = false;
    xSemaphoreTake(sd_mx, portMAX_DELAY);
    FILE *f = fopen(USERS_FILE, "r");
    if (f) {
        opened = true;
        char buf[80];
        bool header = true;
        while (n < max && fgets(buf, sizeof buf, f)) {
            if (header) { header = false; continue; }
            char *uid  = strtok(buf, ",\r\n");
            char *name = strtok(NULL, ",\r\n");
            char *adm  = strtok(NULL, ",\r\n");
            if (!uid || !name) continue;
            snprintf(out[n].uid,  sizeof out[n].uid,  "%s", uid);
            snprintf(out[n].name, sizeof out[n].name, "%s", name);
            out[n].admin = adm && atoi(adm) == 1;
            n++;
        }
        fclose(f);
    }
    xSemaphoreGive(sd_mx);
    return opened ? n : -1;
}

bool storage_save_users(const user_rec_t *in, int n)
{
    if (!ready) return false;
    xSemaphoreTake(sd_mx, portMAX_DELAY);
    FILE *f = fopen(USERS_FILE, "w");
    bool ok = (f != NULL);
    if (f) {
        fputs("uid,name,admin\n", f);
        for (int i = 0; i < n; i++)
            fprintf(f, "%s,%s,%d\n", in[i].uid, in[i].name, in[i].admin ? 1 : 0);
        fclose(f);
    }
    xSemaphoreGive(sd_mx);
    return ok;
}