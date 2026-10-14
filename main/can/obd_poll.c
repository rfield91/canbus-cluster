#include "obd_poll.h"
#include "can_bus.h"
#include "vehicle.h"
#include "signals.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "OBD";

#define MAX_PIDS 32

static int64_t       s_due_ms[MAX_PIDS];
static volatile bool s_paused;

void obd_poll_pause(bool paused) { s_paused = paused; }

static void send_request(const obd_pid_t *p)
{
    // [length][service][pid...], padded with 0xAA, which the ECM ignores.
    uint8_t d[8] = { 0, 0, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    d[1] = p->mode;
    if (p->mode == OBD_MODE_22) {
        d[0] = 3;
        d[2] = p->pid >> 8;
        d[3] = p->pid & 0xFF;
    } else {
        d[0] = 2;
        d[2] = p->pid & 0xFF;
    }
    // Never wait on a full queue: a dropped request is retried next lap.
    can_bus_send(ECM_REQ_ID, d, 0);
}

bool obd_poll_handle_frame(uint32_t id, const uint8_t *d, uint8_t dlc)
{
    if (id < OBD_RESP_LO || id > OBD_RESP_HI)
        return false;
    if (id != ECM_RESP_ID || dlc < 4 || (d[0] >> 4) != 0)
        return true;                    // an OBD reply, but not a single-frame one for us

    // Single frame: [length][service + 0x40][pid, 1 or 2 bytes][A][B]...
    obd_mode_t mode;
    uint16_t   pid;
    int        first;                   // index of data byte A
    if (d[1] == 0x41) {
        mode = OBD_MODE_01; pid = d[2]; first = 3;
    } else if (d[1] == 0x62) {
        mode = OBD_MODE_22; pid = (d[2] << 8) | d[3]; first = 4;
    } else {
#if CONFIG_DASH_OBD_DEBUG
        // 7F <service> <reason>. The PID is not echoed, so line this up with
        // the request that went out just before it.
        if (d[1] == 0x7F)
            ESP_LOGW(TAG, "ECM refused service 0x%02X, reason 0x%02X", d[2], d[3]);
#endif
        return true;
    }
    int len = d[0] & 0x0F;              // service + pid + data bytes

    for (int i = 0; i < VEHICLE_PID_COUNT; i++) {
        const obd_pid_t *p = &VEHICLE_PIDS[i];
        if (p->mode != mode || p->pid != pid)
            continue;
        if (first + p->nbytes > dlc || first - 1 + p->nbytes > len)
            return true;                // short answer
        uint32_t raw = d[first];
        if (p->nbytes == 2)
            raw = (raw << 8) | d[first + 1];
        float value = raw * p->scale + p->offset;
        signals_set(p->signal, value);
#if CONFIG_DASH_OBD_DEBUG
        ESP_LOGI(TAG, "%-9s %02X %04X  raw %5lu  -> %.2f", signals_name(p->signal),
                 mode, pid, (unsigned long)raw, value);
#endif
        return true;
    }
    return true;
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "polling %d PIDs on 0x%03X", VEHICLE_PID_COUNT, ECM_REQ_ID);
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        // Send the MOST OVERDUE PID, not the first due one in table order:
        // if the table ever asks for more than the 50/s the tick allows,
        // the shortfall is spread evenly instead of starving the last rows.
        int     pick  = -1;
        int64_t worst = 0;
        for (int i = 0; i < VEHICLE_PID_COUNT; i++) {
            int64_t late = now - s_due_ms[i];
            if (late >= 0 && (pick < 0 || late > worst)) {
                pick  = i;
                worst = late;
            }
        }
        if (pick >= 0 && !s_paused) {
            send_request(&VEHICLE_PIDS[pick]);
            s_due_ms[pick] = now + VEHICLE_PIDS[pick].period_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(OBD_POLL_TICK_MS));
    }
}

void obd_poll_start(void)
{
    if (VEHICLE_PID_COUNT > MAX_PIDS) {
        ESP_LOGE(TAG, "%d PIDs in the profile, room for %d", VEHICLE_PID_COUNT, MAX_PIDS);
        return;
    }
    xTaskCreatePinnedToCore(poll_task, "obd_poll", 3072, NULL, 8, NULL, 0);
}
