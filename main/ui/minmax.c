#include "minmax.h"
#include "theme.h"
#include "nav.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "MINMAX";

// The readings recorded, top to bottom on the page.
static const struct { signal_id_t sig; const char *name; const char *fmt; } ROWS[] = {
    { SIG_COOLANT,  "WATER",    "%.0f" },
    { SIG_OIL_TEMP, "OIL TEMP", "%.0f" },
    { SIG_OIL_PSI,  "OIL PSI",  "%.0f" },
    { SIG_IAT,      "IAT",      "%.0f" },
    { SIG_BATTERY,  "VOLTS",    "%.1f" },
};
#define N_ROWS (int)(sizeof ROWS / sizeof ROWS[0])

#define MM_NS      "minmax"
#define MM_KEY_CUR "cur"
#define MM_KEY_LAST "last"
// Bump when ROWS or the record layout changes: an old record is then ignored
// rather than read into the wrong rows.
#define MM_MAGIC   0x4D4D3032u           // "MM02"
// A warm-up moves the maxima every few seconds; this keeps NVS writes to a
// couple a minute. Losing up to this much at key-off is fine.
#define MINMAX_SAVE_MS 30000

// Nothing records until the engine has run this long: key-on-engine-off
// readings are not engine-health data, and oil pressure is still building.
#define SETTLE_MS  5000
// At shutdown the oil pressure and voltage fall before the RPM reading does,
// which recorded false minimums. So minimums are collected in windows and
// only committed once the engine is seen still running a full window later.
#define MIN_WIN_MS 1000

typedef struct {
    uint32_t magic;
    float    min[N_ROWS], max[N_ROWS];   // NAN = never seen
} record_t;

static record_t      s_cur, s_last;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty;

static lv_obj_t *s_scr;
static lv_obj_t *s_v[N_ROWS][4];        // this min, this max, last min, last max

static void blank(record_t *r)
{
    r->magic = MM_MAGIC;
    for (int i = 0; i < N_ROWS; i++) r->min[i] = r->max[i] = NAN;
}

static bool has_data(const record_t *r)
{
    for (int i = 0; i < N_ROWS; i++)
        if (!isnan(r->max[i])) return true;
    return false;
}

static bool load(nvs_handle_t h, const char *key, record_t *r)
{
    size_t n = sizeof *r;
    return nvs_get_blob(h, key, r, &n) == ESP_OK && n == sizeof *r && r->magic == MM_MAGIC;
}

void minmax_init(void)
{
    blank(&s_cur);
    blank(&s_last);

    nvs_handle_t h;
    if (nvs_open(MM_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, records will not persist");
        return;
    }
    // Promote the previous drive. An empty one (a bench boot with no car)
    // does not overwrite a real last drive.
    record_t prev;
    if (load(h, MM_KEY_CUR, &prev) && has_data(&prev)) {
        nvs_set_blob(h, MM_KEY_LAST, &prev, sizeof prev);
        nvs_erase_key(h, MM_KEY_CUR);
        nvs_commit(h);
        ESP_LOGI(TAG, "previous drive moved to last");
    }
    if (!load(h, MM_KEY_LAST, &s_last)) blank(&s_last);
    nvs_close(h);
}

static void take_min(int i, float v)
{
    if (!isnan(v) && (isnan(s_cur.min[i]) || v < s_cur.min[i])) { s_cur.min[i] = v; s_dirty = true; }
}

static void take_max(int i, float v)
{
    if (!isnan(v) && (isnan(s_cur.max[i]) || v > s_cur.max[i])) { s_cur.max[i] = v; s_dirty = true; }
}

void minmax_feed(const dash_frame_t *f)
{
    // Minimums: this window's, and the previous window's waiting to be
    // committed. A window counts only if the engine was settled throughout.
    static float   win[N_ROWS], held[N_ROWS];
    static bool    win_ok, held_ok, started;
    static int64_t win_start;

    int64_t now = esp_timer_get_time() / 1000;
    bool settled = f->run_ms >= SETTLE_MS;

    if (!started) {
        for (int i = 0; i < N_ROWS; i++) win[i] = held[i] = NAN;
        started = true;
    }
    if (!settled)
        win_ok = held_ok = false;
    else
        for (int i = 0; i < N_ROWS; i++) {
            float v = f->v[ROWS[i].sig];
            if (!isnan(v) && (isnan(win[i]) || v < win[i])) win[i] = v;
        }

    bool commit = false;
    float commit_min[N_ROWS];
    if (now - win_start >= MIN_WIN_MS) {
        commit = held_ok && win_ok;
        for (int i = 0; i < N_ROWS; i++) {
            commit_min[i] = held[i];
            held[i] = win[i];
            win[i] = NAN;
        }
        held_ok = win_ok;
        win_ok = settled;
        win_start = now;
    }

    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < N_ROWS; i++) {
        if (commit)  take_min(i, commit_min[i]);
        if (settled) take_max(i, f->v[ROWS[i].sig]);
    }
    taskEXIT_CRITICAL(&s_mux);
}

void minmax_save(void)
{
    static int64_t last_ms;
    int64_t now = esp_timer_get_time() / 1000;
    if (!s_dirty || now - last_ms < MINMAX_SAVE_MS) return;

    record_t copy;
    taskENTER_CRITICAL(&s_mux);
    copy = s_cur;
    s_dirty = false;
    taskEXIT_CRITICAL(&s_mux);

    nvs_handle_t h;
    if (nvs_open(MM_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, MM_KEY_CUR, &copy, sizeof copy);
    nvs_commit(h);
    nvs_close(h);
    last_ms = now;
}

// ---- screen ------------------------------------------------------------------
#define ROW_Y0  100
#define ROW_H    60
static const int COL_X[4] = { 300, 430, 590, 700 };

static void refresh_cb(lv_timer_t *t)
{
    if (lv_scr_act() != s_scr) return;          // nothing to draw off-screen
    record_t cur;
    taskENTER_CRITICAL(&s_mux);
    cur = s_cur;
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < N_ROWS; i++) {
        ui_set_num(s_v[i][0], cur.min[i],    ROWS[i].fmt);
        ui_set_num(s_v[i][1], cur.max[i],    ROWS[i].fmt);
        ui_set_num(s_v[i][2], s_last.min[i], ROWS[i].fmt);
        ui_set_num(s_v[i][3], s_last.max[i], ROWS[i].fmt);
    }
}

lv_obj_t *minmax_create(int index)
{
    s_scr = ui_screen();

    lv_obj_t *t = ui_label(s_scr, "MIN / MAX", &lv_font_montserrat_28, C_WHITE);
    lv_obj_set_pos(t, 20, 14);

    lv_obj_t *h = ui_label(s_scr, "THIS DRIVE", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(h, COL_X[0], 22);
    h = ui_label(s_scr, "LAST DRIVE", &lv_font_montserrat_20, C_DIM);
    lv_obj_set_pos(h, COL_X[2], 22);
    static const char *sub[4] = { "MIN", "MAX", "MIN", "MAX" };
    for (int c = 0; c < 4; c++) {
        h = ui_label(s_scr, sub[c], &lv_font_montserrat_14, c < 2 ? C_MUTED : C_DIM);
        lv_obj_set_pos(h, COL_X[c], 60);
    }

    for (int i = 0; i < N_ROWS; i++) {
        int y = ROW_Y0 + i * ROW_H;
        lv_obj_t *n = ui_label(s_scr, ROWS[i].name, &lv_font_montserrat_20, C_MUTED);
        lv_obj_set_pos(n, 20, y + 6);
        for (int c = 0; c < 4; c++) {
            bool now = c < 2;
            s_v[i][c] = ui_label(s_scr, "--",
                                 now ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                                 now ? C_WHITE : C_DIM);
            lv_obj_set_pos(s_v[i][c], COL_X[c], now ? y : y + 6);
        }
        if (i) {                                  // hairline between rows
            lv_obj_t *ln = lv_obj_create(s_scr);
            lv_obj_remove_style_all(ln);
            lv_obj_set_pos(ln, 20, y - 12);
            lv_obj_set_size(ln, 760, 1);
            lv_obj_set_style_bg_color(ln, lv_color_hex(C_TILE_LINE), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        }
    }

    nav_dots(s_scr, index);
    lv_timer_create(refresh_cb, 300, NULL);
    return s_scr;
}
