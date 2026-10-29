#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "config.h"
#include "rfid.h"

#define RFID_HOST   SPI3_HOST     // the SD card uses SPI2

static const char *TAG = "RFID";

// MFRC522 registers and commands
enum {
    CommandReg = 0x01, ComIrqReg = 0x04, ErrorReg = 0x06, FIFODataReg = 0x09,
    FIFOLevelReg = 0x0A, ControlReg = 0x0C, BitFramingReg = 0x0D, CollReg = 0x0E,
    ModeReg = 0x11, TxControlReg = 0x14, TxASKReg = 0x15,
    TModeReg = 0x2A, TPrescalerReg = 0x2B, TReloadRegH = 0x2C, TReloadRegL = 0x2D,
    VersionReg = 0x37,
};
enum { CMD_IDLE = 0x00, CMD_TRANSCEIVE = 0x0C, CMD_SOFTRESET = 0x0F };

typedef struct {
    const char *name;
    spi_device_handle_t dev;
    bool present;       // a card is currently on the reader
    int misses;         // consecutive polls with no answer while 'present'
    bool fail_logged;   // the ID-read failure was already reported for this card
} reader_t;

static reader_t readers[2] = { { .name = "ENTRY" }, { .name = "EXIT" } };
static rfid_scan_cb_t scan_cb;
static uint8_t last_err;
static bool diag_quiet;

// ---------------- register access ----------------
static uint8_t rd(spi_device_handle_t d, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(0x80 | (reg << 1)), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(d, &t);
    return rx[1];
}

static void wr(spi_device_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg << 1), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(d, &t);
}

static void setbits(spi_device_handle_t d, uint8_t reg, uint8_t mask) { wr(d, reg, rd(d, reg) | mask); }
static void clrbits(spi_device_handle_t d, uint8_t reg, uint8_t mask) { wr(d, reg, rd(d, reg) & ~mask); }

// Burst-read n bytes (n <= 10) from the FIFO in ONE transaction, like the Arduino MFRC522 library.
// Reading the FIFO one byte per transaction lost every other byte in the simulator.
static void rd_fifo(spi_device_handle_t d, uint8_t *buf, int n)
{
    if (n <= 0) return;
    uint8_t a = (uint8_t)(0x80 | (FIFODataReg << 1));
    uint8_t tx[11], rx[11] = {0};
    for (int i = 0; i < n; i++) tx[i] = a;
    tx[n] = 0x00;
    spi_transaction_t t = { .length = 8 * (n + 1), .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(d, &t);
    memcpy(buf, &rx[1], n);
}

static void dump(const char *what, int status, const uint8_t *b, int n)
{
    if (diag_quiet) return;
    char s[40] = "";
    for (int i = 0; i < n && i < 12; i++) sprintf(s + i * 3, "%02X ", b[i]);
    ESP_LOGW(TAG, "  step '%s': status=%d (0 ok, -1 timeout, -2 error, -3 collision) len=%d err=0x%02X data=%s",
             what, status, n, last_err, s);
}

// ---------------- card communication ----------------
// 0 = ok, -1 = timeout, -2 = protocol/parity/overflow error, -3 = collision
static int transceive(spi_device_handle_t d, const uint8_t *tx, int txlen, int tx_last_bits,
                      uint8_t *rx, int *rxlen, int *rx_valid_bits)
{
    last_err = 0;
    wr(d, CommandReg, CMD_IDLE);
    wr(d, ComIrqReg, 0x7F);
    wr(d, FIFOLevelReg, 0x80);
    for (int i = 0; i < txlen; i++) wr(d, FIFODataReg, tx[i]);
    wr(d, BitFramingReg, tx_last_bits & 0x07);
    wr(d, CommandReg, CMD_TRANSCEIVE);
    setbits(d, BitFramingReg, 0x80);

    TickType_t start = xTaskGetTickCount();
    while (1) {
        uint8_t irq = rd(d, ComIrqReg);
        if (irq & 0x30) break;
        if (irq & 0x01) return -1;
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(50)) return -1;
        vTaskDelay(1);
    }

    uint8_t err = rd(d, ErrorReg);
    last_err = err;
    if (err & 0x13) { *rxlen = 0; return -2; }
    int n = rd(d, FIFOLevelReg);
    if (n > *rxlen) n = *rxlen;
    rd_fifo(d, rx, n);
    *rxlen = n;
    *rx_valid_bits = rd(d, ControlReg) & 0x07;
    return (err & 0x08) ? -3 : 0;
}

static uint16_t crc_a(const uint8_t *data, int len)
{
    uint16_t crc = 0x6363;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        b ^= (uint8_t)(crc & 0xFF);
        b ^= (uint8_t)(b << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4));
    }
    return crc;
}

// cmd: 0x26 = REQA (idle cards only), 0x52 = WUPA (also wakes halted cards)
static bool request(spi_device_handle_t d, uint8_t cmd)
{
    clrbits(d, CollReg, 0x80);
    uint8_t atqa[2];
    int n = sizeof atqa, vb = 0;
    return transceive(d, &cmd, 1, 7, atqa, &n, &vb) == 0 && n == 2 && vb == 0;
}

// Reads a 4- or 7-byte UID. Returns the length, or 0 on failure.
static int read_uid(spi_device_handle_t d, uint8_t *uid)
{
    static const uint8_t SEL[2] = { 0x93, 0x95 };
    int total = 0;

    for (int lvl = 0; lvl < 2; lvl++) {
        uint8_t cmd[9], rx[10] = {0}, sak[4] = {0};
        int n = sizeof rx, vb = 0;

        cmd[0] = SEL[lvl];
        cmd[1] = 0x20;
        clrbits(d, CollReg, 0x80);
        int st = transceive(d, cmd, 2, 0, rx, &n, &vb);
        if (st != 0 || n != 5 || (uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) {
            dump("anticollision", st, rx, st == 0 ? n : 0);
            return 0;
        }

        cmd[0] = SEL[lvl];
        cmd[1] = 0x70;
        memcpy(&cmd[2], rx, 5);
        uint16_t crc = crc_a(cmd, 7);
        cmd[7] = crc & 0xFF;
        cmd[8] = crc >> 8;
        int sn = sizeof sak;
        st = transceive(d, cmd, 9, 0, sak, &sn, &vb);
        if (st != 0 || sn != 3) {
            dump("select", st, sak, st == 0 ? sn : 0);
            return 0;
        }

        if (rx[0] == 0x88) {                // cascade tag: the UID continues at the next level
            memcpy(&uid[total], &rx[1], 3);
            total += 3;
        } else {
            memcpy(&uid[total], rx, 4);
            return total + 4;
        }
    }
    return 0;
}

static void halt_card(spi_device_handle_t d)
{
    const uint8_t hlta[4] = { 0x50, 0x00, 0x57, 0xCD };
    uint8_t rx[2];
    int n = sizeof rx, vb = 0;
    transceive(d, hlta, 4, 0, rx, &n, &vb);     // a timeout here means success
}

// ---------------- reader set-up and polling ----------------
static void reader_init(reader_t *r)
{
    wr(r->dev, CommandReg, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    wr(r->dev, TModeReg, 0x80);
    wr(r->dev, TPrescalerReg, 0xA9);
    wr(r->dev, TReloadRegH, 0x03);
    wr(r->dev, TReloadRegL, 0xE8);
    wr(r->dev, TxASKReg, 0x40);
    wr(r->dev, ModeReg, 0x3D);
    setbits(r->dev, TxControlReg, 0x03);        // antenna on
    uint8_t v = rd(r->dev, VersionReg);
    if (v == 0x00 || v == 0xFF) ESP_LOGE(TAG, "%s reader: no chip answering (version 0x%02X)", r->name, v);
    else ESP_LOGI(TAG, "%s reader ready (version 0x%02X)", r->name, v);
}

static void poll_reader(reader_t *r, bool entering)
{
    if (!r->present) {
        if (!request(r->dev, 0x26)) { r->fail_logged = false; return; }   // no card

        diag_quiet = r->fail_logged;
        uint8_t uid[10];
        int len = read_uid(r->dev, uid);
        if (len > 0) {
            char s[21] = "";
            for (int i = 0; i < len; i++) sprintf(s + i * 2, "%02X", uid[i]);
            ESP_LOGI(TAG, "[%s] card presented: %s", r->name, s);
            r->present = true;
            r->misses = 0;
            r->fail_logged = false;
            halt_card(r->dev);
            if (scan_cb) scan_cb(entering, s);
        } else if (!r->fail_logged) {
            ESP_LOGW(TAG, "[%s] a card answered but its ID could not be read", r->name);
            r->fail_logged = true;
        }
    } else {
        if (request(r->dev, 0x52)) {            // still there: wake it, then halt it again
            r->misses = 0;
            halt_card(r->dev);
        } else if (++r->misses >= 3) {
            r->present = false;
            ESP_LOGI(TAG, "[%s] card removed", r->name);
        }
    }
}

static void poll_task(void *p)
{
    while (1) {
        poll_reader(&readers[0], true);         // entry reader
        poll_reader(&readers[1], false);        // exit reader
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool rfid_start(rfid_scan_cb_t cb)
{
    scan_cb = cb;

    spi_bus_config_t bus_config = {
        .mosi_io_num = RFID_PIN_MOSI,
        .miso_io_num = RFID_PIN_MISO,
        .sclk_io_num = RFID_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t e = spi_bus_initialize(RFID_HOST, &bus_config, SPI_DMA_DISABLED);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SPI bus failed: %s", esp_err_to_name(e)); return false; }

    const int cs[2] = { RFID_PIN_CS_ENTRY, RFID_PIN_CS_EXIT };
    for (int i = 0; i < 2; i++) {
        spi_device_interface_config_t dc = {
            .clock_speed_hz = 1000000,
            .mode = 0,
            .spics_io_num = cs[i],
            .queue_size = 1,
        };
        e = spi_bus_add_device(RFID_HOST, &dc, &readers[i].dev);
        if (e != ESP_OK) { ESP_LOGE(TAG, "%s reader device failed: %s", readers[i].name, esp_err_to_name(e)); return false; }
    }
    for (int i = 0; i < 2; i++) reader_init(&readers[i]);

    xTaskCreate(poll_task, "rfid_poll", 4096, NULL, 4, NULL);
    return true;
}