// Everything specific to the car: which PIDs to ask for and how to read them,
// and the drivetrain the gear indicator works from.
//
// The rest of the firmware only knows signals (signals.h). To fit the dash to
// a different car, write another vehicle_*.c that defines the symbols below
// and put it in main/CMakeLists.txt in place of vehicle_lfx.c.
#pragma once

#include <stdint.h>
#include "signals.h"

// ---- CAN addressing -----------------------------------------------------------
// Functional request address (every OBD module listens) and the reply range.
#define OBD_FUNC_REQ_ID  0x7DF
#define OBD_RESP_LO      0x7E8
#define OBD_RESP_HI      0x7EF

// The engine computer: physical request address and its reply address.
#define ECM_REQ_ID       0x7E0
#define ECM_RESP_ID      0x7E8

// ---- polled values ----------------------------------------------------------
typedef enum {
    OBD_MODE_01 = 0x01,   // SAE J1979 current data, one-byte PID
    OBD_MODE_22 = 0x22,   // read data by identifier (GM enhanced), two-byte PID
} obd_mode_t;

// value = raw * scale + offset, where raw is the first `nbytes` data bytes,
// big-endian. Scales fold in the unit conversion, so the result is already
// imperial.
typedef struct {
    obd_mode_t  mode;
    uint16_t    pid;
    uint8_t     nbytes;     // 1 or 2
    float       scale;
    float       offset;
    uint16_t    period_ms;  // how often to ask: matched to how fast it moves
    signal_id_t signal;
} obd_pid_t;

extern const obd_pid_t VEHICLE_PIDS[];
extern const int       VEHICLE_PID_COUNT;

// ---- drivetrain, for the gear indicator --------------------------------------
#define VEHICLE_MAX_GEARS 8

typedef struct {
    int   n_gears;
    float ratio[VEHICLE_MAX_GEARS];  // transmission, first gear first
    float final_drive;
    float tire_diameter_in;
} drivetrain_t;

extern const drivetrain_t VEHICLE_DRIVETRAIN;

// ---- display -----------------------------------------------------------------
#define VEHICLE_RPM_MAX    7600   // tach full scale
#define VEHICLE_WARN_RPM   6000   // the tach turns amber from here
#define VEHICLE_SHIFT_RPM  7200   // red from here, and the strip flashes
