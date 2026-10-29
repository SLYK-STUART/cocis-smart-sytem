#pragma once
#include <stdbool.h>

// Called from the reader task for every card presented. entering is true for the ENTRY reader.
typedef void (*rfid_scan_cb_t)(bool entering, const char *uid_hex);

bool rfid_start(rfid_scan_cb_t cb);   // false if the readers could not be started