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
        .haptics_enabled = true, .brightness_percent = 50,
        .idle_timeout = DEVICE_IDLE_5_MIN, .preferred_transport = DEVICE_TRANSPORT_BLUETOOTH,
        .tap_to_click = true, .bluetooth_name = DEVICE_SETTINGS_BLUETOOTH_DEFAULT_NAME,
        .motion_gain_x_centi = 400, .motion_gain_y_centi = 400,
        .motion_deadzone_mdps = 500, .motion_filter_ms = 30, .motion_freeze_ms = 150
    };
}

bool device_settings_bluetooth_name_valid(const char *utf8, size_t length)
{
    if (!utf8 || !length || length > DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES ||
        utf8[0] == ' ' || utf8[length - 1] == ' ') return false;
    const unsigned char *bytes = (const unsigned char *)utf8;
    for (size_t i = 0; i < length;) {
        const unsigned char first = bytes[i++];
        uint32_t point;
        unsigned continuation;
        uint32_t minimum;
        if (first < 0x80U) { point = first; continuation = 0; minimum = 0; }
        else if (first >= 0xc2U && first <= 0xdfU) {
            point = first & 0x1fU; continuation = 1; minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            point = first & 0x0fU; continuation = 2; minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            point = first & 0x07U; continuation = 3; minimum = 0x10000U;
        } else return false;
        if (continuation > length - i) return false;
        for (unsigned j = 0; j < continuation; ++j) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0U) != 0x80U) return false;
            point = (point << 6) | (next & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU) || point < 0x20U ||
            (point >= 0x7fU && point <= 0x9fU)) return false;
    }
    return true;
}

static size_t bluetooth_name_length(const char *name)
{
    size_t length = 0;
    while (length <= DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES && name[length]) ++length;
    return length;
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
        device_settings_bluetooth_name_valid(settings->bluetooth_name,
            bluetooth_name_length(settings->bluetooth_name)) &&
        settings->motion_gain_x_centi >= 25 && settings->motion_gain_x_centi <= 10000 &&
        settings->motion_gain_y_centi >= 25 && settings->motion_gain_y_centi <= 10000 &&
        settings->motion_deadzone_mdps <= 10000 && settings->motion_filter_ms <= 1000 &&
        settings->motion_freeze_ms <= 1000 &&
        device_idle_timeout_ms(settings->idle_timeout, &duration);
}

bool device_settings_equal(const device_settings_t *a, const device_settings_t *b)
{
    return device_settings_validate(a) && device_settings_validate(b) &&
        a->theme == b->theme && a->host_modifier == b->host_modifier &&
        a->haptics_enabled == b->haptics_enabled &&
        a->brightness_percent == b->brightness_percent && a->idle_timeout == b->idle_timeout &&
        a->preferred_transport == b->preferred_transport && a->tap_to_click == b->tap_to_click &&
        a->motion_gain_x_centi == b->motion_gain_x_centi &&
        a->motion_gain_y_centi == b->motion_gain_y_centi &&
        a->motion_deadzone_mdps == b->motion_deadzone_mdps &&
        a->motion_filter_ms == b->motion_filter_ms && a->motion_freeze_ms == b->motion_freeze_ms &&
        a->motion_invert_x == b->motion_invert_x && a->motion_invert_y == b->motion_invert_y &&
        a->navigation == b->navigation &&
        strcmp(a->bluetooth_name, b->bluetooth_name) == 0;
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

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
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
        (uint8_t)settings->idle_timeout, (uint8_t)settings->preferred_transport,
        settings->tap_to_click ? 1U : 0U, (uint8_t)bluetooth_name_length(settings->bluetooth_name)
    };
    memcpy(encoded + 14, settings->bluetooth_name, encoded[13]);
    put_u16(encoded + 36, settings->motion_gain_x_centi);
    put_u16(encoded + 38, settings->motion_gain_y_centi);
    put_u16(encoded + 40, settings->motion_deadzone_mdps);
    put_u16(encoded + 42, settings->motion_filter_ms);
    put_u16(encoded + 44, settings->motion_freeze_ms);
    encoded[46] = (uint8_t)((settings->motion_invert_x ? 1U : 0U) |
                          (settings->motion_invert_y ? 2U : 0U));
    encoded[47] = settings->navigation ? 1U : 0U;
    const size_t crc_at = sizeof encoded - 4U;
    const uint32_t crc = checksum(encoded, crc_at);
    for (unsigned int i = 0; i < 4; ++i) encoded[crc_at + i] = (uint8_t)(crc >> (8U * i));
    memcpy(record, encoded, sizeof encoded);
    return DEVICE_SETTINGS_OK;
}

device_settings_status_t device_settings_decode(const uint8_t *record, size_t length,
                                                device_settings_t *settings)
{
    if (!record || !settings) return DEVICE_SETTINGS_INVALID_ARGUMENT;
    if (length != DEVICE_SETTINGS_RECORD_SIZE && length != DEVICE_SETTINGS_V3_RECORD_SIZE &&
        length != DEVICE_SETTINGS_LEGACY_RECORD_SIZE)
        return DEVICE_SETTINGS_INVALID_LENGTH;
    if (memcmp(record, "DSET", 4) != 0) return DEVICE_SETTINGS_INVALID_MAGIC;
    if (record[4] != 1 && record[4] != 2 && record[4] != 3 && record[4] != 4 &&
        record[4] != DEVICE_SETTINGS_SCHEMA_VERSION)
        return DEVICE_SETTINGS_UNSUPPORTED_VERSION;
    const bool legacy = record[4] < 3;
    const size_t expected = legacy ? DEVICE_SETTINGS_LEGACY_RECORD_SIZE :
        record[4] == 3 ? DEVICE_SETTINGS_V3_RECORD_SIZE : DEVICE_SETTINGS_RECORD_SIZE;
    if (length != expected || record[5] != expected) return DEVICE_SETTINGS_INVALID_LENGTH;
    const size_t crc_at = expected - 4;
    uint32_t stored = 0;
    for (unsigned int i = 0; i < 4; ++i) stored |= (uint32_t)record[crc_at + i] << (8U * i);
    if (stored != checksum(record, crc_at)) return DEVICE_SETTINGS_INVALID_CHECKSUM;
    if (record[8] > 1 || (record[4] == 1 && record[11] != 0)) return DEVICE_SETTINGS_INVALID_VALUE;
    device_settings_t decoded;
    device_settings_defaults(&decoded);
    decoded.theme = (device_theme_t)record[6];
    decoded.host_modifier = (device_host_modifier_t)record[7];
    decoded.haptics_enabled = record[8] != 0;
    decoded.brightness_percent = record[9];
    decoded.idle_timeout = (device_idle_timeout_t)record[10];
    decoded.preferred_transport = record[4] == 1 ? DEVICE_TRANSPORT_BLUETOOTH :
        (device_transport_t)record[11];
    if (!legacy) {
        if (record[12] > 1 || !device_settings_bluetooth_name_valid((const char *)record + 14, record[13]))
            return DEVICE_SETTINGS_INVALID_VALUE;
        for (size_t i = 14U + record[13]; i < 36; ++i)
            if (record[i]) return DEVICE_SETTINGS_INVALID_VALUE;
        decoded.tap_to_click = record[12] != 0;
        memset(decoded.bluetooth_name, 0, sizeof decoded.bluetooth_name);
        memcpy(decoded.bluetooth_name, record + 14, record[13]);
    }
    if (record[4] >= 4) {
        if (record[46] & ~3U) return DEVICE_SETTINGS_INVALID_VALUE;
        if (record[47] > (record[4] >= 5 ? 1U : 0U)) return DEVICE_SETTINGS_INVALID_VALUE;
        for (size_t i = 48; i < crc_at; ++i)
            if (record[i]) return DEVICE_SETTINGS_INVALID_VALUE;
        decoded.motion_gain_x_centi = get_u16(record + 36);
        decoded.motion_gain_y_centi = get_u16(record + 38);
        decoded.motion_deadzone_mdps = get_u16(record + 40);
        decoded.motion_filter_ms = get_u16(record + 42);
        decoded.motion_freeze_ms = get_u16(record + 44);
        decoded.motion_invert_x = (record[46] & 1U) != 0;
        decoded.motion_invert_y = (record[46] & 2U) != 0;
        decoded.navigation = record[47] != 0;
    }
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

bool device_idle_request_sleep(device_idle_t *p, uint64_t now_ms)
{
    if (!p) return false;
    observe_clock(p, now_ms);
    if (p->clock_regressed || any_down(p) || p->wake_capture) return false;
    consume_history(p);
    p->asleep = true;
    return true;
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
