# canbus-cluster

A digital dash for a GM LFX-swapped Mazda RX-8 (E39 ECM, Aisin AY6 / MV5
6-speed manual), on a Waveshare ESP32-S3-Touch-LCD-7 (800x480). It polls the
ECM over OBD-II on the board's CAN transceiver.

This project is source-available and free for personal and educational use.
Commercial use is NOT permitted.

### *****Please do not use these files to sell this to others. They exist so that those wanting to modify their dash/cluster don't have to spend an arm and a leg to do so.***** ###

Original project: https://youtu.be/t7H6pevep40

## Pages

Swipe left/right between them.

1. **Dash**: RPM bar with a shift light, MPH, the engaged gear (worked out from
   RPM against speed), and tiles for coolant, oil temp, oil pressure, intake air
   temp and battery voltage. A tile flashes red when its reading is out of range.
   Low oil pressure, high coolant temp and high oil temp also take over the whole
   screen until you tap it.
2. **Min / Max**: the lowest and highest values for this drive and the last one.
3. **Trouble codes**: read and clear.

## Building

1. Install [VS Code](https://code.visualstudio.com/Download) and the ESP-IDF
   extension (v5.5). Waveshare's setup guide:
   https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-3.4C#Introduction_to_ESP-IDF_and_Environment_Setup_.28VSCode_Column.29
2. Open this folder, set the target to `esp32s3`.
3. Pick the run mode: `idf.py menuconfig` → **Dash** → **Run mode**:
   - **Car**: polls the ECM and runs the dash.
   - **Demo**: a simulated engine for the bench. CAN never starts.
   - **PID scan**: sweeps the ECM's GM mode 22 identifiers. See
     [main/can/pid_scan.h](main/can/pid_scan.h).
4. Build and flash through the board's **UART** USB-C port. In Car mode the
   native USB port is switched off, because CAN shares its pins.

## Layout

| Path | What's there |
|---|---|
| `main/vehicle/` | **Everything car-specific.** The PIDs to poll and how to scale them, the gearing ([vehicle_lfx.c](main/vehicle/vehicle_lfx.c)), and the rev range ([vehicle.h](main/vehicle/vehicle.h)). Also the signal store the rest of the firmware reads from, and gear detection. |
| `main/can/` | TWAI driver, the OBD poller, the trouble-code reader, and the PID scanner |
| `main/ui/` | Pages. Which readings get a tile, and their warning limits, are in the `TILES` table at the top of [dash.c](main/ui/dash.c) |
| `main/board/` | LCD, touch, LVGL port, and the CH422G I/O expander |
| `main/demo/` | The bench simulator |

## Fitting it to the car

The mode 22 PIDs for oil temp and oil pressure, and the gear ratios, still need
to be confirmed on the car. Check them against HP Tuners: turn on **Dash** →
**Log every OBD reply** in menuconfig, watch `idf.py monitor`, and compare. If a
PID doesn't answer, use PID scan mode to find the right one.
