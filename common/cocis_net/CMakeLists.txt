idf_component_register(SRCS "net.c" "room.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__mqtt esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio)