// SPDX-License-Identifier: Apache-2.0
#define main coordinator_suite_main
#include "../coordinator/test_coordinator.c"
#undef main

int main(void)
{
    test_case = "touch_trace";
    /* Startup, stationary contacts, private menus and not-ready input must
     * leave the single capture available for the actual test movement. */
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    assert(!app_touch_trace_active());
    pad_at(APP_TOUCH_BEGIN, 8001, 1000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 8001, 1015, 0, 0);
    assert(!app_touch_trace_active());
    pad_at(APP_TOUCH_END, 8001, 1030, 100, 100);
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    s_overlay = APP_OVERLAY_SETTINGS; ++s_ui_revision; sync_interaction();
    pad_at(APP_TOUCH_BEGIN, 8002, 2000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 8002, 2015, 10, 0);
    assert(!app_touch_trace_active());
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    hid_engine_set_link(&s_engine, true, false); sync_interaction();
    pad_at(APP_TOUCH_BEGIN, 8003, 3000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 8003, 3015, 10, 0);
    assert(!app_touch_trace_active());

    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    pad_at(APP_TOUCH_BEGIN, 8100, 4000, 100, 100);
    uint16_t x = 100, y = 100;
    for (unsigned i = 1; i <= 120; ++i) {
        const uint64_t now = 4000U + 15U * i;
        const int dx = i % 20 < 10 ? 3 : -3, dy = i % 12 < 6 ? 2 : -2;
        x = (uint16_t)((int)x + dx); y = (uint16_t)((int)y + dy);
        test_link.congested = i % 4 != 0;
        app_touch_trace_loop(now, test_link.generation, test_link.congested, true);
        s_now_ms = now;
        board_touch_snapshot_t sample = {.sequence = i, .timestamp_ms = now, .valid = true,
            .count = 1, .contacts = {{.id = 0, .x = x, .y = y}}};
        assert(submit_contacts(&sample)); consume_one();
        pad_at(APP_TOUCH_MOVE, 8100, now, dx, dy);
        if (!test_link.congested) (void)step();
    }
#if APP_TOUCH_TRACE_ENABLED
    assert(app_touch_trace_active());
#endif
    test_link.congested = false;
    while (s_engine.count) assert(step() == HID_ENGINE_STEP_SENT);
    for (size_t i = 0; i < trace_count; ++i)
        printf("REPORT %u %u %u %u %u\n", trace[i].id, trace[i].bytes[0], trace[i].bytes[1], trace[i].bytes[2], trace[i].bytes[3]);
#if APP_TOUCH_TRACE_ENABLED
    app_touch_trace_loop(24015, test_link.generation, false, false);
    app_touch_trace_snapshot_t frozen;
    assert(app_touch_trace_take(&frozen));
    assert(frozen.start_us == 4015000 && frozen.motion_enqueues == 120 && frozen.motion_merged > 0);
    assert(frozen.scans == 119 && frozen.invalid_scans == 0 && frozen.missing_scans == 0);
    assert(frozen.sdk_started == 0); /* This fixture substitutes only the transport boundary. */
#endif
    return 0;
}
