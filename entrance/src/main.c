#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "config.h"
#include "storage.h"
#include "net.h"
#include "messages.h"
#include "types.h"
#include "jsonlite.h"

static const char *TAG = "ENTRANCE";
#define LOG_ADMIN(...) do { ESP_LOGW(TAG, __VA_ARGS__); log_systemf(__VA_ARGS__); } while (0)

// ---------------- users (editable, stored on the SD card) ----------------
#define MAX_USERS 16
typedef struct { char uid[16]; char name[16]; bool admin; bool inside; } user_t;
static user_t users[MAX_USERS];
static int n_users;

static const user_rec_t default_users[] = {
    { "A1B2C3D4", "Alice", false },
    { "11223344", "Bob",   false },
    { "DEADBEEF", "Admin", true  },
};
#define N_DEFAULTS (sizeof(default_users) / sizeof(default_users[0]))

typedef struct { bool entering; char uid[16]; } scan_evt_t;

// ---- shared state, all guarded by state_mx ----
static room_state_t rooms[2];                 // index 0 = Room 1
static int  sched_open  = DEFAULT_OPEN_MIN;   // minutes after midnight, local time
static int  sched_close = DEFAULT_CLOSE_MIN;
static bool emergency = false;
static bool session_active = false;
static char session_uid[16], session_name[16];
static TickType_t session_expiry = 0, lock_until = 0;
static int login_fails = 0;

#define N_NODES 3
static const char *node_names[N_NODES] = { "entrance", "room1", "room2" };
static int node_state[N_NODES] = { -1, -1, -1 };      // -1 unknown, 0 offline, 1 online
static TickType_t node_since[N_NODES];

static SemaphoreHandle_t state_mx;
static QueueHandle_t scan_q;
static volatile bool sys_dirty = true;        // system state needs publishing
static bool sntp_active = false;
static TickType_t alarm_until = 0;            // used only by the housekeeping task
static bool alerted_after_hours = false;
static bool alerted_mismatch = false;
static int  mismatch_secs = 0;

#define LOCK()   xSemaphoreTake(state_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(state_mx)

// ---------------- time helpers ----------------
static bool time_valid(void) { return time(NULL) > 1700000000; }

static int minutes_now(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static bool in_window(int now, int open, int close)
{
    if (open <= close) return now >= open && now < close;
    return now >= open || now < close;        // window crosses midnight
}

static void fmt_hm(int m, char *out) { sprintf(out, "%02d:%02d", m / 60, m % 60); }

static bool parse_hhmm(const char *s, int *out)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = h * 60 + m;
    return true;
}

static bool valid_token(const char *s, size_t maxlen)
{
    size_t n = strlen(s);
    if (n == 0 || n > maxlen) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

// ---------------- persistence ----------------
static void sched_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READONLY, &h) == ESP_OK) {
        uint16_t o, c;
        if (nvs_get_u16(h, "open", &o) == ESP_OK && nvs_get_u16(h, "close", &c) == ESP_OK) {
            sched_open = o;
            sched_close = c;
        }
        nvs_close(h);
    }
}

static void sched_save(int o, int c)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "open", (uint16_t)o);
        nvs_set_u16(h, "close", (uint16_t)c);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool users_save(void)
{
    user_rec_t tmp[MAX_USERS];
    int n;
    LOCK();
    n = n_users;
    for (int i = 0; i < n; i++) {
        snprintf(tmp[i].uid, sizeof tmp[i].uid, "%s", users[i].uid);
        snprintf(tmp[i].name, sizeof tmp[i].name, "%s", users[i].name);
        tmp[i].admin = users[i].admin;
    }
    UNLOCK();
    return storage_save_users(tmp, n);      // SD access happens outside our own lock
}

static void users_load(void)
{
    user_rec_t tmp[MAX_USERS];
    int n = storage_load_users(tmp, MAX_USERS);
    bool from_file = n > 0;
    const user_rec_t *src = from_file ? tmp : default_users;
    if (!from_file) n = (int)N_DEFAULTS;

    LOCK();
    n_users = 0;
    bool have_admin = false;
    for (int i = 0; i < n; i++) {
        snprintf(users[i].uid, sizeof users[i].uid, "%s", src[i].uid);
        snprintf(users[i].name, sizeof users[i].name, "%s", src[i].name);
        users[i].admin = src[i].admin;
        users[i].inside = false;
        if (src[i].admin) have_admin = true;
        n_users++;
    }
    if (!have_admin && n_users < MAX_USERS) {          // never start with nobody able to log in
        snprintf(users[n_users].uid, sizeof users[n_users].uid, "%s", default_users[2].uid);
        snprintf(users[n_users].name, sizeof users[n_users].name, "%s", default_users[2].name);
        users[n_users].admin = true;
        users[n_users].inside = false;
        n_users++;
    }
    UNLOCK();

    if (from_file) ESP_LOGI(TAG, "Loaded %d users from SD card", n);
    else if (storage_ready()) { users_save(); ESP_LOGI(TAG, "No user file found: wrote default users to SD card"); }
}

// ---------------- helpers ----------------
static user_t *find_user(const char *uid)               // lock held
{
    for (int i = 0; i < n_users; i++)
        if (strcasecmp(users[i].uid, uid) == 0) return &users[i];
    return NULL;
}

static int inside_count(void)                           // lock held
{
    int n = 0;
    for (int i = 0; i < n_users; i++) if (users[i].inside) n++;
    return n;
}

static bool is_admin_locked(void)                       // lock held
{
    if (!session_active || (int32_t)(session_expiry - xTaskGetTickCount()) <= 0) return false;
    user_t *u = find_user(session_uid);                 // re-check: rights may have been revoked
    return u && u->admin;
}

static bool require_admin(void)
{
    LOCK();
    bool ok = is_admin_locked();
    UNLOCK();
    if (!ok) printf("Not authorised. Use: login <admin_uid> <pin>\n");
    return ok;
}

static void pulse(int gpio, int ms)
{
    gpio_set_level(gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(gpio, 0);
}

static void publish_access(const char *dir, const char *uid, const char *name,
                           const char *result, int inside)
{
    char buf[200];
    snprintf(buf, sizeof buf,
             "{\"dir\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"result\":\"%s\",\"inside\":%d}",
             dir, uid, name, result, inside);
    net_publish(T_ACCESS_EVENT, buf, 1, 0);
}

static void publish_system(void)
{
    LOCK();
    int o = sched_open, c = sched_close;
    bool em = emergency;
    UNLOCK();
    char ob[8], cb[8], buf[96];
    fmt_hm(o, ob);
    fmt_hm(c, cb);
    snprintf(buf, sizeof buf, "{\"open\":\"%s\",\"close\":\"%s\",\"emergency\":%d}", ob, cb, em);
    net_publish(T_SYSTEM_STATE, buf, 1, 1);
}

static void raise_alert(const char *code, const char *detail)   // no commas in detail: it goes into a CSV
{
    char buf[160];
    snprintf(buf, sizeof buf, "{\"code\":\"%s\",\"detail\":\"%s\"}", code, detail);
    net_publish(T_ALERT, buf, 1, 0);
    LOG_ADMIN("ALERT %s: %s", code, detail);
    gpio_set_level(GPIO_ALARM, 1);
    alarm_until = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
}

// ---------------- access decisions: the only place that changes who is inside ----------------
static void access_task(void *p)
{
    scan_evt_t ev;
    while (1) {
        if (xQueueReceive(scan_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        const char *dir = ev.entering ? "in" : "out";
        char name[16] = "unknown";
        const char *result;
        int inside;
        int now_min = time_valid() ? minutes_now() : -1;

        LOCK();
        user_t *u = find_user(ev.uid);
        if (!u) {
            result = "denied_unknown_tag";
        } else {
            snprintf(name, sizeof name, "%s", u->name);
            if (ev.entering && u->inside) {
                result = "denied_already_inside";
            } else if (!ev.entering && !u->inside) {
                result = "denied_not_inside";
            } else if (!ev.entering && emergency) {
                u->inside = false;
                result = "granted_emergency";
            } else if (now_min < 0) {
                result = "denied_time_not_set";
            } else if (!in_window(now_min, sched_open, sched_close)) {
                result = ev.entering ? "denied_outside_hours" : "denied_exit_outside_hours";
            } else {
                u->inside = ev.entering;
                result = "granted";
            }
        }
        inside = inside_count();
        UNLOCK();

        bool granted = (strncmp(result, "granted", 7) == 0);
        if (granted) ESP_LOGI(TAG, "%s %s %s (%s), inside now: %d", result, dir, name, ev.uid, inside);
        else         ESP_LOGW(TAG, "DENIED %s %s (%s): %s", dir, name, ev.uid, result);

        publish_access(dir, ev.uid, name, result, inside);
        log_access(dir, ev.uid, name, result, inside);
        if (granted) pulse(ev.entering ? GPIO_DOOR_IN : GPIO_DOOR_OUT, DOOR_OPEN_MS);
        else         pulse(GPIO_DENIED, 500);
    }
}

// ---------------- housekeeping: system state, clock report, safety alerts ----------------
static void housekeeping_task(void *p)
{
    int n = 0;
    bool reported = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!reported && time_valid()) {
            reported = true;
            char b[24];
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
            ESP_LOGI(TAG, "Clock is set: %s", b);
        }

        if (alarm_until && (int32_t)(alarm_until - xTaskGetTickCount()) <= 0) {
            gpio_set_level(GPIO_ALARM, 0);
            alarm_until = 0;
        }

        // ---- safety checks ----
        LOCK();
        int inside = inside_count();
        bool room_occ = (rooms[0].occupied && node_state[1] == 1) ||     // ignore offline rooms
                        (rooms[1].occupied && node_state[2] == 1);
        int o = sched_open, c = sched_close;
        bool em = emergency;
        UNLOCK();

        // 1) people still inside after closing time (the exit rule keeps them in)
        if (time_valid() && !em) {
            bool open_now = in_window(minutes_now(), o, c);
            if (!open_now && inside > 0 && !alerted_after_hours) {
                alerted_after_hours = true;
                char d[64];
                snprintf(d, sizeof d, "%d person(s) still inside after closing", inside);
                raise_alert("after_hours_presence", d);
            }
            if (open_now || inside == 0) alerted_after_hours = false;
        }

        // 2) a room stays occupied while nobody is checked in
        if (room_occ && inside == 0 && !em) {
            if (++mismatch_secs >= MISMATCH_GRACE_S && !alerted_mismatch) {
                alerted_mismatch = true;
                raise_alert("unregistered_presence", "room occupied but nobody is checked in");
            }
        } else {
            mismatch_secs = 0;
            alerted_mismatch = false;
        }

        if (net_is_connected() && (sys_dirty || ++n >= 30)) {
            publish_system();
            sys_dirty = false;
            n = 0;
        }
    }
}

// ---------------- console: monitoring ----------------
static void submit_scan(bool entering, const char *uid)
{
    scan_evt_t ev = { .entering = entering };
    if (!valid_token(uid, sizeof ev.uid - 1)) { printf("Invalid tag id\n"); return; }
    strcpy(ev.uid, uid);
    if (xQueueSend(scan_q, &ev, 0) != pdTRUE) printf("Reader busy, try again\n");
}

static void print_status(void)
{
    LOCK();
    printf("Inside the building: %d\n", inside_count());
    for (int i = 0; i < n_users; i++)
        if (users[i].inside) printf("  %s (%s)\n", users[i].name, users[i].uid);
    printf("Admin session: %s\n", is_admin_locked() ? session_name : "none");
    UNLOCK();
}

static void print_rooms(void)
{
    static const char *mode_txt[] = { "auto", "forced ON", "forced OFF" };
    LOCK();
    room_state_t r[2] = { rooms[0], rooms[1] };
    UNLOCK();
    for (int i = 0; i < 2; i++) {
        int m = (r[i].mode >= 0 && r[i].mode <= 2) ? r[i].mode : 0;
        printf("Room %d: %s, light %s (%s)\n", i + 1,
               r[i].occupied ? "OCCUPIED" : "vacant", r[i].light ? "ON" : "off", mode_txt[m]);
    }
}

static void print_nodes(void)
{
    int st[N_NODES];
    TickType_t since[N_NODES];
    LOCK();
    for (int i = 0; i < N_NODES; i++) { st[i] = node_state[i]; since[i] = node_since[i]; }
    UNLOCK();
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < N_NODES; i++) {
        if (st[i] < 0) { printf("  %-9s unknown\n", node_names[i]); continue; }
        printf("  %-9s %s for %lu s\n", node_names[i], st[i] ? "ONLINE" : "OFFLINE",
               (unsigned long)((now - since[i]) / configTICK_RATE_HZ));
    }
}

static void print_time(void)
{
    if (!time_valid()) { printf("Clock not set yet (waiting for network time)\n"); return; }
    char b[24];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    printf("Time (UTC+3): %s\n", b);
}

static void print_schedule(void)
{
    LOCK();
    int o = sched_open, c = sched_close;
    bool em = emergency;
    UNLOCK();
    char ob[8], cb[8];
    fmt_hm(o, ob);
    fmt_hm(c, cb);
    printf("Access window: %s to %s", ob, cb);
    if (time_valid()) printf(" (now %s)", in_window(minutes_now(), o, c) ? "OPEN" : "CLOSED");
    printf("\nEmergency override: %s\n", em ? "ON" : "off");
}

// ---------------- console: authentication ----------------
static bool pin_ok(const char *p)
{
    const char *r = ADMIN_PIN;
    size_t lr = strlen(r), lp = strlen(p);
    unsigned d = (unsigned)(lr ^ lp);
    for (size_t i = 0; i < lr && i < lp; i++) d |= (unsigned)(r[i] ^ p[i]);
    return d == 0;
}

static void cmd_login(const char *uid, const char *pin)
{
    TickType_t now = xTaskGetTickCount();
    LOCK();
    if ((int32_t)(lock_until - now) > 0) {
        UNLOCK();
        printf("Login locked after failed attempts. Try again shortly.\n");
        return;
    }
    user_t *u = find_user(uid);
    bool ok = u && u->admin && pin_ok(pin);
    char name[16] = "";
    bool locked_now = false;
    if (ok) {
        session_active = true;
        snprintf(session_uid, sizeof session_uid, "%s", u->uid);
        snprintf(session_name, sizeof session_name, "%s", u->name);
        session_expiry = now + pdMS_TO_TICKS(SESSION_S * 1000);
        login_fails = 0;
        snprintf(name, sizeof name, "%s", u->name);
    } else if (++login_fails >= 3) {
        lock_until = now + pdMS_TO_TICKS(30000);
        login_fails = 0;
        locked_now = true;
    }
    UNLOCK();
    if (ok) {
        printf("Logged in as %s (%d s session)\n", name, SESSION_S);
        LOG_ADMIN("ADMIN login: %s", name);
    } else {
        printf("Login failed\n");
        LOG_ADMIN("ADMIN login FAILED for tag %s", valid_token(uid, 15) ? uid : "invalid");
        if (locked_now) LOG_ADMIN("ADMIN login locked for 30 s after repeated failures");
    }
}

static void cmd_logout(void)
{
    LOCK();
    session_active = false;
    UNLOCK();
    printf("Logged out\n");
}

// ---------------- console: admin actions ----------------
static void cmd_schedule_set(const char *a, const char *b)
{
    int o, c;
    if (!require_admin()) return;
    if (!parse_hhmm(a, &o) || !parse_hhmm(b, &c) || o == c) {
        printf("Usage: schedule set HH:MM HH:MM (open and close must differ)\n");
        return;
    }
    LOCK();
    sched_open = o;
    sched_close = c;
    UNLOCK();
    sched_save(o, c);
    sys_dirty = true;
    LOG_ADMIN("ADMIN changed schedule to %s - %s", a, b);
    print_schedule();
}

static void cmd_settime(const char *arg)
{
    int m;
    if (!require_admin()) return;
    if (!parse_hhmm(arg, &m)) { printf("Usage: settime HH:MM\n"); return; }

    struct tm tm;
    if (time_valid()) {
        time_t t = time(NULL);
        localtime_r(&t, &tm);
    } else {
        memset(&tm, 0, sizeof tm);
        tm.tm_year = 126; tm.tm_mon = 10; tm.tm_mday = 2;    // 2 Nov 2026
    }
    tm.tm_hour = m / 60;
    tm.tm_min = m % 60;
    tm.tm_sec = 0;
    tm.tm_isdst = 0;
    struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
    settimeofday(&tv, NULL);

    if (sntp_active) { esp_netif_sntp_deinit(); sntp_active = false; }   // stop network time overriding
    LOG_ADMIN("ADMIN set clock manually to %s", arg);
    print_time();
}

static void cmd_synctime(void)
{
    if (!require_admin()) return;
    if (!sntp_active) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) == ESP_OK) sntp_active = true;
    }
    LOG_ADMIN("ADMIN requested network time sync");
    printf("Network time sync requested. Type 'time' in a few seconds.\n");
}

static void cmd_emergency(const char *arg)
{
    if (!require_admin()) return;
    bool on;
    if (!strcasecmp(arg, "on")) on = true;
    else if (!strcasecmp(arg, "off")) on = false;
    else { printf("Usage: emergency on|off\n"); return; }
    LOCK();
    emergency = on;
    UNLOCK();
    sys_dirty = true;
    LOG_ADMIN("ADMIN emergency override %s", on ? "ON" : "OFF");
    print_schedule();
}

static void cmd_room(int n, const char *a, const char *b, const char *c)
{
    bool ok_args = (n == 4) && !strcasecmp(b, "light") && (!strcmp(a, "1") || !strcmp(a, "2")) &&
                   (!strcasecmp(c, "on") || !strcasecmp(c, "off") || !strcasecmp(c, "auto"));
    if (!ok_args) { printf("Usage: room <1|2> light on|off|auto\n"); return; }
    if (!require_admin()) return;

    const char *v = !strcasecmp(c, "on") ? "on" : (!strcasecmp(c, "off") ? "off" : "auto");
    char buf[40];
    snprintf(buf, sizeof buf, "{\"light\":\"%s\"}", v);
    net_publish(a[0] == '1' ? T_ROOM1_CMD : T_ROOM2_CMD, buf, 1, 0);
    LOG_ADMIN("ADMIN set room %s light to %s", a, v);
    printf("Command sent to room %s. Check 'rooms' in a moment.\n", a);
}

static void cmd_user_list(void)
{
    if (!require_admin()) return;
    LOCK();
    printf("%d user(s):\n", n_users);
    for (int i = 0; i < n_users; i++)
        printf("  %-9s %-15s %s%s\n", users[i].uid, users[i].name,
               users[i].admin ? "admin" : "user", users[i].inside ? "  (inside)" : "");
    UNLOCK();
}

static void cmd_user_add(const char *uid, const char *name, bool admin)
{
    if (!require_admin()) return;
    if (!valid_token(uid, 15) || !valid_token(name, 15)) {
        printf("Tag id and name: letters, digits, _ or - only, max 15 characters\n");
        return;
    }
    const char *err = NULL;
    LOCK();
    if (find_user(uid)) {
        err = "That tag is already registered";
    } else if (n_users >= MAX_USERS) {
        err = "User table is full";
    } else {
        user_t *u = &users[n_users++];
        snprintf(u->uid, sizeof u->uid, "%s", uid);
        snprintf(u->name, sizeof u->name, "%s", name);
        u->admin = admin;
        u->inside = false;
    }
    UNLOCK();
    if (err) { printf("%s\n", err); return; }

    bool saved = users_save();
    LOG_ADMIN("ADMIN added user %s (%s)%s", name, uid, admin ? " as admin" : "");
    printf("User added%s\n", saved ? "" : " (NOT saved: SD card unavailable, lost on reboot)");
}

static void cmd_user_del(const char *uid)
{
    if (!require_admin()) return;
    const char *err = NULL;
    char name[16] = "";
    LOCK();
    int idx = -1, admins = 0;
    for (int i = 0; i < n_users; i++) {
        if (strcasecmp(users[i].uid, uid) == 0) idx = i;
        if (users[i].admin) admins++;
    }
    if (idx < 0) {
        err = "No such user";
    } else if (users[idx].admin && admins <= 1) {
        err = "Cannot delete the last admin";
    } else {
        snprintf(name, sizeof name, "%s", users[idx].name);
        memmove(&users[idx], &users[idx + 1], (size_t)(n_users - idx - 1) * sizeof(user_t));
        n_users--;
    }
    UNLOCK();
    if (err) { printf("%s\n", err); return; }

    bool saved = users_save();
    LOG_ADMIN("ADMIN deleted user %s (%s)", name, uid);
    printf("User deleted%s\n", saved ? "" : " (NOT saved: SD card unavailable, lost on reboot)");
}

static void cmd_user(int n, const char *a, const char *b, const char *c, const char *d)
{
    if      (!strcasecmp(a, "list") && n == 2) cmd_user_list();
    else if (!strcasecmp(a, "add") && n >= 4)  cmd_user_add(b, c, n >= 5 && !strcasecmp(d, "admin"));
    else if (!strcasecmp(a, "del") && n == 3)  cmd_user_del(b);
    else printf("Usage: user list | user add <uid> <name> [admin] | user del <uid>\n");
}

// ---------------- console: menu and dispatch ----------------
static void print_help(void)
{
    printf("=== CoCIS entrance menu ===\n"
           "Reader (simulated):\n"
           "  in <uid> | out <uid>\n"
           "Monitor:\n"
           "  status | rooms | nodes | time | schedule\n"
           "  logs access|rooms|system [n] | sd\n"
           "Authentication:\n"
           "  login <admin_uid> <pin> | logout\n"
           "Admin only:\n"
           "  user list | user add <uid> <name> [admin] | user del <uid>\n"
           "  schedule set HH:MM HH:MM | settime HH:MM | synctime\n"
           "  emergency on|off\n"
           "  room <1|2> light on|off|auto\n");
}

static void handle_line(char *line)
{
    char cmd[16] = {0}, a[32] = {0}, b[32] = {0}, c[32] = {0}, d[32] = {0};
    int n = sscanf(line, "%15s %31s %31s %31s %31s", cmd, a, b, c, d);
    if (n < 1) return;

    if      (!strcasecmp(cmd, "in")  && n == 2) submit_scan(true,  a);
    else if (!strcasecmp(cmd, "out") && n == 2) submit_scan(false, a);
    else if (!strcasecmp(cmd, "status"))        print_status();
    else if (!strcasecmp(cmd, "rooms"))         print_rooms();
    else if (!strcasecmp(cmd, "nodes"))         print_nodes();
    else if (!strcasecmp(cmd, "time"))          print_time();
    else if (!strcasecmp(cmd, "login") && n == 3) cmd_login(a, b);
    else if (!strcasecmp(cmd, "logout"))        cmd_logout();
    else if (!strcasecmp(cmd, "schedule") && n == 1) print_schedule();
    else if (!strcasecmp(cmd, "schedule") && n == 4 && !strcasecmp(a, "set")) cmd_schedule_set(b, c);
    else if (!strcasecmp(cmd, "settime") && n == 2) cmd_settime(a);
    else if (!strcasecmp(cmd, "synctime"))      cmd_synctime();
    else if (!strcasecmp(cmd, "emergency") && n == 2) cmd_emergency(a);
    else if (!strcasecmp(cmd, "room") && n == 4) cmd_room(n, a, b, c);
    else if (!strcasecmp(cmd, "user") && n >= 2) cmd_user(n, a, b, c, d);
    else if (!strcasecmp(cmd, "logs") && n >= 2) storage_tail(a, n >= 3 ? atoi(b) : 10);
    else if (!strcasecmp(cmd, "sd"))            storage_status();
    else print_help();
}

static void serial_task(void *p)
{
    char line[80];
    int len = 0;
    printf("\nEntrance ready. Type 'menu' for commands.\n> ");
    fflush(stdout);
    while (1) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (len) {
                line[len] = 0;
                printf("\n");
                handle_line(line);
                len = 0;
            }
            printf("> ");
            fflush(stdout);
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

// ---------------- messages from other nodes (MQTT task) ----------------
static void on_msg(const char *topic, const char *data)
{
    int idx = -1;
    if (!strcmp(topic, T_ROOM1_STATE)) idx = 0;
    else if (!strcmp(topic, T_ROOM2_STATE)) idx = 1;

    if (idx >= 0) {
        int occ, light, mode = 0;
        if (jl_get_int(data, "occupied", &occ) && jl_get_int(data, "light", &light)) {
            jl_get_int(data, "mode", &mode);
            LOCK();
            bool changed = (rooms[idx].occupied != occ) || (rooms[idx].light != light) ||
                           (rooms[idx].mode != mode);
            rooms[idx].occupied = occ;
            rooms[idx].light = light;
            rooms[idx].mode = mode;
            UNLOCK();
            log_room(idx + 1, occ, light);          // the logger ignores unchanged heartbeats itself
            if (changed)
                ESP_LOGI(TAG, "Room %d update: occupied=%d light=%d mode=%d", idx + 1, occ, light, mode);
        }
    } else if (!strncmp(topic, T_STATUS_BASE, strlen(T_STATUS_BASE))) {
        const char *name = topic + strlen(T_STATUS_BASE);
        int st = !strcmp(data, "online") ? 1 : 0;
        for (int i = 0; i < N_NODES; i++) {
            if (strcmp(name, node_names[i]) != 0) continue;
            LOCK();
            bool changed = (node_state[i] != st);
            node_state[i] = st;
            if (changed) node_since[i] = xTaskGetTickCount();
            UNLOCK();
            if (changed) {
                ESP_LOGI(TAG, "Node '%s' is %s", name, data);
                log_systemf("node %s %s", name, st ? "online" : "offline");
            }
        }
    }
}

void app_main(void)
{
    state_mx = xSemaphoreCreateMutex();
    scan_q = xQueueCreate(8, sizeof(scan_evt_t));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_DOOR_IN) | (1ULL << GPIO_DOOR_OUT) |
                        (1ULL << GPIO_DENIED) | (1ULL << GPIO_ALARM),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(GPIO_DOOR_IN, 0);
    gpio_set_level(GPIO_DOOR_OUT, 0);
    gpio_set_level(GPIO_DENIED, 0);
    gpio_set_level(GPIO_ALARM, 0);

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    net_init(NODE_NAME, on_msg);              // also initialises NVS and the network stack
    net_subscribe(T_ROOM1_STATE);
    net_subscribe(T_ROOM2_STATE);
    net_subscribe(T_STATUS_ALL);

    setenv("TZ", "EAT-3", 1);                 // East Africa Time, UTC+3
    tzset();
    sched_load();
    storage_init();
    users_load();

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) sntp_active = true;

    xTaskCreate(access_task, "access", 4096, NULL, 5, NULL);
    xTaskCreate(housekeeping_task, "house", 4096, NULL, 3, NULL);
    xTaskCreate(serial_task, "serial", 6144, NULL, 4, NULL);
}