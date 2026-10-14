// Polls the vehicle profile's PIDs (vehicle.h) and writes the answers into
// the signal store.
//
// THIS TRANSMITS. Each request goes physically to the ECM, exactly as a scan
// tool's would: safe on a running engine, but the dash is an active node on
// the bus rather than a silent listener.
//
// Single-frame answers only. Every PID in the profile returns at most two
// data bytes, which fits.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// How often the poller wakes. At most one request goes out per tick, so this
// also caps the request rate (50/s).
#define OBD_POLL_TICK_MS 20

// Call after can_bus_init.
void obd_poll_start(void);

// From the CAN receive task. True if the frame was a poll answer.
bool obd_poll_handle_frame(uint32_t id, const uint8_t *data, uint8_t dlc);

// Stop sending while something else talks to the ECM (reading trouble codes),
// so a poll never lands in the middle of a multi-frame answer.
void obd_poll_pause(bool paused);
