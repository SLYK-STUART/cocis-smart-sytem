#pragma once

// ---- Entrance indicators
#define GPIO_DOOR_IN        18
#define GPIO_DOOR_OUT       19
#define GPIO_DENIED         21
#define GPIO_ALARM          25
#define DOOR_OPEN_MS        2000

// ---- Authentication and schedule
#define ADMIN_PIN           "4321"
#define SESSION_S           120
#define DEFAULT_OPEN_MIN    (7 * 60)
#define DEFAULT_CLOSE_MIN   (22 * 60)

// ---- Safety alerts
#define MISMATCH_GRACE_S    20

// ---- SD card (SPI2)
#define SD_PIN_MOSI         13
#define SD_PIN_MISO         33
#define SD_PIN_SCK          14
#define SD_PIN_CS           27

// ---- Rooms (the PIR pins 34 and 35 are input-only, which suits a sensor that drives its own output)
#define ROOM1_PIR           34
#define ROOM1_LIGHT         16
#define ROOM2_PIR           35
#define ROOM2_LIGHT         17
#define VACANCY_TIMEOUT_S   10
#define NODE_TIMEOUT_S      15

// ---- RFID readers (SPI3). Both RST pins are tied to 3V3; the code uses a soft reset.
#define RFID_PIN_SCK        22
#define RFID_PIN_MOSI       26
#define RFID_PIN_MISO       32
#define RFID_PIN_CS_ENTRY   23
#define RFID_PIN_CS_EXIT    4