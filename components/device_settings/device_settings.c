// SPDX-License-Identifier: Apache-2.0
#include "device_settings.h"
#include <string.h>

bool device_idle_timeout_ms(device_idle_timeout_t timeout, uint64_t *milliseconds)
{
    if (!milliseconds) return false;
    switch (timeout) {
    case DEVICE_IDLE_OFF: *milliseconds = 0; return true;
    case DEVICE_IDLE_1_MIN: *milliseconds = 60000; return true;
    case DEVICE_IDLE_5_MIN: *milliseconds = 300000; return true;
    case DEVICE_IDLE_10_MIN: *milliseconds = 600000; return true;
    case DEVICE_IDLE_30_MIN: *milliseconds = 1800000; return true;
    default: return false;
    }
}

void device_settings_defaults(device_settings_t *settings)
{
    *settings = (device_settings_t){
        .theme = DEVICE_THEME_DARK, .host_modifier = DEVICE_HOST_CTRL,
        .haptics_enabled = true, .brightness_percent = 70,
        .idle_timeout = DEVICE_IDLE_5_MIN, .preferred_transport = DEVICE_TRANSPORT_BLUETOOTH
    };
}

bool device_settings_validate(const device_settings_t *settings)
{
    uint64_t duration;
    return settings &&
        (settings->theme == DEVICE_THEME_LIGHT || settings->theme == DEVICE_THEME_DARK) &&
        (settings->host_modifier == DEVICE_HOST_CTRL || settings->host_modifier == DEVICE_HOST_COMMAND) &&
        (settings->preferred_transport == DEVICE_TRANSPORT_BLUETOOTH ||
         settings->preferred_transport == DEVICE_TRANSPORT_USB) &&
        settings->brightness_percent >= DEVICE_SETTINGS_MIN_BRIGHTNESS &&
        settings->brightness_percent <= DEVICE_SETTINGS_MAX_BRIGHTNESS &&
        settings->brightness_percent % DEVICE_SETTINGS_BRIGHTNESS_STEP == 0 &&
        device_idle_timeout_ms(settings->idle_timeout, &duration);
}

bool device_settings_equal(const device_settings_t *a, const device_settings_t *b)
{
    return device_settings_validate(a) && device_settings_validate(b) &&
        a->theme == b->theme && a->host_modifier == b->host_modifier &&
        a->haptics_enabled == b->haptics_enabled &&
        a->brightness_percent == b->brightness_percent && a->idle_timeout == b->idle_timeout &&
        a->preferred_transport == b->preferred_transport;
}

device_settings_apply_t device_settings_apply(device_settings_t *current,
                                              const device_settings_t *candidate)
{
    if (!current || !device_settings_validate(candidate)) return DEVICE_SETTINGS_APPLY_INVALID;
    if (device_settings_equal(current, candidate)) return DEVICE_SETTINGS_APPLY_UNCHANGED;
    *current = *candidate;
    return DEVICE_SETTINGS_APPLY_CHANGED;
}

static uint32_t checksum(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

device_settings_status_t device_settings_encode(const device_settings_t *settings,
                                                uint8_t *record, size_t length)
{
    if (!settings || !record) return DEVICE_SETTINGS_INVALID_ARGUMENT;
    if (length != DEVICE_SETTINGS_RECORD_SIZE) return DEVICE_SETTINGS_INVALID_LENGTH;
    if (!device_settings_validate(settings)) return DEVICE_SETTINGS_INVALID_VALUE;
    uint8_t encoded[DEVICE_SETTINGS_RECORD_SIZE] = {
        'D', 'S', 'E', 'T', DEVICE_SETTINGS_SCHEMA_VERSION, DEVICE_SETTINGS_RECORD_SIZE,
        (uint8_t)settings->theme, (uint8_t)settings->host_modifier,
        settings->haptics_enabled ? 1U : 0U, settings->brightness_percent,
        (uint8_t)settings->idle_timeout, (uint8_t)settings->preferred_transport, 0, 0, 0, 0
    };
    const uint32_t crc = checksum(encoded, 12);
    for (unsigned int i = 0; i < 4; ++i) encoded[12 + i] = (uint8_t)(crc >> (8U * i));
    memcpy(record, encoded, sizeof encoded);
    return DEVICE_SETTINGS_OK;
}

device_settings_status_t device_settings_decode(const uint8_t *record, size_t length,
                                                device_settings_t *settings)
{
    if (!record || !settings) return DEVICE_SETTINGS_INVALID_ARGUMENT;
    if (length != DEVICE_SETTINGS_RECORD_SIZE) return DEVICE_SETTINGS_INVALID_LENGTH;
    if (memcmp(record, "DSET", 4) != 0) return DEVICE_SETTINGS_INVALID_MAGIC;
    if (record[4] != 1 && record[4] != DEVICE_SETTINGS_SCHEMA_VERSION)
        return DEVICE_SETTINGS_UNSUPPORTED_VERSION;
    if (record[5] != DEVICE_SETTINGS_RECORD_SIZE) return DEVICE_SETTINGS_INVALID_LENGTH;
    uint32_t stored = 0;
    for (unsigned int i = 0; i < 4; ++i) stored |= (uint32_t)record[12 + i] << (8U * i);
    if (stored != checksum(record, 12)) return DEVICE_SETTINGS_INVALID_CHECKSUM;
    if (record[8] > 1 || (record[4] == 1 && record[11] != 0)) return DEVICE_SETTINGS_INVALID_VALUE;
    const device_settings_t decoded = {
        .theme = (device_theme_t)record[6],
        .host_modifier = (device_host_modifier_t)record[7],
        .haptics_enabled = record[8] != 0,
        .brightness_percent = record[9],
        .idle_timeout = (device_idle_timeout_t)record[10],
        .preferred_transport = record[4] == 1 ? DEVICE_TRANSPORT_BLUETOOTH :
            (device_transport_t)record[11]
    };
    if (!device_settings_validate(&decoded)) return DEVICE_SETTINGS_INVALID_VALUE;
    *settings = decoded;
    return DEVICE_SETTINGS_OK;
}

static bool any_down(const device_idle_t *p)
{
    return p->down[DEVICE_INPUT_TOUCH] || p->down[DEVICE_INPUT_AI];
}

static bool captured_down(const device_idle_t *p)
{
    for (size_t source = 0; source < DEVICE_INPUT_SOURCE_COUNT; ++source)
        if (p->down[source] && p->latest_token[source] <= p->consumed_through[source]) return true;
    return false;
}

static void consume_history(device_idle_t *p)
{
    for (size_t source = 0; source < DEVICE_INPUT_SOURCE_COUNT; ++source)
        p->consumed_through[source] = p->latest_token[source];
}

static void refresh_capture(device_idle_t *p)
{
    if (p->ai_wake_waiting && p->now_ms >= p->ai_release_ms &&
        p->now_ms - p->ai_release_ms >= DEVICE_IDLE_AI_CLICK_WINDOW_MS)
        p->ai_wake_waiting = false;
    p->wake_capture = captured_down(p) || p->ai_wake_waiting;
}

static void observe_clock(device_idle_t *p, uint64_t now_ms)
{
    p->clock_regressed = now_ms < p->now_ms;
    p->now_ms = now_ms;
    if (p->clock_regressed) {
        p->asleep = false;
        p->last_activity_ms = now_ms;
        consume_history(p);
        p->ai_wake_waiting = false;
    }
    refresh_capture(p);
}

void device_idle_init(device_idle_t *p, uint64_t now_ms)
{
    *p = (device_idle_t){
        .timeout = DEVICE_IDLE_5_MIN, .now_ms = now_ms, .last_activity_ms = now_ms
    };
}

bool device_idle_activity(device_idle_t *p, uint64_t now_ms)
{
    const bool was_asleep = p->asleep;
    observe_clock(p, now_ms);
    p->asleep = false;
    p->last_activity_ms = now_ms;
    return was_asleep;
}

void device_idle_note_physical_activity(device_idle_t *p, uint64_t activity_ms)
{
    if (!p->asleep && activity_ms > p->last_activity_ms) p->last_activity_ms = activity_ms;
}

bool device_idle_configure(device_idle_t *p, device_idle_timeout_t timeout, uint64_t now_ms)
{
    uint64_t duration;
    if (!device_idle_timeout_ms(timeout, &duration)) return false;
    observe_clock(p, now_ms);
    if (p->timeout != timeout) {
        p->timeout = timeout;
        p->asleep = false;
        p->last_activity_ms = now_ms;
    }
    return true;
}

bool device_idle_tick(device_idle_t *p, uint64_t now_ms)
{
    const bool was_asleep = p->asleep;
    observe_clock(p, now_ms);
    uint64_t duration;
    if (!device_idle_timeout_ms(p->timeout, &duration)) {
        /* Corrupt policy state must not unexpectedly hide the screen. */
        p->asleep = false;
    } else if (duration && !p->asleep && !any_down(p) && !p->wake_capture &&
               now_ms >= p->last_activity_ms && now_ms - p->last_activity_ms >= duration) {
        p->asleep = true;
        consume_history(p);
    }
    return was_asleep != p->asleep;
}

device_input_route_t device_idle_input(device_idle_t *p,
    device_input_source_t source, device_input_phase_t phase, uint64_t token,
    uint64_t now_ms)
{
    observe_clock(p, now_ms);
    if (source < DEVICE_INPUT_TOUCH || source >= DEVICE_INPUT_SOURCE_COUNT ||
        phase < DEVICE_INPUT_BEGIN || phase > DEVICE_INPUT_CANCEL || token == 0)
        return DEVICE_INPUT_INVALID;
    if (phase == DEVICE_INPUT_BEGIN) {
        if (p->down[source] || token <= p->latest_token[source]) return DEVICE_INPUT_INVALID;
        const bool consume = p->asleep || p->clock_regressed || captured_down(p) ||
            (source == DEVICE_INPUT_AI && p->ai_wake_waiting);
        p->latest_token[source] = token;
        p->action_seen[source] = false;
        p->down[source] = true;
        p->asleep = false;
        p->last_activity_ms = now_ms;
        if (consume) p->consumed_through[source] = token;
        if (source == DEVICE_INPUT_AI) p->ai_wake_waiting = false;
        refresh_capture(p);
        return consume ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_PASS;
    }
    if (token != p->latest_token[source]) {
        return token <= p->consumed_through[source] ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_INVALID;
    }
    const bool consume = token <= p->consumed_through[source] || p->asleep || p->clock_regressed;
    if (phase == DEVICE_INPUT_ACTION) {
        if (p->action_seen[source]) return consume ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_INVALID;
        p->action_seen[source] = true;
        if (consume) {
            p->consumed_through[source] = token;
            return DEVICE_INPUT_CONSUME;
        }
        p->last_activity_ms = now_ms;
        return DEVICE_INPUT_PASS;
    }
    if (!p->down[source]) return consume ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_INVALID;
    if (phase == DEVICE_INPUT_MOVE) {
        p->last_activity_ms = now_ms;
        return consume ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_PASS;
    }
    p->down[source] = false;
    p->last_activity_ms = now_ms;
    if (phase == DEVICE_INPUT_CANCEL) {
        p->consumed_through[source] = token;
        p->action_seen[source] = true;
    }
    if (source == DEVICE_INPUT_AI && consume) {
        p->ai_wake_waiting = true;
        p->ai_release_ms = now_ms;
    }
    refresh_capture(p);
    return consume ? DEVICE_INPUT_CONSUME : DEVICE_INPUT_PASS;
}
