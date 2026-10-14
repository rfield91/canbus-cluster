#include "pid_scan.h"
#include "can_bus.h"
#include "vehicle.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "PIDSCAN";

#define SCAN_FIRST CONFIG_DASH_PID_SCAN_FIRST
#define SCAN_LAST  CONFIG_DASH_PID_SCAN_LAST

// The ECM answers a known identifier or rejects an unknown one (7F 22 31)
// within a few ms; silence past this means move on.
#define REPLY_TIMEOUT_MS   40
// 7F 22 78 is "busy, answer coming": allow this much longer.
#define PENDING_TIMEOUT_MS 300

// Raw first-byte windows to flag on screen. Empty by default; see pid_scan.h.
typedef struct { const char *name; uint8_t lo, hi; } target_t;
static const target_t TARGETS[] = {
    // { "OIL TEMP 200F", 131, 135 },     // (200 - 32) / 1.8 + 40 = 133
    { NULL, 0, 0 },
};

#define RECENT 12
#define MAX_FLAGGED 32

typedef struct { uint16_t pid; uint8_t n, b[4]; } hit_t;

// Written by the scan task, read by the LVGL timer under s_mux.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_cur = SCAN_FIRST;
static int      s_answers;
static bool     s_done;
static hit_t    s_recent[RECENT];
static hit_t    s_flagged[MAX_FLAGGED];
static int      s_nflagged;

static lv_obj_t *s_label;

// Next frame from the ECM, or false on timeout.
static bool recv_ecm(twai_message_t *m, int64_t deadline_us)
{
    for (;;) {
        int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return false;
        if (twai_receive(m, pdMS_TO_TICKS(left_ms)) != ESP_OK) return false;
        if (!m->extd && !m->rtr && m->identifier == ECM_RESP_ID) return true;
    }
}

static void record(uint16_t pid, const uint8_t *val, int n)
{
    char hex[3 * 8 + 1] = "";
    for (int i = 0; i < n && i < 8; i++)
        sprintf(hex + 3 * i, "%02X ", val[i]);
    ESP_LOGI(TAG, "HIT 22 %04X  len %d  %s", pid, n, hex);

    hit_t h = { .pid = pid, .n = n > 4 ? 4 : (n < 0 ? 0 : n) };
    memcpy(h.b, val, h.n);

    taskENTER_CRITICAL(&s_mux);
    s_answers++;
    memmove(s_recent + 1, s_recent, sizeof s_recent - sizeof s_recent[0]);
    s_recent[0] = h;
    for (int t = 0; TARGETS[t].name && n > 0; t++)
        if (val[0] >= TARGETS[t].lo && val[0] <= TARGETS[t].hi && s_nflagged < MAX_FLAGGED)
            s_flagged[s_nflagged++] = h;
    taskEXIT_CRITICAL(&s_mux);
}

static void scan_one(uint16_t pid)
{
    const uint8_t req[8] = { 0x03, 0x22, pid >> 8, pid & 0xFF, 0, 0, 0, 0 };
    can_bus_send(ECM_REQ_ID, req, 20);

    twai_message_t m;
    int64_t dl = esp_timer_get_time() + REPLY_TIMEOUT_MS * 1000;
    while (recv_ecm(&m, dl)) {
        const uint8_t *d = m.data;
        uint8_t pci = d[0] >> 4;

        if (pci == 0 && d[1] == 0x62 && d[2] == (pid >> 8) && d[3] == (pid & 0xFF)) {
            record(pid, d + 4, (d[0] & 0x0F) - 3);          // single frame
            return;
        }
        if (pci == 1 && d[2] == 0x62 && d[3] == (pid >> 8) && d[4] == (pid & 0xFF)) {
            // Long answer. Only the first frame is read (no flow control is
            // sent, so the ECM gives up on the rest) -- enough to spot it.
            int total = ((d[0] & 0x0F) << 8 | d[1]) - 3;
            ESP_LOGI(TAG, "HIT 22 %04X  multi-frame, %d bytes total", pid, total);
            record(pid, d + 5, 3);
            return;
        }
        if (pci == 0 && d[1] == 0x7F && d[2] == 0x22) {
            if (d[3] != 0x78) return;                         // rejected
            dl = esp_timer_get_time() + PENDING_TIMEOUT_MS * 1000;
        }
    }
}

static void scan_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));          // let the bus settle after start
    int64_t t0 = esp_timer_get_time();
    for (uint32_t pid = SCAN_FIRST; pid <= SCAN_LAST; pid++) {
        taskENTER_CRITICAL(&s_mux);
        s_cur = pid;
        taskEXIT_CRITICAL(&s_mux);
        scan_one(pid);
    }
    taskENTER_CRITICAL(&s_mux);
    s_done = true;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "DONE in %llds: %d identifiers answered",
             (esp_timer_get_time() - t0) / 1000000, s_answers);
    vTaskDelete(NULL);
}

static int fmt_hit(char *out, int n, const hit_t *h)
{
    int k = snprintf(out, n, "22 %04X:", h->pid);
    for (int i = 0; i < h->n && k < n; i++)
        k += snprintf(out + k, n - k, " %02X", h->b[i]);
    return k;
}

static void ui_cb(lv_timer_t *t)
{
    static char buf[2048];
    static hit_t recent[RECENT], flagged[MAX_FLAGGED];

    taskENTER_CRITICAL(&s_mux);
    uint16_t cur = s_cur;
    int answers = s_answers, nflagged = s_nflagged;
    bool done = s_done;
    memcpy(recent, s_recent, sizeof recent);
    memcpy(flagged, s_flagged, sizeof flagged);
    taskEXIT_CRITICAL(&s_mux);

    int span = SCAN_LAST - SCAN_FIRST + 1;
    int pct  = (cur - SCAN_FIRST + 1) * 100 / span;
    int n = snprintf(buf, sizeof buf, "PID SCAN  %s   22 %04X  %d%%   answered %d\n",
                     done ? "DONE" : "running", cur, pct, answers);

    if (TARGETS[0].name) {
        n += snprintf(buf + n, sizeof buf - n, "\nFlagged (%d):\n", nflagged);
        for (int i = 0; i < nflagged && n < (int)sizeof buf - 40; i++) {
            n += fmt_hit(buf + n, sizeof buf - n, &flagged[i]);
            n += snprintf(buf + n, sizeof buf - n, "    ");
        }
        n += snprintf(buf + n, sizeof buf - n, "\n");
    }
    n += snprintf(buf + n, sizeof buf - n, "\nMost recent:\n");
    for (int i = 0; i < RECENT && i < answers && n < (int)sizeof buf - 40; i++) {
        n += fmt_hit(buf + n, sizeof buf - n, &recent[i]);
        n += snprintf(buf + n, sizeof buf - n, "\n");
    }
    ui_set_text_if(s_label, buf);
}

void pid_scan_start(void)
{
    lv_obj_t *scr = ui_screen();
    s_label = ui_label(scr, "", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_width(s_label, 780);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_label, 10, 10);
    lv_scr_load(scr);
    lv_timer_create(ui_cb, 250, NULL);

    xTaskCreatePinnedToCore(scan_task, "pid_scan", 4096, NULL, 10, NULL, 0);
}
