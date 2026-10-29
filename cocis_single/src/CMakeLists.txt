FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_event esp_netif nvs_flash fatfs sdmmc
)