// Warning logic for one reading: limits with hysteresis, a minimum on-time,
// and an optional delay before it trips. No LVGL here, just the decision.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Once on, an alarm stays on at least this long: a pressure dip can be over in
// 100 ms, too brief to notice otherwise.
#define ALARM_MIN_HOLD_MS 2000

typedef struct {
    // Trip above hi, clear only once back below hi_clear (and the same, the
    // other way, for lo), so a reading sitting on a limit does not strobe.
    // NAN = no limit on that side.
    float hi, hi_clear;
    float lo, lo_clear;
    // Low readings only count once the engine is running: a stopped engine
    // has no oil pressure and no alternator output.
    bool  lo_needs_running;
    // The condition must hold this long before the alarm trips, so one stray
    // reading is not a failing pump. 0 = trip at once.
    int   sustain_ms;
} alarm_limits_t;

typedef struct {
    bool    on;
    int64_t on_since;       // ms
    int64_t cond_since;     // ms; when the out-of-range condition began, 0 if not
} alarm_state_t;

// value NAN means no reading, which never alarms. running is whether the
// engine has been turning long enough for low limits to apply. Returns on.
bool alarm_update(alarm_state_t *s, const alarm_limits_t *lim, float value,
                  bool running, int64_t now_ms);
