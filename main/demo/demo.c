#include "demo.h"
#include "signals.h"
#include "gear.h"
#include "vehicle.h"

#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define TICK_MS       20
#define IDLE_RPM      800.0f
#define LAUNCH_RPM    1800.0f   // rpm as the clutch bites in first
#define SHIFT_AT_RPM  7400.0f   // brushes the shift light every gear
#define TOP_GEAR_RPM  3500.0f   // cruise in top rather than chase 200 mph
#define GEAR_SEC      3.0f      // time to pull through one gear
#define CLUTCH_SEC    0.4f      // time between gears, clutch in
#define IDLE_SEC      3.0f      // stopped, before and after each run
#define COAST_SEC     8.0f      // clutch in, rolling to a stop
#define WARN_SEC      10.0f     // per warning phase: long enough for the takeover

// One pull through the gears, then a coast to a stop.
static void drive(float t, float *rpm, float *mph)
{
    const int n = VEHICLE_DRIVETRAIN.n_gears;
    const float pull = n * GEAR_SEC + (n - 1) * CLUTCH_SEC;
    t = fmodf(t, IDLE_SEC + pull + COAST_SEC);

    if (t < IDLE_SEC) { *rpm = IDLE_RPM; *mph = 0; return; }
    t -= IDLE_SEC;

    if (t < pull) {
        int   g  = (int)(t / (GEAR_SEC + CLUTCH_SEC));       // 0-based
        float in = t - g * (GEAR_SEC + CLUTCH_SEC);
        float k  = gear_rpm_per_mph(g + 1);
        // Each gear starts where the last one's road speed drops the revs to.
        float from = g == 0 ? LAUNCH_RPM : SHIFT_AT_RPM * k / gear_rpm_per_mph(g);
        float to   = g == n - 1 ? TOP_GEAR_RPM : SHIFT_AT_RPM;
        if (in < GEAR_SEC) {
            *rpm = from + (to - from) * in / GEAR_SEC;
            *mph = *rpm / k;
        } else {
            // Clutch in: road speed holds, the revs fall toward the next gear.
            float next = SHIFT_AT_RPM * gear_rpm_per_mph(g + 2) / k;
            *mph = SHIFT_AT_RPM / k;
            *rpm = to + (next - to) * (in - GEAR_SEC) / CLUTCH_SEC;
        }
        return;
    }
    t -= pull;

    float top = TOP_GEAR_RPM / gear_rpm_per_mph(n);
    *rpm = IDLE_RPM;
    *mph = top * (1.0f - t / COAST_SEC);
}

static void demo_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        float t = (esp_timer_get_time() - t0) / 1e6f;
        float rpm, mph;
        drive(t, &rpm, &mph);
        float load = (rpm - IDLE_RPM) / (SHIFT_AT_RPM - IDLE_RPM);

        float warm = 1.0f - expf(-t / 20.0f);
        float wob  = sinf(t * 0.6f) * 3.0f;
        float coolant  = 100.0f + warm * 95.0f  + wob;
        float oil_temp = 100.0f + warm * 115.0f + wob;
        float iat      = 90.0f  + warm * 30.0f  + wob;
        float oil_psi  = 22.0f + load * 45.0f;
        float volts    = 14.1f + sinf(t * 0.3f) * 0.15f;

#if CONFIG_DASH_DEMO_EXERCISE_WARNINGS
        switch ((int)(t / WARN_SEC) % 6) {
            case 1: coolant  = 255.0f; break;
            case 2: oil_temp = 300.0f; break;
            case 3: oil_psi  = 5.0f;   break;
            case 4: iat      = 185.0f; break;
            case 5: volts    = 11.8f;  break;
            default: break;                     // all in range
        }
#endif
        signals_set(SIG_RPM,      rpm);
        signals_set(SIG_SPEED,    mph);
        signals_set(SIG_COOLANT,  coolant);
        signals_set(SIG_OIL_TEMP, oil_temp);
        signals_set(SIG_OIL_PSI,  oil_psi);
        signals_set(SIG_IAT,      iat);
        signals_set(SIG_BATTERY,  volts);
        signals_set(SIG_THROTTLE, load * 100.0f);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void demo_start(void)
{
    xTaskCreatePinnedToCore(demo_task, "demo", 3072, NULL, 8, NULL, 0);
}
