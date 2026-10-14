// CAN on the 7in board: onboard transceiver on GPIO20 (TX) / GPIO19 (RX),
// 500 kbit/s. The transceiver is only powered once ch422g_init has been told
// CAN is wanted.
#pragma once

#include <stdint.h>
#include <stdbool.h>

void can_bus_init(void);

// Starts the receive task, which hands each frame to the trouble-code reader
// and then the OBD poller.
void can_bus_start(void);

// Sends one standard-id, 8-byte frame. Waits up to timeout_ms for queue space.
bool can_bus_send(uint32_t id, const uint8_t data[8], int timeout_ms);

// esp_timer time of the last OBD reply, 0 if none yet: whether the car is
// answering at all.
int64_t can_bus_last_reply_us(void);
