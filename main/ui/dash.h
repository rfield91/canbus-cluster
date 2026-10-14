// The main page: a shift-light tach strip across the top, MPH and RPM in the
// middle, and a row of readings along the bottom. Which readings, and their
// limits, are in the TILES table at the top of dash.c.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "signals.h"

// Low readings (oil pressure, battery) only warn once the engine has been
// turning this long: oil pressure lags the crank at start-up.
#define DASH_ALARM_START_MS 3000

// One update's worth of everything the dash and min/max pages show.
typedef struct {
    float   v[SIG_COUNT];   // NAN = no reading
    float   rpm_smooth;     // smoothed RPM, for the tach
    int     gear;           // 1..n, 0 = no gear
    bool    no_can;         // the car is not answering
    int64_t run_ms;         // how long the engine has been turning, 0 if stopped
} dash_frame_t;

// Shows the boot logo, then switches to the dash after boot_ms.
void dash_create(uint32_t boot_ms);

// Call from an lv_timer. Redraws only what changed.
void dash_update(const dash_frame_t *f);

// The dash page, for swipe navigation.
lv_obj_t *dash_screen(void);
