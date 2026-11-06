// SPDX-License-Identifier: Apache-2.0
#include <setjmp.h>

/* Run the actual coordinator loop. The RTOS delay boundary schedules the
 * independent 15/20 ms touch producer; it does not call consumer helpers. */
static jmp_buf schedule_done;
static unsigned schedule_turns, schedule_period_ms, schedule_rejected;
static unsigned schedule_source_ms, schedule_scans;
static int schedule_distance, schedule_position;
static uint64_t schedule_scan_ms, schedule_epoch;
static uint32_t schedule_gesture;
static size_t schedule_reports;
static int schedule_emitted;

static void schedule_check_reports(void)
{
    assert(trace_count - schedule_reports <= 1); /* No catch-up report burst. */
    for (; schedule_reports < trace_count; ++schedule_reports)
        if (trace[schedule_reports].id == HID_ENGINE_MOUSE_REPORT_ID)
            schedule_emitted += (int8_t)trace[schedule_reports].bytes[1];
    if (schedule_turns > 3 && schedule_period_ms < INTERACTION_CONTACT_TIMEOUT_MS) {
        assert(schedule_emitted == schedule_distance);
        assert(!uxQueueMessagesWaiting(s_intents));
    }
}

static void schedule_touch_source(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(COORDINATOR_PERIOD_MS));
    schedule_check_reports();
    if (++schedule_turns == (schedule_period_ms >= 250 ? 20U : schedule_period_ms == 120 ? 40U : 120U)) longjmp(schedule_done, 1);
    if (schedule_turns <= 3) {
        test_clock_ms += COORDINATOR_PERIOD_MS;
        if (schedule_turns == 3) {
            assert(!s_engine.state.release_pending);
            schedule_epoch = s_engine.state.epoch;
            app_intent_t begin = {.action = APP_TOUCH_BEGIN, .epoch = schedule_epoch,
                .ui_revision = s_ui_revision, .gesture = schedule_gesture,
                .surface = APP_SURFACE_TOUCHPAD, .x = 100, .y = 100};
            assert(submit_intent(&begin));
            schedule_scan_ms = test_clock_ms + schedule_source_ms;
        }
        return;
    }
    const uint64_t deadline = test_clock_ms + schedule_period_ms;
    while (schedule_scan_ms <= deadline) {
        test_clock_ms = schedule_scan_ms;
        int dx = schedule_scans++ % 40U < 20U ? 2 : -2;
        schedule_distance += dx; schedule_position += dx;
        board_touch_snapshot_t sample = {.sequence = schedule_scans,
            .timestamp_ms = schedule_scan_ms, .valid = true, .count = 1,
            .contacts = {{.id = 3, .x = (uint16_t)schedule_position, .y = 100}}};
        if (!submit_contacts(&sample)) ++schedule_rejected;
        app_intent_t move = {.action = APP_TOUCH_MOVE, .epoch = schedule_epoch,
            .ui_revision = s_ui_revision, .gesture = schedule_gesture,
            .surface = APP_SURFACE_TOUCHPAD, .x = dx};
        if (!submit_intent(&move)) ++schedule_rejected;
        schedule_scan_ms += schedule_source_ms;
    }
    test_clock_ms = deadline;
}

static void mouse_schedule_drains_touch_bursts(void)
{
    for (unsigned transport = 0; transport < 2; ++transport) {
        for (unsigned period = 0; period < 5; ++period) {
            for (unsigned source = 0; source < 2; ++source) {
                transport_setup(INPUT_MOUSE, (app_input_transport_t)transport);
                schedule_epoch = s_engine.state.epoch; schedule_gesture = 9001;
                schedule_turns = schedule_rejected = 0;
                schedule_scans = 0; schedule_distance = 0; schedule_position = 100;
                schedule_reports = 0; schedule_emitted = 0;
                schedule_source_ms = source == 0 ? 15U : 20U;
                const unsigned periods[] = {10, 40, 60, 120, 300};
                schedule_period_ms = periods[period];
                schedule_scan_ms = 1015;
                test_clock_override = true; test_clock_ms = 1000;
                test_coordinator_delay = schedule_touch_source;
                if (!setjmp(schedule_done)) coordinator_run(ESP_OK);
                test_coordinator_delay = NULL; test_clock_override = false;
                int emitted = 0;
                for (size_t i = 0; i < trace_count; ++i)
                    if (trace[i].id == HID_ENGINE_MOUSE_REPORT_ID) emitted += (int8_t)trace[i].bytes[1];
                fprintf(stderr, "Mouse schedule transport=%u period_ms=%u source_ms=%u rejected=%u reports=%zu epoch=%llu->%llu queued=%u distance=%d/%d\n",
                    transport, schedule_period_ms, schedule_source_ms, schedule_rejected, trace_count,
                    (unsigned long long)schedule_epoch, (unsigned long long)s_engine.state.epoch,
                    uxQueueMessagesWaiting(s_intents), emitted, schedule_distance);
                if (schedule_period_ms < INTERACTION_CONTACT_TIMEOUT_MS) {
                    assert(!schedule_rejected && s_engine.state.epoch == schedule_epoch);
                    assert(s_interaction.touch.active && !s_engine.state.release_pending && trace_count > 20);
                    assert(emitted == schedule_distance);
                } else {
                    assert(!s_interaction.touch.active && emitted == 0);
                    if (schedule_period_ms >= HID_ENGINE_CONGESTION_TIMEOUT_MS)
                        assert(schedule_rejected && s_engine.state.epoch != schedule_epoch);
                }
            }
        }
    }
}

static struct {
    jmp_buf done;
    unsigned turn, receives, pause_ms;
    uint64_t epoch;
    size_t report_start;
} expiry;

static void pause_after_buffering_move(void)
{
    if (++expiry.receives != 3) return;
    assert(s_engine.buffered_motion_count == 1);
    assert(s_engine.motion_since_ms == test_clock_ms);
    /* Stop after admission, before the next receive. With the FIFO empty,
     * no subsequent input handler refreshes the coordinator's cached time. */
    test_clock_ms += expiry.pause_ms;
}

static void expiry_schedule(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(COORDINATOR_PERIOD_MS));
    ++expiry.turn;
    if (expiry.turn == 3) {
        assert(!s_engine.state.release_pending);
        app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
        begin.surface = APP_SURFACE_TOUCHPAD;
        begin.gesture = 9002;
        assert(submit_intent(&begin));
    } else if (expiry.turn == 4) {
        assert(s_interaction.touch.active);
        expiry.epoch = s_engine.state.epoch;
        expiry.report_start = trace_count;
        board_touch_snapshot_t sample = {.sequence = 1, .timestamp_ms = test_clock_ms,
            .valid = true, .count = 1, .contacts = {{.id = 3, .x = 102, .y = 100}}};
        assert(submit_contacts(&sample));
        app_intent_t move = make_intent(APP_TOUCH_MOVE, 2, 0, NULL);
        move.surface = APP_SURFACE_TOUCHPAD;
        move.gesture = 9002;
        assert(submit_intent(&move));
        test_before_intent_receive = pause_after_buffering_move;
        return;
    } else if (expiry.turn == 5) {
        test_before_intent_receive = NULL;
        assert(expiry.receives == 3 && trace_count == expiry.report_start + 1U);
        const report_t *report = &trace[expiry.report_start];
        fprintf(stderr, "Mouse pre-submit transport=%d age_ms=%u first_id=%u first_x=%d epoch=%llu->%llu\n",
            (int)s_input_transport, expiry.pause_ms, (unsigned)report->id, (int8_t)report->bytes[1],
            (unsigned long long)expiry.epoch, (unsigned long long)s_engine.state.epoch);
        if (expiry.pause_ms < 250) {
            assert(report->id == HID_ENGINE_MOUSE_REPORT_ID && report->bytes[1] == 2);
            assert(!report->bytes[0] && !report->bytes[2] && !report->bytes[3]);
            assert(s_engine.state.epoch == expiry.epoch && !s_engine.state.release_pending);
            longjmp(expiry.done, 1);
        }
        assert(report->id == HID_ENGINE_KEYBOARD_REPORT_ID && !s_engine.count && !s_engine.send_failures);
        assert(s_engine.state.epoch != expiry.epoch && s_engine.state.release_pending);
        for (size_t i = 0; i < report->length; ++i) assert(!report->bytes[i]);
    } else if (expiry.turn == 6) {
        assert(trace_count == expiry.report_start + 2U && !s_engine.state.release_pending);
        const report_t *report = &trace[expiry.report_start + 1U];
        assert(report->id == HID_ENGINE_MOUSE_REPORT_ID && !s_engine.send_failures);
        for (size_t i = 0; i < report->length; ++i) assert(!report->bytes[i]);
        longjmp(expiry.done, 1);
    }
    test_clock_ms += COORDINATOR_PERIOD_MS;
}

static void mouse_buffer_expiry_before_submit(void)
{
    const unsigned ages[] = {249, 250, 251, 300};
    for (unsigned transport = 0; transport < 2; ++transport) {
        for (size_t age = 0; age < sizeof ages / sizeof ages[0]; ++age) {
            transport_setup(INPUT_MOUSE, (app_input_transport_t)transport);
            expiry.turn = expiry.receives = 0;
            expiry.pause_ms = ages[age];
            test_clock_override = true;
            test_clock_ms = 1000;
            test_coordinator_delay = expiry_schedule;
            if (!setjmp(expiry.done)) coordinator_run(ESP_OK);
            test_coordinator_delay = NULL;
            test_clock_override = false;
        }
    }
}
