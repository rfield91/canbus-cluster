// What state a reading is in: cold, normal, warning or alarm, from limits set
// per gauge. No LVGL here, just the decision.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { LEVEL_NORMAL, LEVEL_COLD, LEVEL_WARN, LEVEL_ALARM } level_t;

// A limit that may be left out. Write LIMIT(x) to set one; a field left out
// of the initialiser is off.
typedef struct { bool on; float at; } limit_t;
#define LIMIT(x) { true, (x) }

typedef struct {
    limit_t cold;                 // below: not up to temperature yet (blue)
    limit_t warn_lo, warn_hi;     // outside: caution (amber)
    limit_t alarm_lo, alarm_hi;   // outside: alarm (red, flashing)
    // How far back past a limit a reading must come to clear it, so a value
    // sitting on a limit does not strobe.
    float   hyst;
    // Low limits only count once the engine is running: a stopped engine has
    // no oil pressure and no alternator output.
    bool    lo_needs_running;
    // A warning or alarm must hold this long before it shows, so one stray
    // reading is not a failing pump. 0 = at once.
    int     sustain_ms;
} limits_t;

// Once on, an alarm stays on at least this long: a pressure dip can be over in
// 100 ms, too brief to notice otherwise.
#define ALARM_MIN_HOLD_MS 2000

// One out-of-range condition, with hysteresis, sustain and minimum hold.
typedef struct {
    bool    on;
    int64_t on_since;      // ms
    int64_t cond_since;    // ms; when the trip condition began, 0 if not
} band_t;

typedef struct {
    band_t cold, warn, alarm;
} level_state_t;

// value NAN means no reading: always LEVEL_NORMAL. running is whether the
// engine has been turning long enough for low limits to apply.
level_t level_update(level_state_t *s, const limits_t *lim, float value,
                     bool running, int64_t now_ms);

// When the current alarm came on, for the takeover's delay.
static inline int64_t level_alarm_since(const level_state_t *s) { return s->alarm.on_since; }
