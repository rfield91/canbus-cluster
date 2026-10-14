#include "dash.h"

#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "limits.h"
#include "theme.h"
#include "vehicle.h"

LV_IMG_DECLARE(boot_logo);

// ---- what is shown -----------------------------------------------------------
// One entry per reading along the bottom, left to right. Any limit left out is
// off. A reading with a takeover title also gets the full-screen alert: once
// its alarm has held for takeover_ms it covers whatever page is showing until
// it clears or is tapped. takeover_rank orders them when more than one is due;
// 1 is shown first.
typedef struct {
    signal_id_t sig;
    const char *caption;
    const char *unit;
    const char *fmt;
    limits_t    lim;
    const char *takeover;          // NULL = colour change only
    int         takeover_ms;
    int         takeover_rank;
} tile_def_t;

static const tile_def_t TILES[] = {
    { .sig = SIG_COOLANT, .caption = "WATER", .unit = "°F", .fmt = "%.0f",
      .lim = { .cold = LIMIT(160), .warn_hi = LIMIT(220), .alarm_hi = LIMIT(230),
               .hyst = 5 },
      .takeover = "COOLANT HOT", .takeover_ms = 5000, .takeover_rank = 2 },

    { .sig = SIG_OIL_TEMP, .caption = "OIL TEMP", .unit = "°F", .fmt = "%.0f",
      .lim = { .cold = LIMIT(150), .warn_hi = LIMIT(260), .alarm_hi = LIMIT(290),
               .hyst = 5 },
      .takeover = "OIL TEMP HIGH", .takeover_ms = 5000, .takeover_rank = 3 },

    // Oil pressure kills an engine in seconds, so its alert comes quickest:
    // 1s of sustained low pressure trips it, 3.5s more the takeover.
    { .sig = SIG_OIL_PSI, .caption = "OIL PSI", .unit = "psi", .fmt = "%.0f",
      .lim = { .warn_lo = LIMIT(30), .alarm_lo = LIMIT(20), .hyst = 3,
               .lo_needs_running = true, .sustain_ms = 1000 },
      .takeover = "LOW OIL PRESSURE", .takeover_ms = 3500, .takeover_rank = 1 },

    { .sig = SIG_IAT, .caption = "IAT", .unit = "°F", .fmt = "%.0f",
      .lim = { .warn_hi = LIMIT(150), .alarm_hi = LIMIT(170), .hyst = 5 } },

    // Running, the alternator holds ~13.5-14.5 V. Low means it is not
    // charging; high means the regulator has failed.
    { .sig = SIG_BATTERY, .caption = "VOLTS", .unit = "V", .fmt = "%.1f",
      .lim = { .warn_lo = LIMIT(12.8f), .alarm_lo = LIMIT(12.0f),
               .warn_hi = LIMIT(14.9f), .alarm_hi = LIMIT(15.5f), .hyst = 0.2f,
               .lo_needs_running = true, .sustain_ms = 3000 } },
};
#define N_TILES (int)(sizeof TILES / sizeof TILES[0])

// ---- layout (800 x 480) --------------------------------------------------------
#define MARGIN       20
#define STRIP_Y      16
#define STRIP_H      34
#define N_SEG        30          // tach segments
#define SEG_GAP       5
#define TICK_Y       56

// MPH sits centred in the space between the tach and the readings.
#define DIGITS_BASE 240          // bottom of the MPH digits
#define CAPTION_Y   254
#define HUNDREDS_GAP  6          // MPH hundreds "1" ink to the tens cell; the
                                 // tens digit adds its own side bearing (~7 px)

#define ROW_RULE_Y  338          // hairline above the readings
#define ROW_Y       350
#define ROW_H       116

#define WARN_FLASH_MS   450
#define SHIFT_BLINK_MS  100      // the strip at the shift point

#define CELL_INSET        4      // a filled reading stops short of the hairlines
#define CELL_RADIUS       8

typedef struct {
    lv_obj_t     *cell, *cap, *val, *unit;
    level_state_t state;
    level_t       level;
    int           painted; // fill shown, -1 = never drawn
    bool          acked;   // takeover dismissed; re-arms when the alarm clears
    float         last;
} tile_t;

static tile_t    s_t[N_TILES];
static lv_obj_t *s_dash, *s_boot;
static lv_obj_t *s_seg[N_SEG];
static int       s_seg_state[N_SEG];          // colour shown, -1 = never drawn
static lv_obj_t *s_no_data;
static bool      s_flash, s_blink;

// MPH: one label per digit cell, hundreds first.
static lv_obj_t *s_mph[3];
static uint32_t  s_mph_col[3];
static int       s_mph_x0;        // the hundreds cell
static int       s_mph_one_x;     // where a hundreds "1" goes instead

static lv_obj_t *s_to, *s_to_title, *s_to_val, *s_to_unit;
static int       s_to_k = -1;           // tile index on the takeover, -1 none

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static uint32_t rpm_colour(float rpm)
{
    return rpm >= VEHICLE_SHIFT_RPM ? C_RED : rpm >= VEHICLE_WARN_RPM ? C_AMBER : C_GREEN;
}

// The fill a reading's cell shows: 0 for none. An alarm flashes between red
// and the normal look.
static uint32_t level_fill(level_t l)
{
    switch (l) {
        case LEVEL_COLD:  return C_BLUE;
        case LEVEL_WARN:  return C_AMBER;
        case LEVEL_ALARM: return s_flash ? C_RED : 0;
        default:          return 0;
    }
}

// The fill's own hue at a quarter brightness: dark enough to read on it, but
// still visibly secondary to the black value.
static uint32_t shade(uint32_t c)
{
    return (((c >> 16 & 0xFF) / 4) << 16) | (((c >> 8 & 0xFF) / 4) << 8) | ((c & 0xFF) / 4);
}

static void paint_tile(tile_t *t)
{
    int fill = (int)level_fill(t->level);
    if (fill == t->painted) return;            // a style write redraws, even unchanged
    t->painted = fill;
    lv_obj_set_style_bg_opa(t->cell, fill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (fill) lv_obj_set_style_bg_color(t->cell, lv_color_hex(fill), 0);
    // Every fill is bright, so black reads best on each: white on the blue
    // would be under 3:1.
    lv_obj_set_style_text_color(t->val,  lv_color_hex(fill ? C_INK : C_WHITE), 0);
    lv_obj_set_style_text_color(t->cap,  lv_color_hex(fill ? shade(fill) : C_MUTED), 0);
    lv_obj_set_style_text_color(t->unit, lv_color_hex(fill ? shade(fill) : C_MUTED), 0);
}

static lv_obj_t *rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *caption(lv_obj_t *p, const char *txt)
{
    lv_obj_t *l = ui_label(p, txt, &font_text_18, C_MUTED);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

// ---- tach strip ----------------------------------------------------------------
// Each segment lights at its own rpm and keeps its own zone colour, like a
// row of shift lights. Only segments that change are redrawn, so a sweep
// costs a few small rectangles per frame.
static int seg_rpm(int i) { return (i + 1) * VEHICLE_RPM_MAX / N_SEG; }

static void build_strip(void)
{
    int w = (800 - 2 * MARGIN - (N_SEG - 1) * SEG_GAP) / N_SEG;
    int x0 = (800 - (N_SEG * w + (N_SEG - 1) * SEG_GAP)) / 2;
    for (int i = 0; i < N_SEG; i++) {
        s_seg[i] = rect(s_dash, x0 + i * (w + SEG_GAP), STRIP_Y, w, STRIP_H, C_SEG_OFF);
        lv_obj_set_style_radius(s_seg[i], 4, 0);
        s_seg_state[i] = -1;
    }
    // Ticks every 500 rpm, long at each thousand, in the colour of their zone.
    for (int r = 0; r <= VEHICLE_RPM_MAX; r += 500) {
        int x = MARGIN + (800 - 2 * MARGIN) * r / VEHICLE_RPM_MAX;
        bool major = r % 1000 == 0;
        uint32_t c = r >= VEHICLE_SHIFT_RPM ? C_RED : r >= VEHICLE_WARN_RPM ? C_AMBER : C_DIM;
        rect(s_dash, x > 800 - MARGIN - 2 ? 800 - MARGIN - 2 : x, TICK_Y, 2, major ? 12 : 6, c);
    }
    // Over the strip, which is dark anyway while the car is not answering.
    s_no_data = ui_label(s_dash, "WAITING FOR ECM", &font_text_18, C_AMBER);
    lv_obj_set_style_text_letter_space(s_no_data, 3, 0);
    lv_obj_align(s_no_data, LV_ALIGN_TOP_MID, 0, STRIP_Y + (STRIP_H - 20) / 2);
    lv_obj_add_flag(s_no_data, LV_OBJ_FLAG_HIDDEN);
}

static void update_strip(float rpm)
{
    bool blink_off = rpm >= VEHICLE_SHIFT_RPM && !s_blink;
    for (int i = 0; i < N_SEG; i++) {
        int r = seg_rpm(i);
        int want = (rpm >= r && !blink_off) ? (int)rpm_colour(r) : C_SEG_OFF;
        if (want == s_seg_state[i]) continue;
        s_seg_state[i] = want;
        lv_obj_set_style_bg_color(s_seg[i], lv_color_hex(want), 0);
    }
}

// ---- readings row --------------------------------------------------------------
static void build_tiles(void)
{
    rect(s_dash, MARGIN, ROW_RULE_Y, 800 - 2 * MARGIN, 1, C_TILE_LINE);
    int w = (800 - 2 * MARGIN) / N_TILES;
    for (int i = 0; i < N_TILES; i++) {
        int x = MARGIN + i * w;
        if (i) rect(s_dash, x, ROW_Y, 1, ROW_H, C_TILE_LINE);

        // The cell fills with the state colour; transparent while normal.
        lv_obj_t *cell = rect(s_dash, x + CELL_INSET, ROW_Y, w - 2 * CELL_INSET, ROW_H, C_FACE);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(cell, CELL_RADIUS, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        int pad = 16 - CELL_INSET;

        lv_obj_t *c = caption(cell, TILES[i].caption);
        lv_obj_set_pos(c, pad, 14);

        // Value and unit share a bottom-aligned row, so the unit follows the
        // value as its width changes.
        lv_obj_t *row = lv_obj_create(cell);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(row, 5, 0);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, pad, ROW_H - 14 - 33);

        lv_obj_t *v = ui_label(row, "--", &font_value_46, C_WHITE);
        lv_obj_t *u = ui_label(row, TILES[i].unit, &font_text_18, C_MUTED);
        lv_obj_set_style_pad_bottom(u, 2, 0);

        s_t[i] = (tile_t){ .cell = cell, .cap = c, .val = v, .unit = u,
                           .painted = -1, .last = NAN };
        paint_tile(&s_t[i]);
    }
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
    s_to_val = ui_label(s_to, "", &font_speed_150, C_WHITE);
    lv_obj_align(s_to_val, LV_ALIGN_CENTER, -30, 10);
    s_to_unit = ui_label(s_to, "", &lv_font_montserrat_44, C_WHITE);
    lv_obj_t *h = ui_label(s_to, "TAP TO ACKNOWLEDGE", &font_text_18, C_WHITE);
    lv_obj_set_style_text_letter_space(h, 2, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_flag(s_to, LV_OBJ_FLAG_HIDDEN);
}

static void update_takeover(int64_t now)
{
    int show = -1;
    for (int i = 0; i < N_TILES; i++) {
        tile_t *t = &s_t[i];
        if (t->level != LEVEL_ALARM) { t->acked = false; continue; }
        if (!TILES[i].takeover || t->acked ||
            now - level_alarm_since(&t->state) < TILES[i].takeover_ms)
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
        // Montserrat 44 has no degree sign; the title says what it is.
        const char *u = TILES[show].unit;
        ui_set_text_if(s_to_unit, (unsigned char)u[0] == 0xC2 ? u + 2 : u);
        lv_obj_clear_flag(s_to, LV_OBJ_FLAG_HIDDEN);
    }
    if (show >= 0) {
        ui_set_num(s_to_val, s_t[show].last, TILES[show].fmt);
        lv_obj_align_to(s_to_unit, s_to_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -6);
    }
}

// ---- build -------------------------------------------------------------------
static void flash_cb(lv_timer_t *t)
{
    s_flash = !s_flash;
    if (s_to_k >= 0)
        lv_obj_set_style_bg_color(s_to, lv_color_hex(s_flash ? C_RED : C_FACE), 0);
    for (int i = 0; i < N_TILES; i++)
        paint_tile(&s_t[i]);           // only alarms change with the flash
}

static void blink_cb(lv_timer_t *t) { s_blink = !s_blink; }

static void build_dash(void)
{
    s_dash = ui_screen();
    build_strip();

    // MPH, centred, always three digits: leading zeros are ghosted padding,
    // so the number never moves as it gains or loses a digit.
    //
    // A hundreds "1" is the exception to the fixed cells. In its cell it sits
    // centred with a wide gap either side, which in front of the tens reads
    // as a hole; placed by its ink instead, it gets ordinary letter spacing.
    int y    = DIGITS_BASE - font_speed_150.line_height;
    int cell = lv_font_get_glyph_width(&font_speed_150, '0', 0);
    s_mph_x0 = 400 - 3 * cell / 2;
    for (int i = 0; i < 3; i++) {
        s_mph[i] = ui_label(s_dash, "-", &font_speed_150, C_GHOST);
        lv_obj_set_width(s_mph[i], cell);
        lv_obj_set_style_text_align(s_mph[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_mph[i], s_mph_x0 + i * cell, y);
        s_mph_col[i] = C_GHOST;
    }
    lv_font_glyph_dsc_t one;
    lv_font_get_glyph_dsc(&font_speed_150, &one, '1', 0);
    s_mph_one_x = s_mph_x0 + cell - HUNDREDS_GAP - (one.ofs_x + one.box_w);

    lv_obj_align(caption(s_dash, "MPH"), LV_ALIGN_TOP_MID, 0, CAPTION_Y);

    build_tiles();

    lv_timer_create(flash_cb, WARN_FLASH_MS, NULL);
    lv_timer_create(blink_cb, SHIFT_BLINK_MS, NULL);
}

static void boot_done_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    // No fade: a fade redraws all 384k pixels on every step.
    lv_scr_load(s_dash);
    lv_obj_del(s_boot);          // the photo is 750 KB of draw state; free it
    s_boot = NULL;
}

lv_obj_t *dash_screen(void) { return s_dash; }

void dash_create(uint32_t boot_ms)
{
    build_dash();
    build_takeover();

    s_boot = ui_screen();
    lv_obj_t *img = lv_img_create(s_boot);
    lv_img_set_src(img, &boot_logo);
    lv_obj_center(img);
    lv_scr_load(s_boot);

    lv_timer_create(boot_done_cb, boot_ms, NULL);
}

// ---- update ------------------------------------------------------------------
static void set_digit(int i, char c, uint32_t col)
{
    char t[2] = { c, 0 };
    ui_set_text_if(s_mph[i], t);
    if (col != s_mph_col[i]) {           // a style write redraws, even unchanged
        s_mph_col[i] = col;
        lv_obj_set_style_text_color(s_mph[i], lv_color_hex(col), 0);
    }
}

static void update_mph(float mph)
{
    char b[4] = "---";                   // no reading: three ghosted dashes
    int m = isnan(mph) ? -1 : (int)roundf(mph);
    if (m > 999) m = 999;
    if (m >= 0) snprintf(b, sizeof b, "%03d", m);

    // Leading zeros are padding. The units digit always counts, so standing
    // still reads "00" ghosted and a white "0".
    bool lead = m >= 0;
    for (int i = 0; i < 3; i++) {
        if (b[i] != '0' || i == 2) lead = false;
        set_digit(i, b[i], (lead || m < 0) ? C_GHOST : C_WHITE);
    }
    int x = b[0] == '1' ? s_mph_one_x : s_mph_x0;
    if (lv_obj_get_x(s_mph[0]) != x) lv_obj_set_x(s_mph[0], x);
}

void dash_update(const dash_frame_t *f)
{
    int64_t now = now_ms();

    if (f->no_can == lv_obj_has_flag(s_no_data, LV_OBJ_FLAG_HIDDEN)) {
        if (f->no_can) lv_obj_clear_flag(s_no_data, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_no_data, LV_OBJ_FLAG_HIDDEN);
    }

    update_strip(f->rpm_smooth);
    update_mph(f->v[SIG_SPEED]);

    bool running = f->run_ms >= DASH_ALARM_START_MS;
    for (int i = 0; i < N_TILES; i++) {
        float v = f->v[TILES[i].sig];
        s_t[i].last = v;
        ui_set_num(s_t[i].val, v, TILES[i].fmt);
        s_t[i].level = level_update(&s_t[i].state, &TILES[i].lim, v, running, now);
        paint_tile(&s_t[i]);
    }

    update_takeover(now);
}
