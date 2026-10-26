#pragma once

typedef struct {
    int room_id;             // 1 or 2
    const char *node_name;   // "room1" / "room2"
    int pir_gpio;
    int light_gpio;
    int vacancy_timeout_s;   // seconds without motion before the room counts as vacant
} room_cfg_t;

void room_start(const room_cfg_t *cfg);   // sets up GPIO, Wi-Fi/MQTT and the room task