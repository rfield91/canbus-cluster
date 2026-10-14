#include "alarm.h"

#include <math.h>

bool alarm_update(alarm_state_t *s, const alarm_limits_t *lim, float v,
                  bool running, int64_t now)
{
    bool out = false;
    if (!isnan(v)) {
        if (!isnan(lim->hi))
            out |= v > (s->on ? lim->hi_clear : lim->hi);
        if (!isnan(lim->lo) && (running || !lim->lo_needs_running))
            out |= v < (s->on ? lim->lo_clear : lim->lo);
    }

    if (!out)                s->cond_since = 0;
    else if (!s->cond_since) s->cond_since = now;

    if (!s->on && out && now - s->cond_since >= lim->sustain_ms) {
        s->on = true;
        s->on_since = now;
    } else if (s->on && !out && now - s->on_since >= ALARM_MIN_HOLD_MS) {
        s->on = false;
    }
    return s->on;
}
