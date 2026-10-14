# canbus-cluster

A digital dash for a GM LFX-swapped Mazda RX-8 (E39 ECM, Aisin AY6 / MV5
6-speed manual), on a [Waveshare ESP32-S3-Touch-LCD-7](https://www.amazon.com/dp/B0D6XSY476). It polls the
ECM over OBD-II on the board's CAN transceiver.

## Attribution

Credit for the bulk of the code belongs to [travis.cea](https://github.com/traviscea) and [smark88](https://github.com/smark88).

**Please do not use these files to sell this to others. They exist so that those wanting to modify their dash/cluster don't have to spend an arm and a leg to do so.**

Original project video: https://youtu.be/t7H6pevep40

This project is source-available and free for personal and educational use.
Commercial use is NOT permitted.

## Pages

Swipe left/right between them.

1. **Dash**: a shift-light tach strip (green, then amber from 6000 rpm, then
   red and flashing at the 7200 rpm shift point), MPH in the centre, and
   readings for coolant, oil temp, oil pressure, intake air temp and battery
   voltage. A reading's cell fills blue while cold, amber at its warning limit
   and flashes red at its alarm limit. Low oil pressure, high coolant temp and high oil temp also take over
   the whole screen until they recover or you tap it.
2. **Min / Max**: the lowest and highest values for this drive and the last one.
3. **Trouble codes**: read and clear.

## Setup and flashing

1. Install VS Code and the ESP-IDF extension.
2. Check out Waveshare's [docs for the LCD-7](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7)
3. Follow Waveshare's [environment setup](https://docs.waveshare.com/ESP32-ESP-IDF-Tutorials/ESP-IDF-Installation):
   - Install ESP-IDF v5.5.5
   - Install usb drivers
4. Device setup:
   - Ensure the UART toggle is set to UART1. You won't be able to flash the device through the UART USB-C port if it's not (mine wasn't, and it was infuriating).
   - You need to use a USB A to USB C cable. A C to C cable didn't work for me.
5. Open this folder in VS Code.
   - Set the target to `esp32s3`
   - Select the appropriate COM port - it should show details of the connected device
   - Set flash method to UART
   - Open VS Code Settings (`ctrl + ,`) and search for `baud`. Set the ESP-IDF Flash Baud rate to `115200`.
6. Pick the run mode: `idf.py menuconfig` → **Dash** → **Run mode**:
   - **Car**: polls the ECM and runs the dash.
   - **Demo**: a simulated engine for the bench. CAN never starts.
   - **PID scan**: sweeps the ECM's GM mode 22 identifiers. See
     [main/can/pid_scan.h](main/can/pid_scan.h).
   - The committed default is Car. Switch to Demo for testing.
7. Build and flash through the board's **UART** USB-C port. In Car mode the
   native USB port is switched off, because CAN shares its pins.

## Layout

| Path            | What's there                                                                                                                                                                                                                                                            |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `main/vehicle/` | **Everything car-specific.** The PIDs to poll and how to scale them, the gearing ([vehicle_lfx.c](main/vehicle/vehicle_lfx.c)), and the rev range ([vehicle.h](main/vehicle/vehicle.h)). Also the signal store the rest of the firmware reads from. |
| `main/can/`     | TWAI driver, the OBD poller, the trouble-code reader, and the PID scanner                                                                                                                                                                                               |
| `main/ui/`      | Pages. Which readings get a tile, and their warning limits, are in the `TILES` table at the top of [dash.c](main/ui/dash.c)                                                                                                                                             |
| `main/board/`   | LCD, touch, LVGL port, and the CH422G I/O expander                                                                                                                                                                                                                      |
| `main/demo/`    | The bench simulator                                                                                                                                                                                                                                                     |

## Fonts and boot image

Both are generated C files; don't edit them by hand. The generators need
Node.js:

```bash
cd tools
npm install
npm run image   # assets/boot.jpg -> main/ui/images/boot_logo.c
npm run fonts   # -> main/ui/fonts/*.c (run a build first, for the TTF)
```

## Fitting it to the car

The mode 22 PIDs for oil temp and oil pressure still need to be confirmed on
the car. Check them against HP Tuners: turn on **Dash** →
**Log every OBD reply** in menuconfig, watch `idf.py monitor`, and compare. If a
PID doesn't answer, use PID scan mode to find the right one.
