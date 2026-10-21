// SPDX-License-Identifier: Apache-2.0
#include "hid_engine.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t attempts;
    size_t accepted;
    bool fail_next;
    uint8_t report_id;
    uint8_t data[HID_ENGINE_KEYBOARD_REPORT_LENGTH];
    size_t length;
} sink_t;

static bool send_report(void *context, uint8_t report_id,
                         const uint8_t *data, size_t length)
{
    sink_t *sink = context;
    assert(report_id == HID_ENGINE_KEYBOARD_REPORT_ID ||
           report_id == HID_ENGINE_MOUSE_REPORT_ID);
    assert(length == (report_id == HID_ENGINE_KEYBOARD_REPORT_ID ? 8U : 4U));
    ++sink->attempts;
    sink->report_id = report_id;
    sink->length = length;
    memcpy(sink->data, data, length);
    if (sink->fail_next) {
        sink->fail_next = false;
        return false;
    }
    ++sink->accepted;
    return true;
}

static hid_engine_step_result_t step(hid_engine_t *engine, sink_t *sink)
{
    const size_t attempts = sink->attempts;
    const size_t accepted = sink->accepted;
    const hid_engine_step_result_t result =
        hid_engine_step(engine, send_report, sink);
    assert(sink->attempts - attempts <= 1);
    assert(sink->accepted - accepted ==
           (result == HID_ENGINE_STEP_SENT ? 1U : 0U));
    assert(engine->count <= HID_ENGINE_QUEUE_CAPACITY);
    assert(engine->text_completed <= engine->text_accepted);
    assert(engine->send_failures <= HID_ENGINE_MAX_SEND_FAILURES);
    return result;
}

static void expect_report(hid_engine_t *engine, sink_t *sink,
                           uint8_t id, const uint8_t *data, size_t length)
{
    assert(step(engine, sink) == HID_ENGINE_STEP_SENT);
    assert(sink->report_id == id && sink->length == length);
    assert(memcmp(sink->data, data, length) == 0);
}

static void expect_zero(hid_engine_t *engine, sink_t *sink, uint8_t id)
{
    static const uint8_t zero[8] = {0};
    expect_report(engine, sink, id, zero, id == 1 ? 8U : 4U);
}

static void release_barrier(hid_engine_t *engine, sink_t *sink)
{
    assert(engine->state.release_pending);
    expect_zero(engine, sink, 1);
    assert(engine->state.release_pending);
    expect_zero(engine, sink, 2);
    assert(!engine->state.release_pending);
}

static void connected(hid_engine_t *engine, sink_t *sink, input_mode_t mode)
{
    hid_engine_init(engine);
    hid_engine_set_link(engine, true, true);
    assert(hid_engine_set_mode(engine, mode));
    release_barrier(engine, sink);
}

static void initial_and_readiness(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    hid_engine_init(&engine);
    assert(engine.state.mode == INPUT_MOUSE && engine.state.owner == INPUT_LOCAL);
    assert(engine.state.epoch == 1 && engine.state.release_pending);
    assert(!engine.state.connected && !engine.ready && !engine.state.locked);
    assert(!engine.state.gyro_enabled && engine.count == 0);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
    assert(!hid_engine_set_gyro(&engine, true));
    hid_engine_set_link(&engine, false, true);
    assert(!engine.ready && engine.state.epoch == 1);
    hid_engine_set_link(&engine, true, false);
    assert(engine.state.connected && !engine.ready);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
    hid_engine_set_link(&engine, true, true);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 1, 1) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    expect_zero(&engine, &sink, 1);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    expect_zero(&engine, &sink, 2);
    assert(!engine.state.release_pending);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
    assert(hid_engine_step(&engine, NULL, &sink) == HID_ENGINE_STEP_BLOCKED);
    assert(hid_engine_set_gyro(&engine, true));
    hid_engine_set_link(&engine, true, true);
    assert(hid_engine_set_mode(&engine, INPUT_MOUSE));
    assert(hid_engine_set_owner(&engine, INPUT_LOCAL));
    hid_engine_set_locked(&engine, false);
    assert(!hid_engine_set_mode(&engine, (input_mode_t)99));
    assert(!hid_engine_set_owner(&engine, (input_owner_t)99));
    assert(engine.state.epoch == epoch && engine.state.gyro_enabled);
    assert(sink.attempts == 2);
}

static void mouse_sequence(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, -127, 127) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 7) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, -127) ==
           HID_ENGINE_ENQUEUE_OK);
    const uint8_t move[] = {0, 0x81, 0x7f, 0};
    const uint8_t click[] = {7, 0, 0, 0};
    const uint8_t scroll[] = {0, 0, 0, 0x81};
    expect_report(&engine, &sink, 2, move, sizeof move);
    assert(engine.count == 2);
    expect_report(&engine, &sink, 2, click, sizeof click);
    assert(engine.count == 2); /* Click slot retains its required release. */
    expect_zero(&engine, &sink, 2);
    assert(engine.count == 1);
    expect_report(&engine, &sink, 2, scroll, sizeof scroll);
    assert(engine.count == 0 && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "a", 1) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, 127) ==
           HID_ENGINE_ENQUEUE_OK);
    const uint8_t positive_scroll[] = {0, 0, 0, 0x7f};
    expect_report(&engine, &sink, 2, positive_scroll, sizeof positive_scroll);
}

static void drag_sequence(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    const uint64_t epoch = engine.state.epoch;
    const uint8_t down[] = {1, 0, 0, 0};
    const uint8_t before_drag[] = {0, 3, 0xfd, 0};
    const uint8_t move[] = {1, 0x81, 0x7f, 0};
    const uint8_t scroll[] = {1, 0, 0, 0xff};
    const uint8_t after_drag[] = {0, 2, 1, 0};
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 3, -3) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, -127, 127) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, -1) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 2) == HID_ENGINE_ENQUEUE_BLOCKED);
    assert(engine.mouse_buttons == 0); /* Enqueue acceptance is not transport acceptance. */
    expect_report(&engine, &sink, 2, before_drag, sizeof before_drag);
    expect_report(&engine, &sink, 2, down, sizeof down);
    assert(engine.mouse_buttons == 1 && engine.count == 2);
    expect_report(&engine, &sink, 2, move, sizeof move);
    expect_report(&engine, &sink, 2, scroll, sizeof scroll);
    assert(engine.mouse_buttons == 1 && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 2, 1) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_BLOCKED);
    expect_zero(&engine, &sink, 2);
    assert(engine.mouse_buttons == 0);
    expect_report(&engine, &sink, 2, after_drag, sizeof after_drag);
    assert(engine.count == 0 && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);

    /* A preceding ordinary click completes its release before the queued drag. */
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 2) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    const uint8_t right_click[] = {2, 0, 0, 0};
    expect_report(&engine, &sink, 2, right_click, sizeof right_click);
    expect_zero(&engine, &sink, 2);
    expect_report(&engine, &sink, 2, down, sizeof down);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    expect_zero(&engine, &sink, 2);
    assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    expect_report(&engine, &sink, 2, down, sizeof down);
    expect_zero(&engine, &sink, 2);
    assert(engine.mouse_buttons == 0 && engine.count == 0);
}

static void drag_capacity_and_validation(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    const uint64_t epoch = engine.state.epoch;
    const unsigned int invalid[] = {2, 3, 7, 8, UINT_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, invalid[i]) == HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch - 1, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_STALE);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_REMOTE, 1) == HID_ENGINE_ENQUEUE_STALE);
    assert(engine.count == 0 && engine.mouse_buttons == 0);

    /* Offset the ring first; pending holds and releases must scan across wrap. */
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 1, 0) == HID_ENGINE_ENQUEUE_OK);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    for (size_t i = 0; i < HID_ENGINE_QUEUE_CAPACITY - 2; ++i)
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 2, 0) == HID_ENGINE_ENQUEUE_OK);
    assert(engine.count == HID_ENGINE_QUEUE_CAPACITY - 1);
    hid_engine_t before = engine;
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 3, 0) == HID_ENGINE_ENQUEUE_FULL);
    assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_FULL);
    assert(memcmp(&engine, &before, sizeof engine) == 0);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    assert(engine.count == HID_ENGINE_QUEUE_CAPACITY);
    const uint8_t down[] = {1, 0, 0, 0};
    const uint8_t held_move[] = {1, 2, 0, 0};
    expect_report(&engine, &sink, 2, down, sizeof down);
    for (size_t i = 0; i < HID_ENGINE_QUEUE_CAPACITY - 2; ++i)
        expect_report(&engine, &sink, 2, held_move, sizeof held_move);
    expect_zero(&engine, &sink, 2);
    assert(engine.count == 0 && engine.mouse_buttons == 0);

    for (size_t i = 0; i < HID_ENGINE_QUEUE_CAPACITY - 1; ++i)
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 2, 0) == HID_ENGINE_ENQUEUE_OK);
    before = engine;
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_FULL);
    assert(memcmp(&engine, &before, sizeof engine) == 0);
    hid_engine_cancel(&engine);
    release_barrier(&engine, &sink);

    /* An already accepted hold retains the same guaranteed release capacity. */
    assert(hid_engine_enqueue_mouse_buttons(&engine, engine.state.epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    expect_report(&engine, &sink, 2, down, sizeof down);
    for (size_t i = 0; i < HID_ENGINE_QUEUE_CAPACITY - 1; ++i)
        assert(hid_engine_enqueue_mouse_scroll(&engine, engine.state.epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_scroll(&engine, engine.state.epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_FULL);
    assert(hid_engine_enqueue_mouse_buttons(&engine, engine.state.epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    const uint8_t held_scroll[] = {1, 0, 0, 1};
    for (size_t i = 0; i < HID_ENGINE_QUEUE_CAPACITY - 1; ++i)
        expect_report(&engine, &sink, 2, held_scroll, sizeof held_scroll);
    expect_zero(&engine, &sink, 2);
    assert(engine.count == 0 && engine.mouse_buttons == 0);
}

static void drag_interruptions(void)
{
    for (unsigned transition = 0; transition < 9; ++transition) {
        for (size_t sent = 0; sent < 5; ++sent) {
            hid_engine_t engine;
            sink_t sink = {0};
            connected(&engine, &sink, INPUT_MOUSE);
            const uint64_t epoch = engine.state.epoch;
            assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 5, 2) == HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 7, 0) == HID_ENGINE_ENQUEUE_OK);
            for (size_t i = 0; i < sent; ++i) assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            switch (transition) {
            case 0: hid_engine_cancel(&engine); break;
            case 1: hid_engine_set_locked(&engine, true); break;
            case 2: assert(hid_engine_set_owner(&engine, INPUT_REMOTE)); break;
            case 3: assert(hid_engine_set_mode(&engine, INPUT_KEYBOARD)); break;
            case 4: assert(hid_engine_set_mode(&engine, INPUT_AUDIO)); break;
            case 5: hid_engine_set_link(&engine, false, false); break;
            case 6: hid_engine_set_link(&engine, true, false); break;
            case 7:
                assert(!hid_engine_observe_transport(&engine, 1000, true));
                assert(hid_engine_observe_transport(&engine, 1250, true));
                assert(!hid_engine_observe_transport(&engine, 1251, false));
                break;
            default:
                assert(!hid_engine_observe_transport(&engine, 1000, false));
                assert(hid_engine_observe_transport(&engine, 999, false));
                break;
            }
            assert(engine.mouse_buttons == 0 && engine.count == 0 && engine.state.release_pending);
            assert(engine.state.epoch != epoch);
            assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_STALE);
            if (transition == 5 || transition == 6) assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
            hid_engine_set_link(&engine, true, true);
            hid_engine_set_locked(&engine, false);
            assert(hid_engine_set_mode(&engine, INPUT_MOUSE));
            assert(hid_engine_set_owner(&engine, INPUT_LOCAL));
            release_barrier(&engine, &sink);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
            assert(hid_engine_enqueue_mouse_move(&engine, engine.state.epoch, INPUT_LOCAL, 3, 0) == HID_ENGINE_ENQUEUE_OK);
            const uint8_t unheld[] = {0, 3, 0, 0};
            expect_report(&engine, &sink, 2, unheld, sizeof unheld);
            assert(engine.mouse_buttons == 0);
        }
    }
}

static void drag_failures_and_short_congestion(void)
{
    for (size_t sent = 0; sent < 4; ++sent) {
        hid_engine_t engine;
        sink_t sink = {0};
        connected(&engine, &sink, INPUT_MOUSE);
        const uint64_t epoch = engine.state.epoch;
        assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 5, 2) == HID_ENGINE_ENQUEUE_OK);
        assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
        assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
        for (size_t i = 0; i < sent; ++i) assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
        sink.fail_next = true;
        assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
        assert(engine.mouse_buttons == 0 && engine.count == 0 && engine.state.release_pending);
        assert(engine.state.epoch != epoch && engine.send_failures == 1);
        release_barrier(&engine, &sink);
        assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
        assert(hid_engine_enqueue_mouse_move(&engine, engine.state.epoch, INPUT_LOCAL, 3, 0) == HID_ENGINE_ENQUEUE_OK);
        const uint8_t unheld[] = {0, 3, 0, 0};
        expect_report(&engine, &sink, 2, unheld, sizeof unheld);
    }
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    uint64_t epoch = engine.state.epoch;
    const uint8_t down[] = {1, 0, 0, 0};
    const uint8_t held_move[] = {1, 5, 2, 0};
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    expect_report(&engine, &sink, 2, down, sizeof down);
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 5, 2) == HID_ENGINE_ENQUEUE_OK);
    assert(!hid_engine_observe_transport(&engine, 1000, true));
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 1, 0) == HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    size_t attempts = sink.attempts;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED && sink.attempts == attempts);
    assert(engine.mouse_buttons == 1 && engine.send_failures == 0);
    assert(!hid_engine_observe_transport(&engine, 1249, false));
    expect_report(&engine, &sink, 2, held_move, sizeof held_move);
    expect_zero(&engine, &sink, 2);
    assert(engine.mouse_buttons == 0 && engine.state.epoch == epoch);

    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    expect_report(&engine, &sink, 2, down, sizeof down);
    assert(hid_engine_enqueue_mouse_buttons(&engine, epoch, INPUT_LOCAL, 0) == HID_ENGINE_ENQUEUE_OK);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED && sink.data[0] == 0);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_RETRY_EXHAUSTED && sink.data[0] == 0);
    assert(engine.mouse_buttons == 0 && !engine.ready && engine.state.release_pending);
    hid_engine_set_link(&engine, false, false);
    hid_engine_set_link(&engine, true, true);
    release_barrier(&engine, &sink);
    assert(engine.mouse_buttons == 0 && engine.count == 0 && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
}

static void interrupted_release_barrier(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    hid_engine_init(&engine);
    hid_engine_set_link(&engine, true, true);
    const uint64_t epoch = engine.state.epoch;
    expect_zero(&engine, &sink, 1);
    hid_engine_set_link(&engine, true, true); /* Duplicate status preserves progress. */
    assert(engine.state.epoch == epoch);
    assert(hid_engine_set_mode(&engine, INPUT_KEYBOARD));
    const uint64_t keyboard_epoch = engine.state.epoch;
    assert(keyboard_epoch != epoch && engine.state.release_pending);
    expect_zero(&engine, &sink, 1); /* Earlier keyboard release belonged to old epoch. */
    assert(hid_engine_enqueue_key(&engine, keyboard_epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    hid_engine_set_locked(&engine, true);
    assert(engine.state.epoch != keyboard_epoch);
    release_barrier(&engine, &sink);
    assert(engine.state.locked && !engine.state.release_pending);
    assert(!hid_engine_set_owner(&engine, INPUT_REMOTE));
    hid_engine_set_locked(&engine, false);
    release_barrier(&engine, &sink);
    assert(hid_engine_enqueue_key(&engine, keyboard_epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_STALE);
    assert(hid_engine_enqueue_key(&engine, engine.state.epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_OK);
}

static void explicit_keys_and_text(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_KEYBOARD);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 255, 0x65) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0x80, 0) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 40) ==
           HID_ENGINE_ENQUEUE_OK);
    const uint8_t chord[] = {255, 0, 0x65, 0, 0, 0, 0, 0};
    const uint8_t modifier[] = {0x80, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t enter[] = {0, 0, 40, 0, 0, 0, 0, 0};
    expect_report(&engine, &sink, 1, chord, sizeof chord);
    expect_zero(&engine, &sink, 1);
    expect_report(&engine, &sink, 1, modifier, sizeof modifier);
    expect_zero(&engine, &sink, 1);
    expect_report(&engine, &sink, 1, enter, sizeof enter);
    expect_zero(&engine, &sink, 1);
    assert(engine.text_accepted == 0 && engine.text_completed == 0);

    char text[] = "aA0!? ";
    const uint8_t usages[] = {4, 4, 39, 30, 56, 44};
    const uint8_t modifiers[] = {0, 2, 0, 2, 2, 0};
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, text, 6) ==
           HID_ENGINE_ENQUEUE_OK);
    memset(text, 'z', 6); /* Enqueue must own its copy. */
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "x", 1) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t key[] = {modifiers[i], 0, usages[i], 0, 0, 0, 0, 0};
        expect_report(&engine, &sink, 1, key, sizeof key);
        assert(engine.text_accepted == i + 1 && engine.text_completed == i);
        expect_zero(&engine, &sink, 1);
        assert(engine.text_completed == i + 1 && engine.text_pending == 5 - i);
    }
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE); /* No appended Enter. */
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "x", 1) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(engine.text_accepted == 0 && engine.text_completed == 0);
}

static void invalid_ranges(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    const uint64_t epoch = engine.state.epoch;
    const int invalid[] = {INT_MIN, -128, 128, INT_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL,
                                             invalid[i], 0) ==
               HID_ENGINE_ENQUEUE_INVALID);
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL,
                                             0, invalid[i]) ==
               HID_ENGINE_ENQUEUE_INVALID);
        assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL,
                                               invalid[i]) ==
               HID_ENGINE_ENQUEUE_INVALID);
    }
    const unsigned int bad_buttons[] = {0, 8, 255, UINT_MAX};
    for (size_t i = 0; i < sizeof bad_buttons / sizeof bad_buttons[0]; ++i)
        assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL,
                                              bad_buttons[i]) ==
               HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_mouse_move(&engine, epoch, (input_owner_t)9, 0, 0) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(engine.count == 0);
    assert(hid_engine_set_mode(&engine, INPUT_KEYBOARD));
    release_barrier(&engine, &sink);
    const uint64_t key_epoch = engine.state.epoch;
    const unsigned int bad_usages[] = {1, 2, 3, 0x66, 255, UINT_MAX};
    for (size_t i = 0; i < sizeof bad_usages / sizeof bad_usages[0]; ++i)
        assert(hid_engine_enqueue_key(&engine, key_epoch, INPUT_LOCAL, 0,
                                       bad_usages[i]) == HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_key(&engine, key_epoch, INPUT_LOCAL, 0, 0) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_key(&engine, key_epoch, INPUT_LOCAL, 256, 4) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_key(&engine, key_epoch, INPUT_LOCAL, UINT_MAX, 4) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL, NULL, 1) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL, "a", 0) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL, "a", SIZE_MAX) ==
           HID_ENGINE_ENQUEUE_INVALID); /* Reject length before reading. */
    char too_long[HID_ENGINE_TEXT_MAX_LENGTH + 1];
    memset(too_long, 'a', sizeof too_long);
    assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL,
                                    too_long, sizeof too_long) ==
           HID_ENGINE_ENQUEUE_INVALID);
    const char bad_text[][3] = {{'a', '\n', 'b'}, {'a', 0, 'b'}, {'a', 0x7f, 'b'}};
    for (size_t i = 0; i < sizeof bad_text / sizeof bad_text[0]; ++i)
        assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL,
                                        bad_text[i], 3) == HID_ENGINE_ENQUEUE_INVALID);
    const unsigned char high[] = {0x80, 0xff};
    assert(hid_engine_enqueue_text(&engine, key_epoch, INPUT_LOCAL,
                                    (const char *)high, sizeof high) ==
           HID_ENGINE_ENQUEUE_INVALID);
    assert(engine.count == 0 && engine.text_accepted == 0);
    assert(hid_engine_set_mode(&engine, INPUT_AUDIO));
    release_barrier(&engine, &sink);
    assert(hid_engine_enqueue_key(&engine, engine.state.epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_mouse_move(&engine, engine.state.epoch,
                                          INPUT_LOCAL, 1, 0) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
}

static void queue_capacity_and_wrap(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_KEYBOARD);
    const uint64_t epoch = engine.state.epoch;
    for (size_t i = 0; i < 31; ++i)
        assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 4) ==
               HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "ab", 2) ==
           HID_ENGINE_ENQUEUE_FULL);
    assert(engine.count == 31 && engine.text_pending == 0);
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "A", 1) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_FULL);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
    assert(engine.count == 32);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_FULL);
    expect_zero(&engine, &sink, 1);
    assert(engine.count == 31);
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 5) ==
           HID_ENGINE_ENQUEUE_OK); /* Wrap tail into released slot. */
    for (size_t i = 0; i < 30; ++i) {
        const uint8_t key[] = {0, 0, 4, 0, 0, 0, 0, 0};
        expect_report(&engine, &sink, 1, key, sizeof key);
        expect_zero(&engine, &sink, 1);
    }
    const uint8_t upper[] = {2, 0, 4, 0, 0, 0, 0, 0};
    const uint8_t last[] = {0, 0, 5, 0, 0, 0, 0, 0};
    expect_report(&engine, &sink, 1, upper, sizeof upper);
    expect_zero(&engine, &sink, 1);
    expect_report(&engine, &sink, 1, last, sizeof last);
    expect_zero(&engine, &sink, 1);
    assert(engine.text_accepted == 1 && engine.text_completed == 1);
    assert(engine.count == 0);
    char text[HID_ENGINE_TEXT_MAX_LENGTH];
    memset(text, 'z', sizeof text);
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL,
                                    text, sizeof text) == HID_ENGINE_ENQUEUE_OK);
    assert(engine.count == 32);
    for (size_t i = 0; i < 32; ++i) {
        assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
        expect_zero(&engine, &sink, 1);
    }
    assert(engine.text_accepted == 32 && engine.text_completed == 32);
    assert(engine.count == 0 && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
}

static void interruption_sequences(void)
{
    /* Interrupt before a key-down, after key-down, after key-up and mid-batch. */
    for (unsigned int transition = 0; transition < 6; ++transition) {
        for (size_t sent = 0; sent < 4; ++sent) {
            hid_engine_t engine;
            sink_t sink = {0};
            connected(&engine, &sink, INPUT_KEYBOARD);
            const uint64_t epoch = engine.state.epoch;
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "abc", 3) ==
                   HID_ENGINE_ENQUEUE_OK);
            for (size_t i = 0; i < sent; ++i)
                assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            switch (transition) {
            case 0: assert(hid_engine_set_mode(&engine, INPUT_MOUSE)); break;
            case 1: hid_engine_set_locked(&engine, true); break;
            case 2: assert(hid_engine_set_owner(&engine, INPUT_REMOTE)); break;
            case 3: hid_engine_set_link(&engine, true, false); break;
            case 4: hid_engine_set_link(&engine, false, false); break;
            default: hid_engine_cancel(&engine); break;
            }
            assert(engine.state.epoch != epoch && engine.state.release_pending);
            assert(engine.count == 0 && engine.text_pending == 0);
            assert(engine.text_accepted == (sent + 1) / 2);
            assert(engine.text_completed == sent / 2);
            assert(!engine.state.gyro_enabled);
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "a", 1) ==
                   HID_ENGINE_ENQUEUE_STALE);
            hid_engine_set_link(&engine, true, true);
            release_barrier(&engine, &sink);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
            if (transition == 1) {
                assert(engine.state.owner == INPUT_LOCAL);
                assert(!hid_engine_set_owner(&engine, INPUT_REMOTE));
                assert(!hid_engine_set_mode(&engine, INPUT_MOUSE));
                assert(hid_engine_enqueue_text(&engine, engine.state.epoch,
                                                INPUT_LOCAL, "a", 1) ==
                       HID_ENGINE_ENQUEUE_BLOCKED);
                hid_engine_set_locked(&engine, false);
                release_barrier(&engine, &sink);
                assert(engine.state.owner == INPUT_LOCAL);
            }
            if (transition == 2) {
                assert(hid_engine_enqueue_text(&engine, engine.state.epoch,
                                                INPUT_LOCAL, "a", 1) ==
                       HID_ENGINE_ENQUEUE_STALE);
                assert(hid_engine_enqueue_text(&engine, engine.state.epoch,
                                                INPUT_REMOTE, "a", 1) ==
                       HID_ENGINE_ENQUEUE_OK);
                hid_engine_set_locked(&engine, true);
                assert(engine.state.owner == INPUT_LOCAL && engine.count == 0);
            }
        }
    }

    /* Cancelling a pressed mouse releases it before any later movement. */
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    assert(hid_engine_set_gyro(&engine, true));
    assert(hid_engine_enqueue_mouse_click(&engine, engine.state.epoch,
                                           INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT && sink.data[0] == 1);
    hid_engine_cancel(&engine);
    assert(!engine.state.gyro_enabled && engine.count == 0);
    release_barrier(&engine, &sink);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);

    /* Disconnect revokes a remote owner, including a ready=false interval. */
    assert(hid_engine_set_owner(&engine, INPUT_REMOTE));
    release_barrier(&engine, &sink);
    hid_engine_set_link(&engine, false, false);
    assert(engine.state.owner == INPUT_LOCAL);
    hid_engine_set_link(&engine, true, true);
    release_barrier(&engine, &sink);
    assert(engine.state.owner == INPUT_LOCAL && !engine.state.gyro_enabled);
}

static void failure_sequences(void)
{
    for (size_t sent = 0; sent < 6; ++sent) {
        hid_engine_t engine;
        sink_t sink = {0};
        connected(&engine, &sink, INPUT_KEYBOARD);
        const uint64_t epoch = engine.state.epoch;
        assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "abc", 3) ==
               HID_ENGINE_ENQUEUE_OK);
        for (size_t i = 0; i < sent; ++i)
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
        sink.fail_next = true;
        assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
        assert(engine.state.epoch != epoch && engine.count == 0);
        assert(engine.state.release_pending && engine.ready);
        assert(engine.send_failures == 1 && engine.text_pending == 0);
        assert(engine.text_accepted == (sent + 1) / 2);
        assert(engine.text_completed == sent / 2);
        release_barrier(&engine, &sink);
        assert(engine.send_failures == 0);
        assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
        assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "a", 1) ==
               HID_ENGINE_ENQUEUE_STALE);
    }

    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    assert(hid_engine_enqueue_mouse_click(&engine, engine.state.epoch,
                                           INPUT_LOCAL, 1) == HID_ENGINE_ENQUEUE_OK);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
    sink.fail_next = true; /* Failed button-up starts release recovery. */
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    expect_zero(&engine, &sink, 1);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    assert(sink.report_id == 2 && engine.send_failures == 2);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_set_mode(&engine, INPUT_KEYBOARD));
    assert(engine.send_failures == 2); /* Mode changes cannot renew retries. */
    expect_zero(&engine, &sink, 1);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_RETRY_EXHAUSTED);
    assert(!engine.ready && engine.state.release_pending && engine.count == 0);
    assert(engine.send_failures == 3 && engine.state.epoch != epoch);
    const size_t attempts = sink.attempts;
    for (size_t i = 0; i < 100; ++i)
        assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
    assert(sink.attempts == attempts);
    assert(hid_engine_enqueue_key(&engine, engine.state.epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    hid_engine_set_link(&engine, true, true); /* Explicitly recovered transport. */
    assert(engine.ready && engine.send_failures == 0);
    release_barrier(&engine, &sink);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);

    /* Initial recovery failures retain progress and have the same finite bound. */
    hid_engine_cancel(&engine);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    assert(sink.report_id == 1);
    expect_zero(&engine, &sink, 1);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    assert(sink.report_id == 2);
    expect_zero(&engine, &sink, 2);
    assert(!engine.state.release_pending && engine.send_failures == 0);
}

static void short_backpressure(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_KEYBOARD);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "aB", 2) ==
           HID_ENGINE_ENQUEUE_OK);
    const uint8_t lower[] = {0, 0, 4, 0, 0, 0, 0, 0};
    const uint8_t upper[] = {2, 0, 5, 0, 0, 0, 0, 0};
    expect_report(&engine, &sink, 1, lower, sizeof lower);
    const size_t attempts = sink.attempts;
    assert(!hid_engine_observe_transport(&engine, 1000, true));
    assert(engine.congested && engine.ready && engine.count == 2);
    assert(input_can_emit(&engine.state, epoch, INPUT_LOCAL, INPUT_KEYBOARD));
    assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 6) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "z", 1) ==
           HID_ENGINE_ENQUEUE_BLOCKED);
    for (size_t i = 0; i < 4; ++i)
        assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
    assert(!hid_engine_observe_transport(&engine, 1249, true));
    assert(engine.state.epoch == epoch && !engine.state.release_pending);
    assert(input_can_emit(&engine.state, epoch, INPUT_LOCAL, INPUT_KEYBOARD));
    assert(engine.send_failures == 0 && sink.attempts == attempts);
    assert(engine.text_accepted == 1 && engine.text_completed == 0);
    assert(!hid_engine_observe_transport(&engine, 1249, false));
    assert(!engine.congested && engine.count == 2);
    expect_zero(&engine, &sink, 1); /* Preserve release of the accepted key-down. */
    expect_report(&engine, &sink, 1, upper, sizeof upper);
    expect_zero(&engine, &sink, 1);
    assert(engine.text_accepted == 2 && engine.text_completed == 2);
    assert(engine.state.epoch == epoch && step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
}

static void congestion_timeout(void)
{
    for (unsigned int keyboard = 0; keyboard < 2; ++keyboard) {
        hid_engine_t engine;
        sink_t sink = {0};
        connected(&engine, &sink, keyboard ? INPUT_KEYBOARD : INPUT_MOUSE);
        const uint64_t epoch = engine.state.epoch;
        if (keyboard) {
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "ab", 2) ==
                   HID_ENGINE_ENQUEUE_OK);
        } else {
            assert(hid_engine_set_gyro(&engine, true));
            assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 100, -20) ==
                   HID_ENGINE_ENQUEUE_OK);
        }
        assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT); /* A key/button is down. */
        const size_t attempts = sink.attempts;
        assert(!hid_engine_observe_transport(&engine, 0, true));
        assert(!hid_engine_observe_transport(&engine, 249, true));
        assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
        assert(engine.count == 2 && engine.state.epoch == epoch);
        assert(hid_engine_observe_transport(&engine, 250, true));
        const uint64_t cancelled_epoch = engine.state.epoch;
        assert(cancelled_epoch != epoch && engine.congested && engine.ready);
        assert(engine.count == 0 && engine.text_pending == 0);
        assert(engine.state.release_pending && !engine.state.gyro_enabled);
        assert(!hid_engine_set_gyro(&engine, true));
        assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 4) ==
               HID_ENGINE_ENQUEUE_STALE);
        assert(hid_engine_enqueue_key(&engine, cancelled_epoch, INPUT_LOCAL, 0, 4) ==
               HID_ENGINE_ENQUEUE_BLOCKED);
        assert(hid_engine_enqueue_text(&engine, cancelled_epoch, INPUT_LOCAL, "ab", 2) ==
               HID_ENGINE_ENQUEUE_BLOCKED);
        assert(hid_engine_enqueue_mouse_move(&engine, cancelled_epoch, INPUT_LOCAL, 1, 0) ==
               HID_ENGINE_ENQUEUE_BLOCKED);
        assert(hid_engine_enqueue_mouse_click(&engine, cancelled_epoch, INPUT_LOCAL, 1) ==
               HID_ENGINE_ENQUEUE_BLOCKED);
        assert(hid_engine_enqueue_mouse_scroll(&engine, cancelled_epoch, INPUT_LOCAL, 1) ==
               HID_ENGINE_ENQUEUE_BLOCKED);
        assert(!hid_engine_observe_transport(&engine, 251, true));
        assert(!hid_engine_observe_transport(&engine, 10000, true));
        assert(engine.state.epoch == cancelled_epoch && engine.count == 0);
        assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
        assert(sink.attempts == attempts && engine.send_failures == 0);
        assert(!hid_engine_observe_transport(&engine, 10001, false));
        release_barrier(&engine, &sink);
        assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE); /* No old text/movement burst. */
        assert(engine.text_accepted == (keyboard ? 1U : 0U));
        assert(engine.text_completed == 0);

        /* Clearing the episode grants a fresh 250 ms budget, not an old timer. */
        assert(hid_engine_enqueue_mouse_move(&engine, cancelled_epoch, INPUT_LOCAL, 0, 0) ==
               (keyboard ? HID_ENGINE_ENQUEUE_BLOCKED : HID_ENGINE_ENQUEUE_OK));
        assert(!hid_engine_observe_transport(&engine, 20000, true));
        assert(!hid_engine_observe_transport(&engine, 20249, true));
        assert(engine.state.epoch == cancelled_epoch);
        assert(hid_engine_observe_transport(&engine, 20250, true));
        assert(!hid_engine_observe_transport(&engine, 20500, true));
    }
}

static void congestion_across_transitions(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    assert(!hid_engine_observe_transport(&engine, 1000, true));
    assert(!hid_engine_observe_transport(&engine, 1200, true));
    assert(hid_engine_set_mode(&engine, INPUT_KEYBOARD));
    hid_engine_set_locked(&engine, true);
    hid_engine_set_link(&engine, false, false);
    hid_engine_set_link(&engine, true, true);
    const uint64_t epoch = engine.state.epoch;
    assert(engine.congested);
    assert(hid_engine_observe_transport(&engine, 1250, true));
    assert(engine.state.epoch != epoch);
    const uint64_t cancelled_epoch = engine.state.epoch;
    hid_engine_set_link(&engine, true, true);
    assert(!hid_engine_observe_transport(&engine, 1500, true));
    assert(engine.state.epoch == cancelled_epoch);
    assert(!hid_engine_observe_transport(&engine, 1501, false));
    release_barrier(&engine, &sink); /* Releases remain mandatory while locked. */

    /* A congestion pause cannot consume or renew the send-failure budget. */
    hid_engine_set_locked(&engine, false);
    release_barrier(&engine, &sink);
    assert(hid_engine_enqueue_key(&engine, engine.state.epoch, INPUT_LOCAL, 0, 4) ==
           HID_ENGINE_ENQUEUE_OK);
    sink.fail_next = true;
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
    assert(engine.send_failures == 1);
    assert(!hid_engine_observe_transport(&engine, 2000, true));
    assert(hid_engine_observe_transport(&engine, 2250, true));
    assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
    assert(engine.send_failures == 1);
    assert(!hid_engine_observe_transport(&engine, 2251, false));
    release_barrier(&engine, &sink);
    assert(engine.send_failures == 0);
}

static void congestion_expiry_on_resume(void)
{
    const uint64_t durations[] = {249, 250, 10000};
    for (size_t i = 0; i < sizeof durations / sizeof durations[0]; ++i) {
        hid_engine_t engine;
        sink_t sink = {0};
        connected(&engine, &sink, INPUT_MOUSE);
        const uint64_t epoch = engine.state.epoch;
        assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 5, 6) ==
               HID_ENGINE_ENQUEUE_OK);
        assert(!hid_engine_observe_transport(&engine, 1000, true));
        const bool expired = durations[i] >= 250;
        assert(hid_engine_observe_transport(&engine, 1000 + durations[i], false) == expired);
        assert(!engine.congested);
        if (expired) {
            assert(engine.state.epoch != epoch && engine.count == 0);
            release_barrier(&engine, &sink);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
        } else {
            assert(engine.state.epoch == epoch && engine.count == 1);
            const uint8_t move[] = {0, 5, 6, 0};
            expect_report(&engine, &sink, 2, move, sizeof move);
        }
    }
}

static void transport_clock_boundaries(void)
{
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_KEYBOARD);
    assert(!hid_engine_observe_transport(&engine, 1000, false));
    assert(hid_engine_enqueue_text(&engine, engine.state.epoch, INPUT_LOCAL, "ab", 2) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
    const uint64_t epoch = engine.state.epoch;
    assert(hid_engine_observe_transport(&engine, 999, false));
    assert(engine.state.epoch != epoch && engine.count == 0);
    assert(engine.state.release_pending && engine.text_accepted == 1);
    assert(!hid_engine_observe_transport(&engine, 999, false));
    release_barrier(&engine, &sink);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);

    assert(hid_engine_enqueue_text(&engine, engine.state.epoch, INPUT_LOCAL, "ab", 2) ==
           HID_ENGINE_ENQUEUE_OK);
    assert(!hid_engine_observe_transport(&engine, 1000, true));
    assert(hid_engine_observe_transport(&engine, 0, true)); /* Regression during pause. */
    const uint64_t cancelled_epoch = engine.state.epoch;
    assert(!hid_engine_observe_transport(&engine, 1, true));
    assert(!hid_engine_observe_transport(&engine, 1000, true));
    assert(engine.state.epoch == cancelled_epoch && engine.count == 0);
    assert(!hid_engine_observe_transport(&engine, 1001, false));
    release_barrier(&engine, &sink);

    /* Unsigned endpoints cannot wrap the elapsed-time check or replay work. */
    assert(!hid_engine_observe_transport(&engine, UINT64_MAX - 250, true));
    assert(!hid_engine_observe_transport(&engine, UINT64_MAX - 1, true));
    assert(hid_engine_observe_transport(&engine, UINT64_MAX, true));
    const uint64_t wrapped_epoch = engine.state.epoch;
    assert(!hid_engine_observe_transport(&engine, 0, false)); /* Already cancelled once. */
    assert(engine.state.epoch == wrapped_epoch && engine.state.release_pending);
    assert(!hid_engine_observe_transport(&engine, 1, false));
    release_barrier(&engine, &sink);
    assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
}

static void lifecycle_stress(void)
{
    /* Fixed storage and deterministic transitions exercise long-lived counters,
     * bounded queue use, press/release pairing and cancellation without allocation. */
    hid_engine_t engine;
    sink_t sink = {0};
    connected(&engine, &sink, INPUT_MOUSE);
    uint64_t now_ms = 0;
    for (size_t cycle = 0; cycle < 100000; ++cycle) {
        const size_t scenario = cycle % 8;
        const input_mode_t mode = scenario == 0 || scenario == 2 || scenario == 4 ?
            INPUT_MOUSE : INPUT_KEYBOARD;
        now_ms += 1000;
        assert(hid_engine_set_mode(&engine, mode));
        if (engine.state.release_pending) release_barrier(&engine, &sink);
        const uint64_t epoch = engine.state.epoch;
        switch (scenario) {
        case 0: {
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 7, -3) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_mouse_scroll(&engine, epoch, INPUT_LOCAL, 1) ==
                   HID_ENGINE_ENQUEUE_OK);
            const uint8_t move[] = {0, 7, 0xfd, 0};
            const uint8_t scroll[] = {0, 0, 0, 1};
            expect_report(&engine, &sink, 2, move, sizeof move);
            expect_report(&engine, &sink, 2, scroll, sizeof scroll);
            break;
        }
        case 1:
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "aB", 2) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            assert(hid_engine_set_mode(&engine, INPUT_AUDIO));
            assert(engine.count == 0 && engine.text_accepted == 1);
            release_barrier(&engine, &sink);
            assert(engine.text_completed == 0);
            break;
        case 2:
            assert(hid_engine_enqueue_mouse_click(&engine, epoch, INPUT_LOCAL, 1) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            hid_engine_set_locked(&engine, true);
            assert(engine.count == 0);
            release_barrier(&engine, &sink);
            assert(!hid_engine_set_owner(&engine, INPUT_REMOTE));
            hid_engine_set_locked(&engine, false);
            release_barrier(&engine, &sink);
            break;
        case 3:
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "aB", 2) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            assert(!hid_engine_observe_transport(&engine, now_ms, true));
            assert(!hid_engine_observe_transport(&engine, now_ms + 249, true));
            assert(engine.count == 2 && engine.state.epoch == epoch);
            assert(hid_engine_observe_transport(&engine, now_ms + 250, true));
            assert(engine.count == 0 && engine.text_accepted == 1);
            assert(hid_engine_enqueue_text(&engine, engine.state.epoch,
                                            INPUT_LOCAL, "aB", 2) ==
                   HID_ENGINE_ENQUEUE_BLOCKED);
            assert(!hid_engine_observe_transport(&engine, now_ms + 251, false));
            release_barrier(&engine, &sink);
            break;
        case 4:
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 7, 3) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_set_owner(&engine, INPUT_REMOTE));
            assert(engine.count == 0);
            release_barrier(&engine, &sink);
            assert(hid_engine_enqueue_mouse_click(&engine, engine.state.epoch,
                                                   INPUT_REMOTE, 1) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_set_owner(&engine, INPUT_LOCAL));
            release_barrier(&engine, &sink);
            assert(hid_engine_enqueue_mouse_move(&engine, epoch, INPUT_LOCAL, 7, 3) ==
                   HID_ENGINE_ENQUEUE_STALE);
            break;
        case 5:
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "aB", 2) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            hid_engine_set_link(&engine, false, false);
            assert(engine.count == 0 && !engine.ready);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
            hid_engine_set_link(&engine, true, true);
            release_barrier(&engine, &sink);
            assert(engine.text_accepted == 1 && engine.text_completed == 0);
            break;
        case 6:
            assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 2, 4) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(hid_engine_enqueue_key(&engine, epoch, INPUT_LOCAL, 0, 5) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            sink.fail_next = true;
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SEND_FAILED);
            assert(engine.count == 0 && engine.send_failures == 1);
            release_barrier(&engine, &sink);
            break;
        default:
            assert(hid_engine_enqueue_text(&engine, epoch, INPUT_LOCAL, "a", 1) ==
                   HID_ENGINE_ENQUEUE_OK);
            assert(step(&engine, &sink) == HID_ENGINE_STEP_SENT);
            assert(!hid_engine_observe_transport(&engine, now_ms, true));
            assert(step(&engine, &sink) == HID_ENGINE_STEP_BLOCKED);
            assert(!hid_engine_observe_transport(&engine, now_ms + 1, false));
            assert(engine.state.epoch == epoch && engine.count == 1);
            expect_zero(&engine, &sink, 1);
            assert(engine.text_accepted == 1 && engine.text_completed == 1);
            break;
        }
        assert(engine.count == 0 && engine.text_pending == 0);
        assert(engine.head < HID_ENGINE_QUEUE_CAPACITY);
        assert(engine.state.connected && engine.ready && !engine.congested);
        assert(!engine.state.release_pending && !engine.state.locked);
        assert(engine.state.owner == INPUT_LOCAL && !engine.state.gyro_enabled);
        assert(engine.send_failures == 0 && !sink.fail_next);
        assert(step(&engine, &sink) == HID_ENGINE_STEP_IDLE);
    }
}

typedef struct {
    uint32_t size;
    uint32_t count;
    uint32_t page;
    uint32_t logical_min;
    uint32_t logical_max;
    uint32_t usage_min;
    uint32_t usage_max;
    uint8_t id;
} descriptor_state_t;

static void descriptor_structure(void)
{
    /* Parse HID short items independently, checking layout rather than a byte
     * snapshot. The Map has no long items, features or implicit report zero. */
    descriptor_state_t state = {0};
    size_t input_bits[3] = {0};
    size_t output_bits[3] = {0};
    size_t offset = 0;
    unsigned int depth = 0;
    unsigned int applications = 0;
    unsigned int modifiers = 0, keys = 0, leds = 0, buttons = 0, axes = 0;
    unsigned int desktop_usages = 0;
    while (offset < hid_engine_report_descriptor_length) {
        const uint8_t prefix = hid_engine_report_descriptor[offset++];
        assert(prefix != 0xfe);
        size_t size = prefix & 3U;
        if (size == 3) size = 4;
        assert(size <= hid_engine_report_descriptor_length - offset);
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= (uint32_t)hid_engine_report_descriptor[offset++] << (8 * i);
        const uint8_t type = (prefix >> 2) & 3U;
        const uint8_t tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: state.page = value; break;
            case 1: state.logical_min = value; break;
            case 2: state.logical_max = value; break;
            case 7: state.size = value; break;
            case 8:
                assert(value == 1 || value == 2);
                state.id = (uint8_t)value;
                break;
            case 9: state.count = value; break;
            default: assert(false);
            }
        } else if (type == 2) {
            if (tag == 1) state.usage_min = value;
            else if (tag == 2) state.usage_max = value;
            else {
                assert(tag == 0);
                if (state.page == 1 && value == 0x30) desktop_usages |= 1U;
                if (state.page == 1 && value == 0x31) desktop_usages |= 2U;
                if (state.page == 1 && value == 0x38) desktop_usages |= 4U;
            }
        } else {
            assert(type == 0);
            if (tag == 10) {
                ++depth;
                if (value == 1) ++applications;
            } else if (tag == 12) {
                assert(depth > 0 && size == 0);
                --depth;
            } else {
                assert(tag == 8 || tag == 9); /* Input or Output only. */
                assert(state.id == 1 || state.id == 2);
                assert(depth > 0 && state.count > 0 && state.size > 0);
                size_t *bits = tag == 8 ? input_bits : output_bits;
                bits[state.id] += state.size * state.count;
                if ((value & 1U) == 0) {
                    if (state.id == 1 && tag == 8 && state.size == 1) {
                        assert(state.page == 7 && state.count == 8 && value == 2);
                        assert(state.usage_min == 0xe0 && state.usage_max == 0xe7);
                        assert(state.logical_min == 0 && state.logical_max == 1);
                        ++modifiers;
                    } else if (state.id == 1 && tag == 8) {
                        assert(state.page == 7 && state.count == 6 && state.size == 8);
                        assert(state.usage_min == 0 && state.usage_max == 0x65);
                        assert(state.logical_min == 0 && state.logical_max == 0x65);
                        assert(value == 0);
                        ++keys;
                    } else if (state.id == 1) {
                        assert(state.page == 8 && state.size == 1 && state.count == 5);
                        assert(state.usage_min == 1 && state.usage_max == 5 && value == 2);
                        ++leds;
                    } else if (state.size == 1) {
                        assert(tag == 8 && state.page == 9 && state.count == 3);
                        assert(state.usage_min == 1 && state.usage_max == 3 && value == 2);
                        ++buttons;
                    } else {
                        assert(tag == 8 && state.page == 1 && state.count == 3);
                        assert(state.size == 8 && state.logical_min == 0x81);
                        assert(state.logical_max == 0x7f && value == 6);
                        assert(desktop_usages == 7);
                        ++axes;
                    }
                }
            }
            state.usage_min = 0;
            state.usage_max = 0;
            desktop_usages = 0;
        }
    }
    assert(depth == 0 && applications == 2);
    assert(input_bits[0] == 0 && input_bits[1] == 64 && input_bits[2] == 32);
    assert(output_bits[0] == 0 && output_bits[1] == 8 && output_bits[2] == 0);
    assert(modifiers == 1 && keys == 1 && leds == 1 && buttons == 1 && axes == 1);
    assert(HID_ENGINE_KEYBOARD_LED_LENGTH == 1);
}

int main(void)
{
    initial_and_readiness();
    mouse_sequence();
    drag_sequence();
    drag_capacity_and_validation();
    drag_interruptions();
    drag_failures_and_short_congestion();
    interrupted_release_barrier();
    explicit_keys_and_text();
    invalid_ranges();
    queue_capacity_and_wrap();
    interruption_sequences();
    failure_sequences();
    short_backpressure();
    congestion_timeout();
    congestion_across_transitions();
    congestion_expiry_on_resume();
    transport_clock_boundaries();
    lifecycle_stress();
    descriptor_structure();
    puts("PASS: HID layout, drag/release, queue, recovery, congestion deadlines and 100000 lifecycle cycles");
    return 0;
}
