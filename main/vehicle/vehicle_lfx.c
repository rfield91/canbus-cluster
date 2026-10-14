// GM LFX 3.6 V6 (E39 ECM) with an Aisin AY6 6-speed manual (RPO MV5), in a
// Mazda RX-8. The ECM is the only module on the bus.
//
// Status of each entry, until it has been checked against HP Tuners on the
// car:
//   mode 01  -- SAE J1979 standard; any OBD-II ECM answers these.
//   mode 22  -- GM enhanced. These identifiers and scalings came from an E92
//               (LT4) ECM. The E39 is the same Global A family, but treat
//               them as UNVERIFIED until they match HP Tuners.

#include "vehicle.h"

#define C_TO_F_SCALE   1.8f
#define C40_TO_F_OFF  -40.0f   // (A - 40) degC = A * 1.8 - 40 degF

// The poller sends at most 50 requests/s (one per 20 ms tick). This table
// asks for ~24/s, which leaves headroom.
const obd_pid_t VEHICLE_PIDS[] = {
    // Engine RPM, (256A + B) / 4. The fastest-moving value on the dash.
    { OBD_MODE_01, 0x0C, 2, 0.25f, 0.0f, 100, SIG_RPM },

    // Vehicle speed, A km/h, folded to mph. Fresh speed matters as much as
    // fresh RPM: the gear indicator divides one by the other.
    { OBD_MODE_01, 0x0D, 1, 0.621371f, 0.0f, 200, SIG_SPEED },

    // Coolant, A - 40 degC.
    { OBD_MODE_01, 0x05, 1, C_TO_F_SCALE, C40_TO_F_OFF, 1000, SIG_COOLANT },

    // Intake air temp, A - 40 degC.
    { OBD_MODE_01, 0x0F, 1, C_TO_F_SCALE, C40_TO_F_OFF, 1000, SIG_IAT },

    // Control module voltage, (256A + B) / 1000 V.
    { OBD_MODE_01, 0x42, 2, 0.001f, 0.0f, 1000, SIG_BATTERY },

    // Throttle position, A * 100 / 255 %.
    { OBD_MODE_01, 0x11, 1, 100.0f / 255.0f, 0.0f, 500, SIG_THROTTLE },

    // UNVERIFIED. Oil temp, A - 40 degC. On the LT4 this was HP Tuners' "Oil
    // Temp Calc", the ECM's model. If the E39 does not answer, try standard
    // mode 01 PID 0x5C (same encoding) or find the right one with a PID scan.
    { OBD_MODE_22, 0x1154, 1, C_TO_F_SCALE, C40_TO_F_OFF, 1000, SIG_OIL_TEMP },

    // UNVERIFIED. Oil pressure. Calibrated on the LT4 as psi = (A - 37) *
    // 0.625: raw 37 read with the engine stopped, and the slope came from a
    // side-by-side with HP Tuners. Check both points on this car: engine off
    // must read 0, and a warm idle must match HP Tuners.
    { OBD_MODE_22, 0x115C, 1, 0.625f, -23.125f, 300, SIG_OIL_PSI },
};

const int VEHICLE_PID_COUNT = sizeof VEHICLE_PIDS / sizeof VEHICLE_PIDS[0];

// AY6 ratios from GM's MV5 spec sheet. Final drive is the Ford 8.8 Diff. Tire is
// 295/30R18: 18 + 2 * 295 * 0.30 / 25.4 = 24.96 in. Change it if the tires
// change. If the indicator shows "-" in a gear at steady speed, compare
// rpm/mph from the log with gear.h's table before touching these.
const drivetrain_t VEHICLE_DRIVETRAIN = {
    .n_gears          = 6,
    .ratio            = { 4.475f, 2.577f, 1.683f, 1.194f, 1.000f, 0.752f },
    .final_drive      = 3.08f,
    .tire_diameter_in = 24.96f,
};
