#include "limits.h"

#include <math.h>

// Past a limit, in the direction that limit guards. On the way back the limit
// moves in by hyst, so clearing needs a real recovery.
static bool over(limit_t l, float v, bool on, float hyst)  { return l.on && v > l.at - (on ? hyst : 0); }
static bool under(limit_t l, float v, bool on, float hyst) { return l.on && v < l.at + (on ? hyst : 0); }

static bool band_update(band_t *b, bool cond, int sustain_ms, int hold_ms, int64_t now)
{
    if (!cond)               b->cond_since = 0;
    else if (!b->cond_since) b->cond_since = now;

    if (!b->on && cond && now - b->cond_since >= sustain_ms) {
        b->on = true;
        b->on_since = now;
    } else if (b->on && !cond && now - b->on_since >= hold_ms) {
        b->on = false;
    }
    return b->on;
}

level_t level_update(level_state_t *s, const limits_t *lim, float v, bool running, int64_t now)
{
    bool have = !isnan(v);
    bool lo   = have && (running || !lim->lo_needs_running);
    float h   = lim->hyst;

    bool alarm = (have && over(lim->alarm_hi, v, s->alarm.on, h)) ||
                 (lo && under(lim->alarm_lo, v, s->alarm.on, h));
    bool warn  = (have && over(lim->warn_hi, v, s->warn.on, h)) ||
                 (lo && under(lim->warn_lo, v, s->warn.on, h));
    bool cold  = have && under(lim->cold, v, s->cold.on, h);

    // Each band runs every update, so its timing stays right whichever one
    // currently decides the colour.
    bool a = band_update(&s->alarm, alarm, lim->sustain_ms, ALARM_MIN_HOLD_MS, now);
    bool w = band_update(&s->warn,  warn || alarm, lim->sustain_ms, 0, now);
    bool c = band_update(&s->cold,  cold, 0, 0, now);

    return a ? LEVEL_ALARM : w ? LEVEL_WARN : c ? LEVEL_COLD : LEVEL_NORMAL;
}
