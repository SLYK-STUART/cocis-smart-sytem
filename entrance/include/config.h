#pragma once
#define NODE_NAME      "entrance"
#define GPIO_DOOR_IN   18
#define GPIO_DOOR_OUT  19
#define GPIO_DENIED    21
#define DOOR_OPEN_MS   2000

#define ADMIN_PIN           "4321"
#define SESSION_S           120
#define DEFAULT_OPEN_MIN    (7 * 60)
#define DEFAULT_CLOSE_MIN  (22 * 60)

// SD card (SPI) - pins chosen to avoid the door LEDs and strapping pins
#define SD_PIN_MOSI  13
#define SD_PIN_MISO  33
#define SD_PIN_SCK   14
#define SD_PIN_CS    27

#define GPIO_ALARM          25
#define MISMATCH_GRACE_S    20