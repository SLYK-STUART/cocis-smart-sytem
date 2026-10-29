#pragma once
#include <stdbool.h>

bool storage_init(void);        // mounts the SD card and starts the logger task
bool storage_ready(void);

void log_access(const char *dir, const char *uid, const char *name,
                const char *result, int inside);
void log_room(int room, int occupied, int light);        // writes only when the state changed
void log_systemf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void storage_tail(const char *kind, int n); 
typedef struct { char uid[16]; char name[16]; bool admin; } user_rec_t;
int storage_load_users(user_rec_t *out, int max);
bool storage_save_users(const user_rec_t *in, int n);             // print last n lines: access | rooms | system
void storage_status(void);