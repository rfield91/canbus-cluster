#include "gear.h"

#include <math.h>

#define IN_PER_MILE_PER_MIN 1056.0f   // 63360 in/mile / 60 min/hour

static float s_rpm_per_mph[VEHICLE_MAX_GEARS];
static int   s_n;

static int     s_shown, s_pending;
static int64_t s_pending_since;

void gear_init(const drivetrain_t *dt)
{
    s_n = dt->n_gears > VEHICLE_MAX_GEARS ? VEHICLE_MAX_GEARS : dt->n_gears;
    float wheel = IN_PER_MILE_PER_MIN / ((float)M_PI * dt->tire_diameter_in);
    for (int g = 0; g < s_n; g++)
        s_rpm_per_mph[g] = dt->ratio[g] * dt->final_drive * wheel;
    s_shown = s_pending = 0;
}

float gear_rpm_per_mph(int gear)
{
    return (gear >= 1 && gear <= s_n) ? s_rpm_per_mph[gear - 1] : NAN;
}

int gear_match(float rpm, float mph)
{
    if (isnan(rpm) || isnan(mph) || rpm < GEAR_MIN_RPM || mph < GEAR_MIN_MPH)
        return 0;
    float r = rpm / mph;
    for (int g = 0; g < s_n; g++)
        if (fabsf(r / s_rpm_per_mph[g] - 1.0f) <= GEAR_TOLERANCE)
            return g + 1;
    return 0;
}

int gear_update(float rpm, float mph, int64_t now_ms)
{
    int g = gear_match(rpm, mph);
    if (g != s_pending) {
        s_pending = g;
        s_pending_since = now_ms;
    }
    if (s_pending != s_shown && now_ms - s_pending_since >= GEAR_SETTLE_MS)
        s_shown = s_pending;
    return s_shown;
}
