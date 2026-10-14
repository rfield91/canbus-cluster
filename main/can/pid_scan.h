// GM mode 22 sweep: for finding a value HP Tuners shows that no published
// PID list has, or confirming one that is supposed to be there.
//
// Asks the ECM for every mode 22 identifier in the configured range, one at a
// time, and logs every positive answer with its raw bytes. Read-only: mode 22
// is "read data by identifier", nothing is written to the car.
//
// HOW TO USE
//   1. idf.py menuconfig -> Dash -> Run mode -> PID scan. Build, flash.
//   2. In the car: engine running and warm, HP Tuners showing the values you
//      are hunting. Laptop on the UART port with `idf.py monitor` for the full
//      log ("HIT 22 xxxx ..." lines). The screen shows progress and the most
//      recent answers.
//   3. Wait for DONE (~3 min for the default range).
//   4. Set the run mode back to Car and reflash.
//
// To have the screen flag likely candidates, fill TARGETS in pid_scan.c with
// the raw first byte a value would have if it uses the common encoding (e.g.
// a temperature T degF as A - 40 degC: A = (T - 32) / 1.8 + 40).
#pragma once

void pid_scan_start(void);
