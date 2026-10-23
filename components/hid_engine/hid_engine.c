// SPDX-License-Identifier: Apache-2.0
#include "hid_engine.h"
#include <string.h>

const uint8_t hid_engine_report_descriptor[] = {
    0x05, 0x01,       /* Generic Desktop */
    0x09, 0x06,       /* Keyboard */
    0xa1, 0x01,       /* Application */
    0x85, HID_ENGINE_KEYBOARD_REPORT_ID,
    0x05, 0x07,       /* Keyboard/Keypad */
    0x19, 0xe0, 0x29, 0xe7, /* Eight modifier usages */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /* Reserved byte */
    0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, /* Five LED usages */
    0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, /* LED padding */
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, HID_ENGINE_KEY_USAGE_MAX,
    0x05, 0x07, 0x19, 0x00, 0x29, HID_ENGINE_KEY_USAGE_MAX,
    0x81, 0x00,       /* Six-key array */
    0xc0,
    0x05, 0x01, 0x09, 0x02, /* Generic Desktop, Mouse */
    0xa1, 0x01,
    0x85, HID_ENGINE_MOUSE_REPORT_ID,
    0x09, 0x01, 0xa1, 0x00, /* Pointer, Physical */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, /* Three buttons */
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01, /* Button padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, /* X/Y/Wheel */
    0x15, 0x81, 0x25, 0x7f, /* Signed -127..127 */
    0x75, 0x08, 0x95, 0x03, 0x81, 0x06, /* Relative axes */
    0xc0, 0xc0
};
const size_t hid_engine_report_descriptor_length =
    sizeof hid_engine_report_descriptor;

_Static_assert(sizeof(input_key_report_t) == HID_ENGINE_KEYBOARD_REPORT_LENGTH,
               "Keyboard payload must contain exactly eight bytes");

static void clear_queue(hid_engine_t *engine)
{
    memset(engine->queue, 0, sizeof engine->queue);
    engine->head = 0;
    engine->count = 0;
    engine->text_pending = 0;
    engine->release_stage = 0;
    engine->mouse_buttons = 0;
    engine->buffered_motion_count = 0;
    engine->motion_since_ms = 0;
}

void hid_engine_init(hid_engine_t *engine)
{
    *engine = (hid_engine_t){0};
    input_init(&engine->state);
}

void hid_engine_cancel(hid_engine_t *engine)
{
    input_invalidate(&engine->state);
    clear_queue(engine);
}

void hid_engine_set_link(hid_engine_t *engine, bool connected, bool ready)
{
    ready = connected && ready;
    const uint64_t epoch = engine->state.epoch;
    input_set_connected(&engine->state, connected);
    if (engine->state.epoch != epoch) {
        clear_queue(engine);
    } else if (engine->ready != ready) {
        hid_engine_cancel(engine);
    }
    if (engine->ready != ready) engine->send_failures = 0;
    engine->ready = ready;
}

bool hid_engine_observe_transport(hid_engine_t *engine, uint64_t now_ms,
                                  bool congested)
{
    const bool regressed = engine->time_observed && now_ms < engine->observed_ms;
    engine->time_observed = true;
    engine->observed_ms = now_ms;
    bool invalidate = regressed;
    if (engine->congested) {
        /* Check the closing observation too: resume must not bypass expiry.
         * An already cancelled episode cannot admit work or need another reset. */
        invalidate = !engine->congestion_cancelled &&
            (regressed || now_ms - engine->congestion_since_ms >=
                          HID_ENGINE_CONGESTION_TIMEOUT_MS);
    } else if (congested) {
        engine->congestion_since_ms = now_ms;
        engine->congestion_cancelled = false;
    }
    if (engine->buffered_motion_count &&
        (now_ms < engine->motion_since_ms || now_ms - engine->motion_since_ms >=
         HID_ENGINE_CONGESTION_TIMEOUT_MS)) invalidate = true;
    if (invalidate) {
        hid_engine_cancel(engine);
        engine->congestion_cancelled = true;
    }
    engine->congested = congested;
    if (!congested) {
        engine->congestion_cancelled = false;
        engine->congestion_since_ms = 0;
    }
    return invalidate;
}

bool hid_engine_set_mode(hid_engine_t *engine, input_mode_t mode)
{
    const uint64_t epoch = engine->state.epoch;
    const bool accepted = input_set_mode(&engine->state, mode);
    if (engine->state.epoch != epoch) clear_queue(engine);
    return accepted;
}

void hid_engine_set_locked(hid_engine_t *engine, bool locked)
{
    const uint64_t epoch = engine->state.epoch;
    input_set_locked(&engine->state, locked);
    if (engine->state.epoch != epoch) clear_queue(engine);
}

bool hid_engine_set_owner(hid_engine_t *engine, input_owner_t owner)
{
    if (owner != INPUT_LOCAL && owner != INPUT_REMOTE) return false;
    if (engine->state.locked && owner != INPUT_LOCAL) return false;
    const uint64_t epoch = engine->state.epoch;
    input_set_owner(&engine->state, owner);
    if (engine->state.epoch != epoch) clear_queue(engine);
    return true;
}

bool hid_engine_set_gyro(hid_engine_t *engine, bool enabled)
{
    return hid_engine_set_gyro_owned(engine, INPUT_LOCAL, enabled);
}

bool hid_engine_set_gyro_owned(hid_engine_t *engine, input_owner_t source, bool enabled)
{
    if (enabled && (!engine->ready || engine->congested)) return false;
    return input_set_gyro_owned(&engine->state, source, enabled);
}

static hid_engine_enqueue_result_t check_enqueue(const hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, input_mode_t mode, size_t needed,
    bool permit_congested_release)
{
    if (owner != INPUT_LOCAL && owner != INPUT_REMOTE)
        return HID_ENGINE_ENQUEUE_INVALID;
    if (epoch != engine->state.epoch || owner != engine->state.owner)
        return HID_ENGINE_ENQUEUE_STALE;
    if (!engine->ready || (engine->congested && !permit_congested_release) ||
        !input_can_emit(&engine->state, epoch, owner, mode))
        return HID_ENGINE_ENQUEUE_BLOCKED;
    if (needed > HID_ENGINE_QUEUE_CAPACITY - engine->count)
        return HID_ENGINE_ENQUEUE_FULL;
    return HID_ENGINE_ENQUEUE_OK;
}

static void push(hid_engine_t *engine, const hid_engine_action_t *action)
{
    const size_t tail = (engine->head + engine->count) % HID_ENGINE_QUEUE_CAPACITY;
    engine->queue[tail] = *action;
    ++engine->count;
}

static void refresh_motion_age(hid_engine_t *engine)
{
    engine->buffered_motion_count = 0;
    engine->motion_since_ms = 0;
    for (size_t i = 0; i < engine->count; ++i) {
        const hid_engine_action_t *action =
            &engine->queue[(engine->head + i) % HID_ENGINE_QUEUE_CAPACITY];
        if (!action->buffered_motion) continue;
        if (!engine->buffered_motion_count || action->motion_since_ms < engine->motion_since_ms)
            engine->motion_since_ms = action->motion_since_ms;
        ++engine->buffered_motion_count;
    }
}

static uint8_t queued_mouse_buttons(const hid_engine_t *engine)
{
    uint8_t buttons = engine->mouse_buttons;
    for (size_t i = 0; i < engine->count; ++i) {
        const hid_engine_action_t *action =
            &engine->queue[(engine->head + i) % HID_ENGINE_QUEUE_CAPACITY];
        if (action->sets_mouse_buttons) buttons = action->data[0];
    }
    return buttons;
}

static bool drag_active_or_queued(const hid_engine_t *engine)
{
    if (engine->mouse_buttons != 0) return true;
    for (size_t i = 0; i < engine->count; ++i) {
        const hid_engine_action_t *action =
            &engine->queue[(engine->head + i) % HID_ENGINE_QUEUE_CAPACITY];
        if (action->sets_mouse_buttons && action->data[0] != 0) return true;
    }
    return false;
}

static hid_engine_enqueue_result_t enqueue_mouse(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, uint8_t buttons, int x, int y, int wheel,
    bool sets_buttons, bool click)
{
    const bool reserve_release = sets_buttons ? buttons != 0 :
        !click && queued_mouse_buttons(engine) != 0;
    const hid_engine_enqueue_result_t result =
        check_enqueue(engine, epoch, owner, INPUT_MOUSE, reserve_release ? 2 : 1,
                      sets_buttons && buttons == 0);
    if (result != HID_ENGINE_ENQUEUE_OK) return result;
    if (click && drag_active_or_queued(engine)) return HID_ENGINE_ENQUEUE_BLOCKED;
    const hid_engine_action_t action = {
        .epoch = epoch, .owner = owner, .mode = INPUT_MOUSE,
        .data = {buttons, (uint8_t)x, (uint8_t)y, (uint8_t)wheel},
        .report_id = HID_ENGINE_MOUSE_REPORT_ID,
        .length = HID_ENGINE_MOUSE_REPORT_LENGTH,
        .needs_release = click,
        .sets_mouse_buttons = sets_buttons
    };
    push(engine, &action);
    return HID_ENGINE_ENQUEUE_OK;
}

hid_engine_enqueue_result_t hid_engine_enqueue_mouse_move(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int x, int y)
{
    if (x < -127 || x > 127 || y < -127 || y > 127)
        return HID_ENGINE_ENQUEUE_INVALID;
    return enqueue_mouse(engine, epoch, owner, 0, x, y, 0, false, false);
}

hid_engine_enqueue_result_t hid_engine_enqueue_mouse_displacement(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int x, int y, uint64_t sample_ms, uint64_t now_ms)
{
    if (x < -HID_ENGINE_MOUSE_DISPLACEMENT_MAX || x > HID_ENGINE_MOUSE_DISPLACEMENT_MAX ||
        y < -HID_ENGINE_MOUSE_DISPLACEMENT_MAX || y > HID_ENGINE_MOUSE_DISPLACEMENT_MAX)
        return HID_ENGINE_ENQUEUE_INVALID;
    hid_engine_action_t *tail = engine->count ?
        &engine->queue[(engine->head + engine->count - 1) % HID_ENGINE_QUEUE_CAPACITY] : NULL;
    const bool merge = owner == INPUT_LOCAL && tail && tail->buffered_motion &&
        tail->epoch == epoch && tail->owner == owner;
    const bool held = queued_mouse_buttons(engine) != 0;
    const hid_engine_enqueue_result_t result = check_enqueue(engine, epoch, owner,
        INPUT_MOUSE, (merge ? 0U : 1U) + (held ? 1U : 0U), owner == INPUT_LOCAL);
    if (result != HID_ENGINE_ENQUEUE_OK) return result;
    if (engine->congestion_cancelled || (engine->time_observed && now_ms < engine->observed_ms))
        return HID_ENGINE_ENQUEUE_BLOCKED;
    if (sample_ms > now_ms || now_ms - sample_ms >= HID_ENGINE_CONGESTION_TIMEOUT_MS)
        return HID_ENGINE_ENQUEUE_STALE;
    const int total_x = x + (merge ? tail->remaining_x : 0);
    const int total_y = y + (merge ? tail->remaining_y : 0);
    if (total_x < -HID_ENGINE_MOUSE_DISPLACEMENT_MAX || total_x > HID_ENGINE_MOUSE_DISPLACEMENT_MAX ||
        total_y < -HID_ENGINE_MOUSE_DISPLACEMENT_MAX || total_y > HID_ENGINE_MOUSE_DISPLACEMENT_MAX)
        return HID_ENGINE_ENQUEUE_FULL;
    if (merge) {
        tail->remaining_x = (int16_t)total_x;
        tail->remaining_y = (int16_t)total_y;
        if (sample_ms < tail->motion_since_ms) tail->motion_since_ms = sample_ms;
        if (!total_x && !total_y) {
            *tail = (hid_engine_action_t){0};
            --engine->count;
        }
    } else {
        if (!x && !y) return HID_ENGINE_ENQUEUE_OK;
        const hid_engine_action_t action = {
            .epoch = epoch, .owner = owner, .mode = INPUT_MOUSE,
            .report_id = HID_ENGINE_MOUSE_REPORT_ID,
            .length = HID_ENGINE_MOUSE_REPORT_LENGTH,
            .buffered_motion = true, .remaining_x = (int16_t)x, .remaining_y = (int16_t)y,
            .motion_since_ms = sample_ms
        };
        push(engine, &action);
    }
    refresh_motion_age(engine);
    return HID_ENGINE_ENQUEUE_OK;
}

hid_engine_enqueue_result_t hid_engine_enqueue_mouse_click(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int buttons)
{
    if (buttons == 0 || buttons > HID_ENGINE_MOUSE_BUTTON_MASK)
        return HID_ENGINE_ENQUEUE_INVALID;
    return enqueue_mouse(engine, epoch, owner, (uint8_t)buttons, 0, 0, 0, false, true);
}

hid_engine_enqueue_result_t hid_engine_enqueue_mouse_buttons(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int buttons)
{
    if (buttons > 1) return HID_ENGINE_ENQUEUE_INVALID;
    return enqueue_mouse(engine, epoch, owner, (uint8_t)buttons, 0, 0, 0, true, false);
}

hid_engine_enqueue_result_t hid_engine_enqueue_mouse_scroll(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int wheel)
{
    if (wheel < -127 || wheel > 127) return HID_ENGINE_ENQUEUE_INVALID;
    return enqueue_mouse(engine, epoch, owner, 0, 0, 0, wheel, false, false);
}

static hid_engine_action_t key_action(uint64_t epoch, input_owner_t owner,
                                      const input_key_report_t *report)
{
    hid_engine_action_t action = {
        .epoch = epoch, .owner = owner, .mode = INPUT_KEYBOARD,
        .report_id = HID_ENGINE_KEYBOARD_REPORT_ID,
        .length = HID_ENGINE_KEYBOARD_REPORT_LENGTH,
        .needs_release = true
    };
    memcpy(action.data, report, sizeof *report);
    return action;
}

hid_engine_enqueue_result_t hid_engine_enqueue_key(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int modifier, unsigned int usage)
{
    if (modifier > 255 || usage > HID_ENGINE_KEY_USAGE_MAX ||
        (usage > 0 && usage < 4) || (usage == 0 && modifier == 0))
        return HID_ENGINE_ENQUEUE_INVALID;
    const hid_engine_enqueue_result_t result =
        check_enqueue(engine, epoch, owner, INPUT_KEYBOARD, 1, false);
    if (result != HID_ENGINE_ENQUEUE_OK) return result;
    const input_key_report_t report = {
        .modifier = (uint8_t)modifier, .keys = {(uint8_t)usage}
    };
    const hid_engine_action_t action = key_action(epoch, owner, &report);
    push(engine, &action);
    return HID_ENGINE_ENQUEUE_OK;
}

hid_engine_enqueue_result_t hid_engine_enqueue_text(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, const char *text, size_t length)
{
    if (!text || length == 0 || length > HID_ENGINE_TEXT_MAX_LENGTH)
        return HID_ENGINE_ENQUEUE_INVALID;
    input_key_report_t report;
    for (size_t i = 0; i < length; ++i) {
        if (!input_ascii_report((unsigned char)text[i], &report))
            return HID_ENGINE_ENQUEUE_INVALID;
    }
    const hid_engine_enqueue_result_t result =
        check_enqueue(engine, epoch, owner, INPUT_KEYBOARD, length, false);
    if (result != HID_ENGINE_ENQUEUE_OK) return result;
    if (engine->text_pending != 0) return HID_ENGINE_ENQUEUE_BLOCKED;
    for (size_t i = 0; i < length; ++i) {
        (void)input_ascii_report((unsigned char)text[i], &report);
        hid_engine_action_t action = key_action(epoch, owner, &report);
        action.text_character = true;
        push(engine, &action);
    }
    engine->text_accepted = 0;
    engine->text_completed = 0;
    engine->text_pending = length;
    return HID_ENGINE_ENQUEUE_OK;
}

static hid_engine_step_result_t send_failed(hid_engine_t *engine)
{
    if (!engine->state.release_pending) hid_engine_cancel(engine);
    ++engine->send_failures;
    if (engine->send_failures >= HID_ENGINE_MAX_SEND_FAILURES) {
        engine->ready = false;
        return HID_ENGINE_STEP_RETRY_EXHAUSTED;
    }
    return HID_ENGINE_STEP_SEND_FAILED;
}

hid_engine_step_result_t hid_engine_step(hid_engine_t *engine,
    hid_engine_send_fn send, void *context)
{
    if (!engine->state.connected || !engine->ready || engine->congested || !send)
        return HID_ENGINE_STEP_BLOCKED;
    static const uint8_t zero[HID_ENGINE_KEYBOARD_REPORT_LENGTH] = {0};
    if (engine->state.release_pending) {
        const uint8_t report_id = engine->release_stage == 0 ?
            HID_ENGINE_KEYBOARD_REPORT_ID : HID_ENGINE_MOUSE_REPORT_ID;
        const size_t length = engine->release_stage == 0 ?
            HID_ENGINE_KEYBOARD_REPORT_LENGTH : HID_ENGINE_MOUSE_REPORT_LENGTH;
        if (!send(context, report_id, zero, length)) return send_failed(engine);
        if (engine->release_stage == 0) {
            engine->release_stage = 1;
        } else {
            (void)input_ack_release(&engine->state, engine->state.epoch);
            engine->release_stage = 0;
            engine->send_failures = 0;
        }
        return HID_ENGINE_STEP_SENT;
    }
    if (engine->count == 0) return HID_ENGINE_STEP_IDLE;
    hid_engine_action_t *action = &engine->queue[engine->head];
    if (!input_can_emit(&engine->state, action->epoch, action->owner,
                        action->mode)) {
        hid_engine_cancel(engine);
        return HID_ENGINE_STEP_BLOCKED;
    }
    if (action->report_id == HID_ENGINE_MOUSE_REPORT_ID && action->needs_release &&
        engine->mouse_buttons != 0) {
        hid_engine_cancel(engine);
        return HID_ENGINE_STEP_BLOCKED;
    }
    uint8_t mouse[HID_ENGINE_MOUSE_REPORT_LENGTH];
    const uint8_t *data = action->release_next ? zero : action->data;
    if (action->report_id == HID_ENGINE_MOUSE_REPORT_ID &&
        !action->needs_release && !action->sets_mouse_buttons) {
        memcpy(mouse, action->data, sizeof mouse);
        mouse[0] = engine->mouse_buttons;
        if (action->buffered_motion) {
            const int x = action->remaining_x, y = action->remaining_y;
            mouse[1] = (uint8_t)(x > 127 ? 127 : x < -127 ? -127 : x);
            mouse[2] = (uint8_t)(y > 127 ? 127 : y < -127 ? -127 : y);
        }
        data = mouse;
    }
    if (!send(context, action->report_id, data, action->length))
        return send_failed(engine);
    if (action->buffered_motion) {
        action->remaining_x = (int16_t)(action->remaining_x - (int8_t)mouse[1]);
        action->remaining_y = (int16_t)(action->remaining_y - (int8_t)mouse[2]);
        if (action->remaining_x || action->remaining_y) return HID_ENGINE_STEP_SENT;
    }
    if (action->sets_mouse_buttons) engine->mouse_buttons = action->data[0];
    if (action->text_character) {
        if (action->release_next) {
            ++engine->text_completed;
            --engine->text_pending;
        } else {
            ++engine->text_accepted;
        }
    }
    if (action->needs_release && !action->release_next) {
        action->release_next = true;
    } else {
        *action = (hid_engine_action_t){0};
        engine->head = (engine->head + 1) % HID_ENGINE_QUEUE_CAPACITY;
        --engine->count;
        refresh_motion_age(engine);
    }
    return HID_ENGINE_STEP_SENT;
}
