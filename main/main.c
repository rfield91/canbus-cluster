// Dash for an LFX-swapped RX-8 on the Waveshare ESP32-S3-Touch-LCD-7.
//
// Polls the GM ECM over the onboard CAN transceiver and shows the result on
// one 800x480 screen. Swipe left/right between three pages: the gauges,
// min/max for this and the last drive, and trouble codes.
//
// The run mode (car, bench demo, PID scan) is chosen in menuconfig -> Dash.

#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "ch422g.h"
#include "lcd.h"
#include "lvgl_port.h"
#include "touch.h"
#include "can_bus.h"
#include "obd_poll.h"
#include "dtc.h"
#include "pid_scan.h"
#include "signals.h"
#include "vehicle.h"
#include "dash.h"
#include "nav.h"
#include "minmax.h"
#include "dtc_screen.h"
#include "demo.h"

static const char *TAG = "MAIN";

// Matches LVGL's refresh period so each frame sees fresh data; the dash only
// redraws values that changed.
#define UPDATE_MS    16
#define BOOT_LOGO_MS 1500

// Below this the engine is not running (cranking is ~200).
#define RUNNING_RPM  400

// No OBD reply for this long and the dash shows NO CAN. The poller asks ~25
// times a second, so 2s of silence means wiring or the car, not luck.
#define NO_CAN_AFTER_US (2 * 1000 * 1000)

// Share of the gap the RPM bar closes each frame: smooths OBD's ~10 Hz
// updates into a sweep rather than steps.
#define RPM_SMOOTHING 0.35f

#if !CONFIG_DASH_MODE_PID_SCAN
static void update_cb(lv_timer_t *t)
{
    static float   rpm_smooth;
    static int64_t running_since;
    int64_t now_ms = esp_timer_get_time() / 1000;

    dash_frame_t f;
    for (int i = 0; i < SIG_COUNT; i++)
        f.v[i] = signals_get(i);

    float rpm = f.v[SIG_RPM];
    bool running = !isnan(rpm) && rpm >= RUNNING_RPM;
    if (!running)            running_since = 0;
    else if (!running_since) running_since = now_ms;
    f.run_ms = running ? now_ms - running_since : 0;

    rpm_smooth += RPM_SMOOTHING * ((isnan(rpm) ? 0.0f : rpm) - rpm_smooth);
    f.rpm_smooth = rpm_smooth;
#if CONFIG_DASH_MODE_CAR
    f.no_can  = esp_timer_get_time() - can_bus_last_reply_us() > NO_CAN_AFTER_US;
#else
    f.no_can  = false;
#endif

    dash_update(&f);
    minmax_feed(&f);
}
#endif

#if CONFIG_DASH_MODE_CAR
static void save_task(void *arg)
{
    for (;;) {
        minmax_save();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();
    signals_init();
    minmax_init();

    // CAN shares GPIO19/20 with native USB. Only switch them over when CAN
    // is wanted, so both USB-C ports keep working on the bench.
#if CONFIG_DASH_MODE_DEMO
    ESP_ERROR_CHECK(ch422g_init(false));
#else
    ESP_ERROR_CHECK(ch422g_init(true));
#endif

    lvgl_port_init(lcd_panel_init());
#if CONFIG_DASH_MODE_PID_SCAN
    can_bus_init();
    pid_scan_start();      // owns the bus: no receive task, no poller
#else
    touch_init();
    dash_create(BOOT_LOGO_MS);
    nav_add(dash_screen());
    nav_add(minmax_create(1));
    nav_add(dtc_screen_create(2));
    lv_timer_create(update_cb, UPDATE_MS, NULL);
#endif
    lvgl_port_start();

    // The first frame (the logo) is drawn with the backlight off; then light it.
    vTaskDelay(pdMS_TO_TICKS(60));
    ch422g_set(CH422G_DISP, true);

#if CONFIG_DASH_MODE_CAR
    can_bus_init();
    dtc_init();
    can_bus_start();
    obd_poll_start();
    xTaskCreatePinnedToCore(save_task, "minmax_save", 3072, NULL, 4, NULL, 0);
#elif CONFIG_DASH_MODE_DEMO
    dtc_init();            // no CAN: a read reports "no module answered"
    demo_start();
    ESP_LOGI(TAG, "demo mode: CAN not started");
#endif

    // Internal RAM is the tight one: draw buffers, bounce buffers, stacks and
    // LVGL's small objects all come from it first.
    ESP_LOGI(TAG, "free: internal %u KB (largest block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
