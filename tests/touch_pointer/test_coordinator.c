// SPDX-License-Identifier: Apache-2.0
/* Diagnostic regressions: production coordinator and domain logic unchanged.
 * Reuse only the established native hardware boundary substitutes. */
#define main coordinator_suite_main
#include "../coordinator/test_coordinator.c"
#undef main

static void pointer_busy(bool busy)
{
    if (s_input_transport == APP_INPUT_USB) test_usb.congested = busy;
    else test_link.congested = busy;
    input_link_t link = input_link_snapshot(&test_link);
    observe_input_link(&link, &test_previous_link);
    sync_interaction();
}

static int pointer_sum(void)
{
    int total = 0;
    for (size_t i = 0; i < trace_count; ++i)
        if (trace[i].id == HID_ENGINE_MOUSE_REPORT_ID)
            total += (int)(int8_t)trace[i].bytes[1];
    return total;
}

static bool exact_without_backpressure(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE, transport);
    pad_at(APP_TOUCH_BEGIN, 6001, 1000, 100, 100);
    for (unsigned i = 0; i < 3; ++i) {
        pad_at(APP_TOUCH_MOVE, 6001, 1010U + i * 10U, 20, 0);
        assert(step() == HID_ENGINE_STEP_SENT);
    }
    assert(!s_interaction.view.motion_enabled);
    printf("baseline transport=%d requested=60 emitted=%d queued=%zu\n", (int)transport, pointer_sum(), s_engine.count);
    return pointer_sum() == 60;
}

static bool transient_busy_conserves_delta(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE, transport);
    pad_at(APP_TOUCH_BEGIN, 6002, 1000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 6002, 1010, 20, 0);
    assert(step() == HID_ENGINE_STEP_SENT);
    const uint64_t epoch = s_engine.state.epoch;
    pointer_busy(true);
    pad_at(APP_TOUCH_MOVE, 6002, 1020, 20, 0);
    const size_t busy_queue = s_engine.count;
    pointer_busy(false);
    pad_at(APP_TOUCH_MOVE, 6002, 1030, 20, 0);
    while (s_engine.count) assert(step() == HID_ENGINE_STEP_SENT);
    assert(!s_interaction.view.motion_enabled && s_engine.state.epoch == epoch);
    printf("busy_move transport=%d requested=60 emitted=%d busy_queue=%zu recovered_active=%d same_epoch=1\n",
           (int)transport, pointer_sum(), busy_queue, s_interaction.touch.active);
    return pointer_sum() == 60;
}

static bool busy_begin_recovers_current_contact(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE, transport);
    pointer_busy(true);
    pad_at(APP_TOUCH_BEGIN, 6003, 1000, 100, 100);
    const bool active_when_busy = s_interaction.touch.active;
    pointer_busy(false);
    pad_at(APP_TOUCH_MOVE, 6003, 1010, 20, 0);
    while (s_engine.count) assert(step() == HID_ENGINE_STEP_SENT);
    printf("busy_begin transport=%d requested=20 emitted=%d active_when_busy=%d active_after_ready=%d\n",
           (int)transport, pointer_sum(), active_when_busy, s_interaction.touch.active);
    return pointer_sum() == 20;
}

static bool large_delta_conserves_distance(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE, transport);
    pad_at(APP_TOUCH_BEGIN, 6004, 1000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 6004, 1010, 300, 0);
    while (s_engine.count) assert(step() == HID_ENGINE_STEP_SENT);
    pad_at(APP_TOUCH_MOVE, 6004, 1020, 0, 0);
    while (s_engine.count) assert(step() == HID_ENGINE_STEP_SENT);
    printf("large_delta transport=%d requested=300 emitted=%d carry=%g queued=%zu\n",
           (int)transport, pointer_sum(), (double)s_interaction.touch_carry[0], s_engine.count);
    return pointer_sum() == 300;
}

static bool fifo_does_not_merge_movement(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE, transport);
    pad_at(APP_TOUCH_BEGIN, 6005, 1000, 100, 100);
    for (unsigned i = 0; i < 4; ++i)
        pad_at(APP_TOUCH_MOVE, 6005, 1010, 20, 0);
    const size_t queued = s_engine.count;
    unsigned service_steps = 0;
    while (s_engine.count) {
        assert(step() == HID_ENGINE_STEP_SENT);
        ++service_steps;
    }
    printf("fifo transport=%d requested=80 emitted=%d queue_before=%zu service_steps=%u\n",
           (int)transport, pointer_sum(), queued, service_steps);
    return queued == 1 && service_steps == 1 && pointer_sum() == 80;
}

static bool pointer_boundaries(app_input_transport_t transport)
{
    transport_setup(INPUT_MOUSE,transport);
    pad_at(APP_TOUCH_BEGIN,6101,1000,100,100);
    pointer_busy(true);
    pad_at(APP_TOUCH_MOVE,6101,1010,30,0);
    pad_at(APP_TOUCH_MOVE,6101,1020,-30,0);
    assert(!s_engine.count && !s_interaction.touch.candidate);
    pointer_busy(false);
    pad_at(APP_TOUCH_END,6101,1030,100,100);
    assert(!s_engine.count); /* The real path still exceeded tap slop. */
    pointer_busy(true);
    pad_at(APP_TOUCH_BEGIN,6102,1100,100,100);
    assert(s_interaction.touch.active && !s_interaction.touch.candidate && !s_interaction.touch.held);
    pointer_busy(false);
    pad_at(APP_TOUCH_END,6102,1120,100,100);
    assert(!s_engine.count); /* Busy BEGIN cannot grant a delayed tap or drag. */
    pad_at(APP_TOUCH_BEGIN,6103,1200,100,100);
    pad_at(APP_TOUCH_MOVE,6103,1210,300,0);
    pad_at(APP_TOUCH_END,6103,1220,400,100);
    while(s_engine.count)assert(step()==HID_ENGINE_STEP_SENT);
    assert(pointer_sum()==300);
    pad_at(APP_TOUCH_BEGIN,6104,1300,100,100);
    pad_at(APP_TOUCH_MOVE,6104,1310,300,0);
    pad_at(APP_TOUCH_CANCEL,6104,1320,400,100);
    assert(!s_engine.count && s_engine.state.release_pending);
    release_barrier();
    pad_at(APP_TOUCH_BEGIN,6105,1400,100,100);
    s_now_ms=1410;
    app_intent_t delayed=make_intent(APP_TOUCH_MOVE,30,0,NULL);
    delayed.gesture=6105;delayed.surface=APP_SURFACE_TOUCHPAD;
    assert(submit_intent(&delayed));
    s_now_ms=1660;consume_one();
    assert(!s_engine.count && s_engine.state.release_pending);
    release_barrier();
    pad_at(APP_TOUCH_END,6105,1670,130,100);
    pad_at(APP_TOUCH_BEGIN,6106,1700,100,100);
    for(unsigned i=0;i<13;++i)pad_at(APP_TOUCH_MOVE,6106,1710,300,0);
    assert(s_engine.count==1);
    pad_at(APP_TOUCH_MOVE,6106,1710,300,0);
    assert(!s_engine.count && s_engine.state.release_pending);
    puts("PASS: raw slop, busy-BEGIN no tap, END/CANCEL, stale source timestamp and overflow cancellation");
    return true;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "existing")) return coordinator_suite_main(1, argv);
    test_case = argv[1];
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        const app_input_transport_t transport = i == 0 ? APP_INPUT_USB : APP_INPUT_BLUETOOTH;
        bool result;
        if (!strcmp(argv[1], "baseline")) result = exact_without_backpressure(transport);
        else if (!strcmp(argv[1], "busy_move")) result = transient_busy_conserves_delta(transport);
        else if (!strcmp(argv[1], "busy_begin")) result = busy_begin_recovers_current_contact(transport);
        else if (!strcmp(argv[1], "large_delta")) result = large_delta_conserves_distance(transport);
        else if (!strcmp(argv[1], "fifo")) result = fifo_does_not_merge_movement(transport);
        else if (!strcmp(argv[1], "boundaries")) result = pointer_boundaries(transport);
        else return coordinator_suite_main(argc, argv);
        ok = ok && result;
    }
    return ok ? 0 : 2;
}
