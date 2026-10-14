// The live engine readings, shared between whatever produces them (the OBD
// poller in the car, the simulator in demo mode) and everything that shows
// them.
//
// Units are imperial everywhere: the producer folds any conversion into its
// scale, so nothing downstream does unit maths.
#pragma once

#include <stdint.h>

typedef enum {
    SIG_RPM,
    SIG_SPEED,      // mph
    SIG_COOLANT,    // degF
    SIG_OIL_TEMP,   // degF
    SIG_OIL_PSI,    // psi
    SIG_IAT,        // degF, intake air
    SIG_BATTERY,    // volts, as the ECM sees it
    SIG_THROTTLE,   // %, not displayed: logged with the rest when debugging
    SIG_COUNT
} signal_id_t;

// A reading older than this is no reading. Every PID is polled well inside
// it, so only a module that stopped answering (key off, a PID it dropped)
// gets here, and the tile shows "--" instead of a frozen last value.
#define SIGNAL_STALE_MS 3000

void signals_init(void);

// From any task. Writing NAN marks the signal as having no reading.
void signals_set(signal_id_t id, float value);

// The latest value, or NAN if there is none or it has gone stale.
float signals_get(signal_id_t id);

const char *signals_name(signal_id_t id);
