#pragma once
#include <stdbool.h>

// Called from the MQTT task: topic and data are null-terminated copies.
typedef void (*net_msg_cb_t)(const char *topic, const char *data);

// node_name e.g. "entrance", "room1". Sets the MQTT client id and the last-will topic.
void net_init(const char *node_name, net_msg_cb_t cb);
void net_subscribe(const char *topic);     // safe to call before connecting
int  net_publish(const char *topic, const char *payload, int qos, int retain);
bool net_is_connected(void);