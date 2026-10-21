// SPDX-License-Identifier: Apache-2.0
#include "haptics.h"
#include "haptics_dispatch.h"

#include <stddef.h>
#include <string.h>

enum { COOLDOWN_MS = 200, STOP_RETRY_MS = 20 };

static uint64_t saturating_add(uint64_t value, uint64_t increment)
{
    return value > UINT64_MAX - increment ? UINT64_MAX : value + increment;
}

static void preserve_off_interval(haptics_t *state, uint64_t now_ms)
{
    const uint64_t cooldown = saturating_add(now_ms, COOLDOWN_MS);
    if (cooldown > state->next_allowed_ms) {
        state->next_allowed_ms = cooldown;
    }
}

static uint32_t duration_ms(haptics_event_t event)
{
    switch (event) {
    case HAPTICS_CONFIRM: return 24;
    case HAPTICS_MODE: return 45;
    case HAPTICS_LOCK: return 65;
    case HAPTICS_ERROR: return 165;
    }
    return 0;
}

static uint8_t pattern_strength(haptics_event_t event, uint64_t elapsed_ms)
{
    switch (event) {
    case HAPTICS_CONFIRM: return elapsed_ms < 24 ? 35 : 0;
    case HAPTICS_MODE: return elapsed_ms < 45 ? 55 : 0;
    case HAPTICS_LOCK: return elapsed_ms < 65 ? 65 : 0;
    case HAPTICS_ERROR:
        return elapsed_ms < 40 || (elapsed_ms >= 105 && elapsed_ms < 165) ? 65 : 0;
    }
    return 0;
}

static bool stop_output(haptics_t *state, uint64_t now_ms)
{
    bool stopped = state->write && state->write(state->context, 0) == HAPTICS_WRITE_OK;
    state->off_pending = !stopped;
    state->last_stop_ms = now_ms;
    if (stopped) {
        state->strength = 0;
        state->last_write_ms = now_ms;
    }
    return stopped;
}

static haptics_result_t fail(haptics_t *state, haptics_result_t reason,
    uint64_t now_ms)
{
    state->available = false;
    state->enabled = false;
    state->playing = false;
    state->fault = reason;
    state->last_ms = now_ms;
    (void)stop_output(state, now_ms);
    return reason;
}

static haptics_result_t cancel_write(haptics_t *state, uint64_t now_ms)
{
    state->enabled = false;
    state->playing = false;
    preserve_off_interval(state, now_ms);
    if ((state->strength || state->off_pending) && !stop_output(state, now_ms)) {
        return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
    }
    return HAPTICS_CANCELLED;
}

static haptics_result_t advance(haptics_t *state, uint64_t now_ms)
{
    if (!state->supported) {
        return HAPTICS_UNAVAILABLE;
    }
    if (now_ms < state->last_ms) {
        return fail(state, HAPTICS_TIME_ERROR, now_ms);
    }
    state->last_ms = now_ms;
    if (!state->available) {
        if (state->off_pending && now_ms >= state->last_stop_ms &&
                now_ms - state->last_stop_ms >= STOP_RETRY_MS) {
            (void)stop_output(state, now_ms);
        }
        return state->fault;
    }
    if (state->playing) {
        const uint64_t elapsed = now_ms - state->started_ms;
        uint8_t desired = pattern_strength(state->event, elapsed);
        if (state->event == HAPTICS_ERROR && elapsed >= 105 && elapsed < 165) {
            if (!state->error_gap_seen) {
                /* If scheduling skipped the off phase, stop now rather than
                 * merge two pulses into one long continuous vibration. */
                desired = 0;
                state->playing = false;
            } else if (!state->strength && now_ms - state->last_write_ms < 65) {
                /* A late first stop must not compress the physical off gap. */
                desired = 0;
            }
        }
        if (desired != state->strength) {
            const haptics_write_result_t written = state->write(state->context, desired);
            if (desired && written == HAPTICS_WRITE_CANCELLED) {
                return cancel_write(state, now_ms);
            }
            if (written != HAPTICS_WRITE_OK) {
                return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
            }
            state->strength = desired;
            state->last_write_ms = now_ms;
            if (!desired && state->event == HAPTICS_ERROR && elapsed < 105) {
                state->error_gap_seen = true;
            }
            if (!desired && elapsed >= duration_ms(state->event)) {
                /* Preserve a real off interval after a late worker wakeup. */
                preserve_off_interval(state, now_ms);
            }
        }
        if (elapsed >= duration_ms(state->event)) {
            state->playing = false;
        }
    }
    return HAPTICS_OK;
}

haptics_result_t haptics_init(haptics_t *state, bool supported,
    haptics_write_t write, void *context, uint64_t now_ms)
{
    if (!state) {
        return HAPTICS_UNAVAILABLE;
    }
    memset(state, 0, sizeof(*state));
    state->write = write;
    state->context = context;
    state->supported = supported && write != NULL;
    state->last_ms = now_ms;
    state->next_allowed_ms = now_ms;
    state->fault = HAPTICS_UNAVAILABLE;
    if (!state->supported) {
        return HAPTICS_UNAVAILABLE;
    }
    if (!stop_output(state, now_ms)) {
        return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
    }
    state->available = true;
    state->enabled = true;
    state->fault = HAPTICS_OK;
    return HAPTICS_OK;
}

haptics_result_t haptics_request(haptics_t *state, haptics_event_t event,
    uint64_t now_ms)
{
    if (!state) {
        return HAPTICS_UNAVAILABLE;
    }
    const haptics_result_t result = advance(state, now_ms);
    if (result != HAPTICS_OK) {
        return result;
    }
    const uint32_t duration = duration_ms(event);
    if (!duration) {
        return HAPTICS_INVALID_EVENT;
    }
    if (!state->enabled) {
        return HAPTICS_DISABLED;
    }
    if (state->playing || now_ms < state->next_allowed_ms) {
        return HAPTICS_SUPPRESSED;
    }
    /* Refuse a pulse whose complete pattern/cooldown cannot be represented. */
    if (now_ms > UINT64_MAX - duration - COOLDOWN_MS) {
        return fail(state, HAPTICS_TIME_ERROR, now_ms);
    }
    state->event = event;
    state->started_ms = now_ms;
    state->next_allowed_ms = now_ms + duration + COOLDOWN_MS;
    state->playing = true;
    state->error_gap_seen = false;
    const uint8_t strength = pattern_strength(event, 0);
    const haptics_write_result_t written = state->write(state->context, strength);
    if (written == HAPTICS_WRITE_CANCELLED) {
        return cancel_write(state, now_ms);
    }
    if (written != HAPTICS_WRITE_OK) {
        return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
    }
    state->strength = strength;
    state->last_write_ms = now_ms;
    return HAPTICS_OK;
}

haptics_result_t haptics_poll(haptics_t *state, uint64_t now_ms)
{
    return state ? advance(state, now_ms) : HAPTICS_UNAVAILABLE;
}

haptics_result_t haptics_set_enabled(haptics_t *state, bool enabled,
    uint64_t now_ms)
{
    if (!state) {
        return HAPTICS_UNAVAILABLE;
    }
    /* Cancellation must not start the next phase of a partly elapsed error. */
    if (!enabled) {
        const bool cancelling = state->enabled || state->playing;
        state->enabled = false;
        state->playing = false;
        if (cancelling && state->supported && (state->strength || state->off_pending)) {
            if (!stop_output(state, now_ms)) {
                return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
            }
            preserve_off_interval(state, now_ms);
        }
    }
    const haptics_result_t result = advance(state, now_ms);
    if (result == HAPTICS_OK) {
        state->enabled = enabled;
    }
    return result;
}

haptics_result_t haptics_recover(haptics_t *state, uint64_t now_ms)
{
    if (!state || !state->supported) {
        return HAPTICS_UNAVAILABLE;
    }
    state->playing = false;
    state->enabled = false;
    state->last_ms = now_ms;
    state->next_allowed_ms = now_ms;
    if (!stop_output(state, now_ms)) {
        return fail(state, HAPTICS_HARDWARE_ERROR, now_ms);
    }
    state->available = true;
    state->fault = HAPTICS_OK;
    return HAPTICS_OK;
}

bool haptics_available(const haptics_t *state)
{
    return state && state->available;
}

bool haptics_enabled(const haptics_t *state)
{
    return haptics_available(state) && state->enabled;
}

bool haptics_active(const haptics_t *state)
{
    return state && (state->strength != 0 || state->off_pending);
}

bool haptics_dispatch_init(haptics_dispatch_t *dispatch,
    haptics_dispatch_lock_t enter, haptics_dispatch_lock_t leave,
    void *lock_context, haptics_write_t write, void *write_context)
{
    if (!dispatch) {
        return false;
    }
    memset(dispatch, 0, sizeof(*dispatch));
    if (!enter || !leave || !write) {
        return false;
    }
    dispatch->enter = enter;
    dispatch->leave = leave;
    dispatch->lock_context = lock_context;
    dispatch->write = write;
    dispatch->write_context = write_context;
    return true;
}

bool haptics_dispatch_enable_locked(haptics_dispatch_t *dispatch, bool enabled)
{
    if (!dispatch || dispatch->exhausted || dispatch->enabled == enabled) {
        return false;
    }
    if (dispatch->generation == UINT64_MAX) {
        dispatch->enabled = false;
        dispatch->exhausted = true;
    } else {
        ++dispatch->generation;
        dispatch->enabled = enabled;
    }
    return true;
}

haptics_write_result_t haptics_dispatch_write(haptics_dispatch_t *dispatch,
    uint64_t generation, uint8_t strength_percent)
{
    if (!dispatch || !dispatch->enter || !dispatch->leave || !dispatch->write ||
            strength_percent > 100) {
        return HAPTICS_WRITE_FAILED;
    }
    dispatch->enter(dispatch->lock_context);
    haptics_write_result_t result;
    if (strength_percent && (!dispatch->enabled || dispatch->exhausted ||
            generation != dispatch->generation)) {
        result = HAPTICS_WRITE_CANCELLED;
    } else {
        result = dispatch->write(dispatch->write_context, strength_percent);
    }
    dispatch->leave(dispatch->lock_context);
    return result;
}
