#include "can_bus.h"
#include "obd_poll.h"
#include "dtc.h"

#include <string.h>
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "CAN";

#define CAN_TX GPIO_NUM_20
#define CAN_RX GPIO_NUM_19

// Deep enough to ride out a busy frame holding core 0. The ECM alone sends
// little, but this costs almost nothing.
#define RX_QUEUE_LEN 64

// 64-bit, written on core 0 and read on core 1: a torn read can at worst
// misjudge staleness for one frame, which the dash's 2s window absorbs.
static volatile int64_t s_last_reply_us;

int64_t can_bus_last_reply_us(void) { return s_last_reply_us; }

void can_bus_init(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
    g.rx_queue_len = RX_QUEUE_LEN;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "up at 500k on TX=GPIO%d RX=GPIO%d", CAN_TX, CAN_RX);
}

bool can_bus_send(uint32_t id, const uint8_t data[8], int timeout_ms)
{
    twai_message_t m = { .identifier = id, .data_length_code = 8 };
    memcpy(m.data, data, 8);
    return twai_transmit(&m, pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

static void rx_task(void *arg)
{
    twai_message_t m;
    for (;;) {
        if (twai_receive(&m, portMAX_DELAY) != ESP_OK || m.extd || m.rtr)
            continue;
        // Trouble-code answers first: they share reply ids with poll answers.
        if (dtc_handle_frame(m.identifier, m.data, m.data_length_code) ||
            obd_poll_handle_frame(m.identifier, m.data, m.data_length_code))
            s_last_reply_us = esp_timer_get_time();
    }
}

void can_bus_start(void)
{
    xTaskCreatePinnedToCore(rx_task, "can_rx", 4096, NULL, 10, NULL, 0);
}
