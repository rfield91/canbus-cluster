// Engaged gear for a manual box, worked out from RPM against road speed.
//
// In gear, rpm / mph is fixed by the gearing: ratio * final drive * 1056 /
// (pi * tire diameter in inches). The reading is matched to the nearest gear,
// and anything not close to one (clutch in, neutral, coasting) is no gear.
// Reverse cannot be told apart from first: speed has no sign.
#pragma once

#include <stdint.h>
#include "vehicle.h"

// Readings must sit within this fraction of a gear's ratio. The AY6's closest
// pair (4th to 5th) is 19% apart, so 7% cannot match the wrong gear.
#define GEAR_TOLERANCE      0.07f
// Below these the ratio is mostly noise: speed reads in whole km/h.
#define GEAR_MIN_MPH        4.0f
#define GEAR_MIN_RPM        500.0f
// A new answer must hold this long before it is shown, so a shift reads as
// one change rather than flickering through every ratio on the way.
#define GEAR_SETTLE_MS      250

void gear_init(const drivetrain_t *dt);

// The rpm/mph a gear should read. gear is 1-based.
float gear_rpm_per_mph(int gear);

// The gear the reading matches right now: 1..n, or 0 for none. Stateless.
int gear_match(float rpm, float mph);

// gear_match, held until a change has settled. Call at a steady rate.
int gear_update(float rpm, float mph, int64_t now_ms);
