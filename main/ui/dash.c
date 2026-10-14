#include "dash.h"

#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "alarm.h"
#include "fastbar.h"
#include "theme.h"
#include "vehicle.h"

LV_FONT_DECLARE(ui_font_rpm_96);   // 96px digits and '-' only
LV_IMG_DECLARE(c5r_blue_boot);

// ---- what is shown -----------------------------------------------------------
// One tile per row, laid out left to right along the bottom. A row with a
// takeover title also gets the full-screen alert: once its alarm has held for
// takeover_ms it covers whatever page is showing until it clears or is tapped.
// takeover_rank orders them when more than one is due; 1 is shown first.

typedef struct {
    signal_id_t    sig;
    const char    *caption;
    const char    *fmt;
    alarm_limits_t lim;
    const char    *takeover;       // NULL = tile warning only
    const char    *unit;
    int            takeover_ms;
    int            takeover_rank;
} tile_def_t;

// Limits, as hi/clear or lo/clear pairs. A side without a limit is NAN.
#define HI(trip, clear) .hi = (trip), .hi_clear = (clear)
#define LO(trip, clear) .lo = (trip), .lo_clear = (clear)
#define NO_HI           HI(NAN, NAN)
#define NO_LO           LO(NAN, NAN)

static const tile_def_t TILES[] = {
    { .sig = SIG_COOLANT, .caption = "WATER", .fmt = "%.0f",
      .lim = { HI(245, 240), NO_LO },
      .takeover = "COOLANT HOT", .unit = "F", .takeover_ms = 5000, .takeover_rank = 2 },

    { .sig = SIG_OIL_TEMP, .caption = "OIL TEMP", .fmt = "%.0f",
      .lim = { HI(290, 285), NO_LO },
      .takeover = "OIL TEMP HIGH", .unit = "F", .takeover_ms = 5000, .takeover_rank = 3 },

    // Oil pressure kills an engine in seconds, so its alert comes quickest:
    // 1s of sustained low pressure trips the tile, 3.5s more the takeover.
    { .sig = SIG_OIL_PSI, .caption = "OIL PSI", .fmt = "%.0f",
      .lim = { NO_HI, LO(10, 13), .lo_needs_running = true, .sustain_ms = 1000 },
      .takeover = "LOW OIL PRESSURE", .unit = "PSI", .takeover_ms = 3500, .takeover_rank = 1 },

    { .sig = SIG_IAT, .caption = "IAT", .fmt = "%.0f",
      .lim = { HI(170, 165), NO_LO } },

    // Running, the alternator holds ~13.5-14.5 V. Low means it is not
    // charging; high means the regulator has failed.
    { .sig = SIG_BATTERY, .caption = "VOLTS", .fmt = "%.1f",
      .lim = { HI(15.2f, 14.9f), LO(12.5f, 12.8f), .lo_needs_running = true,
               .sustain_ms = 3000 } },
};
#define N_TILES (int)(sizeof TILES / sizeof TILES[0])

// ---- layout (800 x 480) --------------------------------------------------------
#define BAR_X        20
#define BAR_Y        10
#define BAR_W       760
#define BAR_H        54

#define BIG_CY      240    // MPH and gear digits: centred on the screen
#define CAPTION_GAP  16    // digits to the caption under them
#define MPH_CX      230    // centre lines of the two big numbers
#define GEAR_CX     570

#define TILE_H      120
#define TILE_GAP     10
#define TILE_Y      (480 - 10 - TILE_H)
#define TILE_BORDER_W       2
#define TILE_WARN_BORDER_W  4
#define TILE_WARN_RING_W    4

#define WARN_FLASH_MS   450
// From VEHICLE_SHIFT_RPM the bar blinks at this period. Each toggle repaints
// the whole bar: affordable at 10 Hz, not every frame.
#define SHIFT_BLINK_MS  100
// The bar moves in steps this big, so jitter does not redraw it every frame.
#define RPM_STEP         25

typedef struct {
    lv_obj_t     *tile, *val;
    alarm_state_t alarm;
    bool          acked;    // takeover dismissed; re-arms when the alarm clears
    float         last;
} tile_t;

static tile_t    s_t[N_TILES];
static lv_obj_t *s_dash, *s_boot;
static lv_obj_t *s_rpm_bar, *s_mph, *s_gear, *s_no_can;
static bool      s_flash_phase, s_shift, s_shift_on;
static int       s_rpm_last = -1;

static lv_obj_t *s_to, *s_to_title, *s_to_val, *s_to_unit;
static int       s_to_k = -1;           // tile index on the takeover, -1 none

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---- tiles -------------------------------------------------------------------
static void make_tile(int i, int x, int w)
{
    lv_obj_t *t = lv_obj_create(s_dash);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, TILE_H);
    lv_obj_set_pos(t, x, TILE_Y);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE_BG), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(C_TILE_LINE), 0);
    lv_obj_set_style_border_width(t, TILE_BORDER_W, 0);
    // Warning ring, invisible until the tile alarms.
    lv_obj_set_style_outline_width(t, TILE_WARN_RING_W, 0);
    lv_obj_set_style_outline_pad(t, 0, 0);
    lv_obj_set_style_outline_color(t, lv_color_hex(C_RED), 0);
    lv_obj_set_style_outline_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = ui_label(t, TILES[i].caption, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *v = ui_label(t, "--", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_t[i] = (tile_t){ .tile = t, .val = v, .last = NAN };
}

static void paint_warning(int i, bool red)
{
    lv_obj_set_style_outline_opa(s_t[i].tile, red ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_t[i].tile, red ? TILE_WARN_BORDER_W : TILE_BORDER_W, 0);
    lv_obj_set_style_border_color(s_t[i].tile, lv_color_hex(red ? C_RED : C_TILE_LINE), 0);
}

static void flash_cb(lv_timer_t *tm)
{
    s_flash_phase = !s_flash_phase;
    if (s_to_k >= 0)
        lv_obj_set_style_bg_color(s_to, lv_color_hex(s_flash_phase ? C_RED : 0x000000), 0);
    for (int i = 0; i < N_TILES; i++)
        if (s_t[i].alarm.on) paint_warning(i, s_flash_phase);
}

// ---- takeover ----------------------------------------------------------------
static void takeover_ack_cb(lv_event_t *e)
{
    if (s_to_k >= 0) s_t[s_to_k].acked = true;
}

// On the top layer, so it covers every page and survives page changes.
static void build_takeover(void)
{
    s_to = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_to);
    lv_obj_set_size(s_to, 800, 480);
    lv_obj_set_style_bg_opa(s_to, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_to, lv_color_hex(C_RED), 0);
    lv_obj_add_flag(s_to, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_to, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_to, takeover_ack_cb, LV_EVENT_CLICKED, NULL);

    s_to_title = ui_label(s_to, "", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align(s_to_title, LV_ALIGN_TOP_MID, 0, 70);
    s_to_val = ui_label(s_to, "", &ui_font_rpm_96, C_WHITE);
    lv_obj_align(s_to_val, LV_ALIGN_CENTER, -30, 10);
    s_to_unit = ui_label(s_to, "", &lv_font_montserrat_44, C_WHITE);
    lv_obj_t *h = ui_label(s_to, "TAP TO ACKNOWLEDGE", &lv_font_montserrat_20, C_WHITE);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_flag(s_to, LV_OBJ_FLAG_HIDDEN);
}

static void update_takeover(int64_t now)
{
    int show = -1;
    for (int i = 0; i < N_TILES; i++) {
        tile_t *t = &s_t[i];
        if (!t->alarm.on) { t->acked = false; continue; }
        if (!TILES[i].takeover || t->acked || now - t->alarm.on_since < TILES[i].takeover_ms)
            continue;
        if (show < 0 || TILES[i].takeover_rank < TILES[show].takeover_rank)
            show = i;
    }
    if (show != s_to_k) {
        s_to_k = show;
        if (show < 0) {
            lv_obj_add_flag(s_to, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        ui_set_text_if(s_to_title, TILES[show].takeover);
        ui_set_text_if(s_to_unit,  TILES[show].unit);
        lv_obj_clear_flag(s_to, LV_OBJ_FLAG_HIDDEN);
    }
    if (show >= 0) {
        ui_set_num(s_to_val, s_t[show].last, TILES[show].fmt);
        lv_obj_align_to(s_to_unit, s_to_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -10);
    }
}

// ---- build -------------------------------------------------------------------
static void shift_cb(lv_timer_t *t)
{
    if (!s_shift) return;
    s_shift_on = !s_shift_on;
    // Off is the track colour, so the bar visibly empties: a real flash
    // rather than a colour shift.
    fastbar_set_fill(s_rpm_bar, s_shift_on ? C_SHIFT : C_TRACK);
}

static void build_rpm_bar(void)
{
    // A straight bar rather than an arc: an arc sweeps a much larger dirty
    // rectangle every frame, a bar only redraws the strip that moved.
    s_rpm_bar = fastbar_create(s_dash, BAR_X, BAR_Y, BAR_W, BAR_H, VEHICLE_RPM_MAX,
                               C_TRACK, C_GREEN, 6);

    // Red line under the bar from the shift point up.
    lv_obj_t *rl = lv_obj_create(s_dash);
    lv_obj_remove_style_all(rl);
    int x0 = BAR_X + BAR_W * VEHICLE_SHIFT_RPM / VEHICLE_RPM_MAX;
    lv_obj_set_pos(rl, x0, BAR_Y + BAR_H + 2);
    lv_obj_set_size(rl, BAR_X + BAR_W - x0, 4);
    lv_obj_set_style_bg_color(rl, lv_color_hex(C_RED), 0);
    lv_obj_set_style_bg_opa(rl, LV_OPA_COVER, 0);

    // A label every 1000 rpm, at its true place on the bar, so the full scale
    // need not be a round thousand.
    for (int k = 0; k * 1000 <= VEHICLE_RPM_MAX; k++) {
        char t[4];
        snprintf(t, sizeof t, "%d", k);
        lv_obj_t *l = ui_label(s_dash, t, &lv_font_montserrat_20, C_MUTED);
        int x = BAR_X + BAR_W * k * 1000 / VEHICLE_RPM_MAX - 5;
        if (x > BAR_X + BAR_W - 12) x = BAR_X + BAR_W - 12;   // keep on screen
        lv_obj_set_pos(l, x, BAR_Y + BAR_H + 8);
    }

    // Over the bar, which is empty anyway when the car is not answering.
    s_no_can = ui_label(s_dash, "NO CAN", &lv_font_montserrat_28, C_RED);
    lv_obj_align(s_no_can, LV_ALIGN_TOP_MID, 0, BAR_Y + (BAR_H - 28) / 2);
    lv_obj_add_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *big_number(const char *caption, int cx)
{
    lv_obj_t *v = ui_label(s_dash, "--", &ui_font_rpm_96, C_WHITE);
    lv_obj_align(v, LV_ALIGN_CENTER, cx - 400, BIG_CY - 240);
    // Placed from the font's height rather than the label's, which is not
    // laid out yet.
    lv_obj_t *c = ui_label(s_dash, caption, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(c, LV_ALIGN_TOP_MID, cx - 400,
                 BIG_CY + ui_font_rpm_96.line_height / 2 + CAPTION_GAP);
    return v;
}

static void build_dash(void)
{
    s_dash = ui_screen();
    build_rpm_bar();
    s_mph  = big_number("MPH",  MPH_CX);
    s_gear = big_number("GEAR", GEAR_CX);

    int w = (800 - 2 * 10 - (N_TILES - 1) * TILE_GAP) / N_TILES;
    for (int i = 0; i < N_TILES; i++)
        make_tile(i, 10 + i * (w + TILE_GAP), w);

    lv_timer_create(flash_cb, WARN_FLASH_MS, NULL);
    lv_timer_create(shift_cb, SHIFT_BLINK_MS, NULL);
}

static void boot_done_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    // No fade: a fade redraws all 384k pixels on every step.
    lv_scr_load(s_dash);
    lv_obj_del(s_boot);          // the logo is 500 KB of draw state; free it
    s_boot = NULL;
}

lv_obj_t *dash_screen(void) { return s_dash; }

void dash_create(uint32_t boot_ms)
{
    build_dash();
    build_takeover();

    s_boot = ui_screen();
    lv_obj_set_style_bg_color(s_boot, lv_color_black(), 0);
    lv_obj_t *img = lv_img_create(s_boot);
    lv_img_set_src(img, &c5r_blue_boot);
    lv_obj_center(img);
    lv_scr_load(s_boot);

    lv_timer_create(boot_done_cb, boot_ms, NULL);
}

// ---- update ------------------------------------------------------------------
static void update_rpm(float rpm)
{
    int q = isnan(rpm) ? 0 : ((int)rpm / RPM_STEP) * RPM_STEP;
    if (q == s_rpm_last) return;
    s_rpm_last = q;
    fastbar_set_value(s_rpm_bar, q);
    // Entering the shift band starts the blink lit; leaving it puts the bar
    // straight back to green. The blink itself runs in shift_cb.
    bool shift = q >= VEHICLE_SHIFT_RPM;
    if (shift != s_shift) {
        s_shift = shift;
        s_shift_on = shift;
        fastbar_set_fill(s_rpm_bar, shift ? C_SHIFT : C_GREEN);
    }
}

void dash_update(const dash_frame_t *f)
{
    int64_t now = now_ms();

    if (f->no_can == lv_obj_has_flag(s_no_can, LV_OBJ_FLAG_HIDDEN)) {
        if (f->no_can) lv_obj_clear_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
    }

    update_rpm(f->rpm_bar);

    float mph = f->v[SIG_SPEED];
    ui_set_num(s_mph, isnan(mph) ? NAN : roundf(mph), "%.0f");

    char b[12];
    if (f->gear > 0) snprintf(b, sizeof b, "%d", f->gear);
    else             snprintf(b, sizeof b, "-");
    ui_set_text_if(s_gear, b);

    bool running = f->run_ms >= DASH_ALARM_START_MS;
    for (int i = 0; i < N_TILES; i++) {
        float v = f->v[TILES[i].sig];
        s_t[i].last = v;
        ui_set_num(s_t[i].val, v, TILES[i].fmt);
        bool was = s_t[i].alarm.on;
        if (alarm_update(&s_t[i].alarm, &TILES[i].lim, v, running, now) != was && was)
            paint_warning(i, false);
    }

    update_takeover(now);
}
