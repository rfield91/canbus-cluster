#include "signals.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

typedef struct {
    float   value;
    int64_t at_us;
} slot_t;

// Written on core 0 (CAN), read on core 1 (LVGL). A value and its timestamp
// have to be seen together, so each access holds a spinlock -- a handful of
// instructions, far cheaper than a mutex at these rates.
static slot_t       s_slot[SIG_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *const NAMES[SIG_COUNT] = {
    [SIG_RPM]      = "rpm",
    [SIG_SPEED]    = "speed",
    [SIG_COOLANT]  = "coolant",
    [SIG_OIL_TEMP] = "oil temp",
    [SIG_OIL_PSI]  = "oil psi",
    [SIG_IAT]      = "IAT",
    [SIG_BATTERY]  = "battery",
    [SIG_THROTTLE] = "throttle",
};

void signals_init(void)
{
    for (int i = 0; i < SIG_COUNT; i++)
        s_slot[i] = (slot_t){ .value = NAN, .at_us = 0 };
}

void signals_set(signal_id_t id, float value)
{
    if ((unsigned)id >= SIG_COUNT) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_slot[id] = (slot_t){ .value = value, .at_us = now };
    taskEXIT_CRITICAL(&s_mux);
}

float signals_get(signal_id_t id)
{
    if ((unsigned)id >= SIG_COUNT) return NAN;
    taskENTER_CRITICAL(&s_mux);
    slot_t s = s_slot[id];
    taskEXIT_CRITICAL(&s_mux);
    if (esp_timer_get_time() - s.at_us > SIGNAL_STALE_MS * 1000LL)
        return NAN;
    return s.value;
}

const char *signals_name(signal_id_t id)
{
    return (unsigned)id < SIG_COUNT ? NAMES[id] : "?";
}
