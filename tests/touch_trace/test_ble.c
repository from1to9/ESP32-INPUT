// SPDX-License-Identifier: Apache-2.0
#define main ble_suite_main
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "../ble_hid/test_ble_hid.c"
#pragma GCC diagnostic pop
#undef main

static void early_completion(void) { complete_notification(ESP_GATT_OK); }

int main(void)
{
    reset(); connect_peer(); subscribe();
    fake_uptime_us = 1000000;
#if APP_TOUCH_TRACE_ENABLED
    app_touch_trace_start(1000000, ble_hid_snapshot().generation, 91, 1000);
#endif
    const uint64_t generation = ble_hid_snapshot().generation;
    const uint8_t move[] = {0, 7, 253, 0};
    const unsigned clocks_before = uptime_reads;
    for (unsigned i = 0; i < 20; ++i) {
        fake_uptime_us += 15000;
        notify_hook = i == 1 ? early_completion : NULL;
        notify_result = i == 2 ? ESP_ERR_NO_MEM : ESP_OK;
        bool sent = ble_hid_send_generation(generation, 2, move, sizeof move);
        printf("BLE %u %d %u %u\n", i, sent, state.mouse[1], state.mouse[2]);
        if (sent && i != 1) {
            fake_uptime_us += 7000;
            complete_notification(ESP_GATT_OK);
        }
    }
#if APP_TOUCH_TRACE_ENABLED
    app_touch_trace_loop(21000, generation, false, false);
    app_touch_trace_snapshot_t frozen;
    assert(app_touch_trace_take(&frozen));
    assert(frozen.sdk_started == 20 && frozen.sdk_accepted == 19 && frozen.sdk_rejected == 1);
    assert(frozen.complete_ok == 19 && frozen.early_complete == 1 && !frozen.outstanding);
    assert(frozen.metrics[TOUCH_TRACE_COMPLETE_DURATION].max_us == 7000);
#endif
    fprintf(stderr, "TIMESTAMP_READS %u\n", uptime_reads - clocks_before);
    return 0;
}
