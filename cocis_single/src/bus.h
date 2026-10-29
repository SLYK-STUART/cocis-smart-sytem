#pragma once
#include <stdbool.h>

// Callbacks run in the bus task: topic and data are null-terminated.
typedef void (*bus_cb_t)(void *arg, const char *topic, const char *data);

void bus_init(void);
// filter: an exact topic, or a prefix ending in "/#"
int  bus_subscribe(const char *filter, bus_cb_t cb, void *arg);
bool bus_publish(const char *topic, const char *payload, bool retain);