#pragma once
#include <stdbool.h>

typedef struct {
    int room_id;             
    const char *node_name;   
    int pir_gpio;
    int light_gpio;
    int vacancy_timeout_s;
} room_cfg_t;

void room_start(const room_cfg_t *cfg);
void room_fault(int room_id, bool failed);   