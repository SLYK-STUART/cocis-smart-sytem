#include "room.h"
#include "config.h"

void app_main(void)
{
    room_cfg_t cfg = {
        .room_id = ROOM_ID,
        .node_name = NODE_NAME,
        .pir_gpio = GPIO_PIR,
        .light_gpio = GPIO_LIGHT,
        .vacancy_timeout_s = VACANCY_TIMEOUT_S,
    };
    room_start(&cfg);
}