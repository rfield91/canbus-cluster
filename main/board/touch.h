// GT911 capacitive touch on the 7in board, as an LVGL pointer.
//
// It shares the CH422G's I2C bus (new i2c_master API), and its reset line is
// CH422G output EXIO1.
#pragma once

#include "esp_err.h"

// Call after ch422g_init and lvgl_port_init, before lvgl_port_start. A missing
// or silent controller is logged and skipped; the dash runs without touch.
esp_err_t touch_init(void);
