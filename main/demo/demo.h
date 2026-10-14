// Bench demo: a simulated engine and gearbox feeding the signal store in
// place of the OBD poller, so everything downstream of it -- gear detection,
// warnings, min/max -- runs exactly as it does in the car.
#pragma once

// Call after gear_init: the simulated drive uses the real gearing.
void demo_start(void);
