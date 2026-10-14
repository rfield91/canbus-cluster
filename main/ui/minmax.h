// Min/max of the engine-health readings for this drive (since power-on), with
// the previous drive's record kept alongside.
//
// The current record is saved to NVS as it goes. At the next power-on it
// becomes "last drive" and a fresh record starts, so pulling the key never
// loses what the previous drive saw.
#pragma once

#include "lvgl.h"
#include "dash.h"

// After NVS is up.
void minmax_init(void);

// The screen. Swipe-navigated; page `index` for its dots.
lv_obj_t *minmax_create(int index);

// Every update tick, from the LVGL task.
void minmax_feed(const dash_frame_t *f);

// From a core-0 task. Writes at most every MINMAX_SAVE_MS, and only when the
// record changed. Not called in demo mode, so demo values never persist.
void minmax_save(void);
