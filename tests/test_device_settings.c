// SPDX-License-Identifier: Apache-2.0
#include "device_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* CRC fixtures were independently calculated with Python zlib.crc32. */
static const uint8_t default_record[16] = {
    0x44, 0x53, 0x45, 0x54, 0x02, 0x10, 0x01, 0x00,
    0x01, 0x32, 0x05, 0x00, 0x93, 0xc7, 0x42, 0xf2
};
static const uint8_t stored_record_70[16] = {
    0x44, 0x53, 0x45, 0x54, 0x02, 0x10, 0x01, 0x00,
    0x01, 0x46, 0x05, 0x00, 0x1f, 0x07, 0xba, 0xa1
};
static const uint8_t legacy_record[16] = {
    0x44, 0x53, 0x45, 0x54, 0x01, 0x10, 0x01, 0x00,
    0x01, 0x46, 0x05, 0x00, 0xfc, 0x00, 0x35, 0x2f
};

static void settings_codec_and_changes(void)
{
    device_settings_t settings;
    device_settings_defaults(&settings);
    assert(device_settings_validate(&settings));
    assert(settings.theme == DEVICE_THEME_DARK && settings.host_modifier == DEVICE_HOST_CTRL);
    assert(settings.haptics_enabled && settings.brightness_percent == 50);
    assert(settings.idle_timeout == DEVICE_IDLE_5_MIN);
    assert(settings.preferred_transport == DEVICE_TRANSPORT_BLUETOOTH);
    uint8_t record[16];
    assert(device_settings_encode(&settings, record, sizeof record) == DEVICE_SETTINGS_OK);
    assert(memcmp(record, default_record, sizeof record) == 0);
    device_settings_t decoded;
    assert(device_settings_decode(default_record, sizeof default_record, &decoded) == DEVICE_SETTINGS_OK);
    assert(device_settings_equal(&settings, &decoded));
    device_settings_t stored_settings = settings;
    stored_settings.brightness_percent = 70;
    assert(device_settings_decode(stored_record_70, sizeof stored_record_70, &decoded) == DEVICE_SETTINGS_OK);
    assert(device_settings_equal(&stored_settings, &decoded));
    decoded.preferred_transport = DEVICE_TRANSPORT_USB;
    assert(device_settings_decode(legacy_record, sizeof legacy_record, &decoded) == DEVICE_SETTINGS_OK);
    assert(device_settings_equal(&stored_settings, &decoded));
    assert(device_settings_encode(&decoded, record, sizeof record) == DEVICE_SETTINGS_OK);
    /* Accepted v1 maps to v2 Bluetooth while preserving its stored brightness. */
    assert(memcmp(record, stored_record_70, sizeof record) == 0);
    assert(device_settings_apply(&stored_settings, &decoded) == DEVICE_SETTINGS_APPLY_UNCHANGED);
    assert(settings.brightness_percent == 50);
    static const uint8_t usb_record[16] = {
        0x44,0x53,0x45,0x54,2,16,1,0,1,70,5,1,0x89,0x37,0xbd,0xd6
    };
    decoded.preferred_transport = DEVICE_TRANSPORT_USB;
    assert(device_settings_encode(&decoded, record, sizeof record) == DEVICE_SETTINGS_OK);
    assert(memcmp(record, usb_record, sizeof record) == 0);
    assert(device_settings_decode(usb_record, sizeof usb_record, &decoded) == DEVICE_SETTINGS_OK);
    assert(decoded.preferred_transport == DEVICE_TRANSPORT_USB);

    const device_idle_timeout_t timeouts[] = {
        DEVICE_IDLE_OFF, DEVICE_IDLE_1_MIN, DEVICE_IDLE_5_MIN, DEVICE_IDLE_10_MIN, DEVICE_IDLE_30_MIN
    };
    for (unsigned int theme = 0; theme < 2; ++theme)
        for (unsigned int host = 0; host < 2; ++host)
            for (unsigned int haptics = 0; haptics < 2; ++haptics)
                for (unsigned int brightness = 10; brightness <= 100; brightness += 10)
                    for (size_t t = 0; t < sizeof timeouts / sizeof timeouts[0]; ++t) {
                        device_settings_t candidate = {
                            .theme = (device_theme_t)theme, .host_modifier = (device_host_modifier_t)host,
                            .haptics_enabled = haptics != 0, .brightness_percent = (uint8_t)brightness,
                            .idle_timeout = timeouts[t]
                        };
                        for (unsigned int transport = 0; transport < 2; ++transport) {
                            candidate.preferred_transport = (device_transport_t)transport;
                            assert(device_settings_validate(&candidate));
                            assert(device_settings_encode(&candidate, record, sizeof record) == DEVICE_SETTINGS_OK);
                            assert(device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
                            assert(device_settings_equal(&candidate, &decoded));
                        }
                    }
    device_settings_defaults(&settings);
    device_settings_t candidate = settings;
    unsigned int save_requests = 0;
    for (size_t i = 0; i < 100000; ++i)
        assert(device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_UNCHANGED);
    candidate.theme = DEVICE_THEME_LIGHT;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    candidate.host_modifier = DEVICE_HOST_COMMAND;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    candidate.haptics_enabled = false;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    candidate.brightness_percent = 100;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    candidate.idle_timeout = DEVICE_IDLE_30_MIN;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    candidate.preferred_transport = DEVICE_TRANSPORT_USB;
    assert(!device_settings_equal(&settings, &candidate));
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++save_requests;
    assert(save_requests == 6);
    assert(device_settings_equal(&settings, &candidate));
    assert(device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_UNCHANGED);
}

static void rejected_records_and_values(void)
{
    device_settings_t sentinel;
    device_settings_defaults(&sentinel);
    sentinel.theme = DEVICE_THEME_LIGHT;
    sentinel.host_modifier = DEVICE_HOST_COMMAND;
    sentinel.haptics_enabled = false;
    sentinel.brightness_percent = 10;
    sentinel.idle_timeout = DEVICE_IDLE_30_MIN;
    sentinel.preferred_transport = DEVICE_TRANSPORT_USB;
    for (size_t byte = 0; byte < 16; ++byte) {
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint8_t corrupt[16];
            memcpy(corrupt, default_record, sizeof corrupt);
            corrupt[byte] ^= (uint8_t)(1U << bit);
            device_settings_t output = sentinel;
            assert(device_settings_decode(corrupt, sizeof corrupt, &output) != DEVICE_SETTINGS_OK);
            assert(device_settings_equal(&output, &sentinel));
        }
    }
    /* Valid CRC must not make unsupported values, flags or v1 reserved bytes valid. */
    static const uint8_t invalid_values[][16] = {
        {0x44,0x53,0x45,0x54,1,16,2,0,1,70,5,0,0x52,0x72,0xa1,0xa9},
        {0x44,0x53,0x45,0x54,1,16,1,2,1,70,5,0,0x9c,0x53,0xf5,0x55},
        {0x44,0x53,0x45,0x54,1,16,1,0,2,70,5,0,0x12,0xaf,0x80,0x3d},
        {0x44,0x53,0x45,0x54,1,16,1,0,1,0,5,0,0x8e,0xf1,0x22,0x5b},
        {0x44,0x53,0x45,0x54,1,16,1,0,1,15,5,0,0xb3,0xb6,0x7e,0x50},
        {0x44,0x53,0x45,0x54,1,16,1,0,1,110,5,0,0xa4,0x17,0x6b,0x19},
        {0x44,0x53,0x45,0x54,1,16,1,0,1,70,2,0,0x3b,0x96,0x74,0x60},
        {0x44,0x53,0x45,0x54,1,16,1,0,1,70,5,1,0x6a,0x30,0x32,0x58},
        {0x44,0x53,0x45,0x54,2,16,1,0,1,70,5,2,0x33,0x66,0xb4,0x4f}
    };
    for (size_t i = 0; i < sizeof invalid_values / sizeof invalid_values[0]; ++i) {
        device_settings_t output = sentinel;
        assert(device_settings_decode(invalid_values[i], 16, &output) == DEVICE_SETTINGS_INVALID_VALUE);
        assert(device_settings_equal(&output, &sentinel));
    }
    for (size_t length = 0; length < 33; ++length) {
        if (length == 16) continue;
        device_settings_t output = sentinel;
        assert(device_settings_decode(default_record, length, &output) == DEVICE_SETTINGS_INVALID_LENGTH);
        assert(device_settings_equal(&output, &sentinel));
        uint8_t buffer[32];
        memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&sentinel, buffer, length) == DEVICE_SETTINGS_INVALID_LENGTH);
        for (size_t i = 0; i < sizeof buffer; ++i) assert(buffer[i] == 0xaa);
    }
    assert(device_settings_decode(NULL, 16, &sentinel) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(device_settings_decode(default_record, 16, NULL) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(!device_settings_validate(NULL) && !device_settings_equal(NULL, &sentinel));
    assert(device_settings_encode(NULL, (uint8_t[16]){0}, 16) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(device_settings_encode(&sentinel, NULL, 16) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    for (unsigned int brightness = 0; brightness < 256; ++brightness) {
        device_settings_t value = sentinel;
        value.brightness_percent = (uint8_t)brightness;
        assert(device_settings_validate(&value) ==
               (brightness >= 10 && brightness <= 100 && brightness % 10 == 0));
    }
    const int invalid_timeouts[] = {-1, 2, 3, 4, 6, 29, 31, 255};
    for (size_t i = 0; i < sizeof invalid_timeouts / sizeof invalid_timeouts[0]; ++i) {
        device_settings_t value = sentinel;
        value.idle_timeout = (device_idle_timeout_t)invalid_timeouts[i];
        assert(!device_settings_validate(&value));
        uint64_t duration = 123;
        assert(!device_idle_timeout_ms(value.idle_timeout, &duration) && duration == 123);
        device_settings_t current = sentinel;
        assert(device_settings_apply(&current, &value) == DEVICE_SETTINGS_APPLY_INVALID);
        assert(device_settings_equal(&current, &sentinel));
        uint8_t buffer[16];
        memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&value, buffer, 16) == DEVICE_SETTINGS_INVALID_VALUE);
        for (size_t j = 0; j < sizeof buffer; ++j) assert(buffer[j] == 0xaa);
    }
    assert(!device_idle_timeout_ms(DEVICE_IDLE_1_MIN, NULL));
    const int invalid_transports[] = {-1, 2, 255};
    for (size_t i = 0; i < sizeof invalid_transports / sizeof invalid_transports[0]; ++i) {
        device_settings_t candidate = sentinel;
        candidate.preferred_transport = (device_transport_t)invalid_transports[i];
        assert(!device_settings_validate(&candidate));
        assert(device_settings_apply(&sentinel, &candidate) == DEVICE_SETTINGS_APPLY_INVALID);
        assert(sentinel.preferred_transport == DEVICE_TRANSPORT_USB);
        uint8_t buffer[16];
        memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&candidate, buffer, sizeof buffer) == DEVICE_SETTINGS_INVALID_VALUE);
        for (size_t j = 0; j < sizeof buffer; ++j) assert(buffer[j] == 0xaa);
    }
    uint8_t legacy_corrupt[16];
    memcpy(legacy_corrupt, legacy_record, sizeof legacy_corrupt);
    legacy_corrupt[15] ^= 1;
    device_settings_t legacy_output = sentinel;
    assert(device_settings_decode(legacy_corrupt, sizeof legacy_corrupt, &legacy_output) ==
           DEVICE_SETTINGS_INVALID_CHECKSUM);
    assert(device_settings_equal(&legacy_output, &sentinel));
    static const uint8_t future[16] = {
        0x44,0x53,0x45,0x54,3,16,1,0,1,70,5,0,0x81,0x07,0x10,0x6d
    };
    device_settings_t untouched = sentinel;
    assert(device_settings_decode(future, sizeof future, &untouched) == DEVICE_SETTINGS_UNSUPPORTED_VERSION);
    assert(device_settings_equal(&untouched, &sentinel));
}

static void sleep_at_minute(device_idle_t *p)
{
    device_idle_init(p, 0);
    assert(device_idle_configure(p, DEVICE_IDLE_1_MIN, 0));
    assert(device_idle_tick(p, 60000) && p->asleep);
}

static device_input_route_t input(device_idle_t *p, device_input_source_t source,
                                 device_input_phase_t phase, uint64_t token, uint64_t now)
{
    const device_input_route_t result = device_idle_input(p, source, phase, token, now);
    if (p->asleep) {
        assert(!p->wake_capture);
        assert(!p->down[DEVICE_INPUT_TOUCH] && !p->down[DEVICE_INPUT_AI]);
    }
    return result;
}

static void idle_boundaries_and_configuration(void)
{
    const device_idle_timeout_t timeouts[] = {
        DEVICE_IDLE_1_MIN, DEVICE_IDLE_5_MIN, DEVICE_IDLE_10_MIN, DEVICE_IDLE_30_MIN
    };
    const uint64_t durations[] = {60000, 300000, 600000, 1800000};
    for (size_t i = 0; i < sizeof timeouts / sizeof timeouts[0]; ++i) {
        device_idle_t p;
        device_idle_init(&p, 100);
        assert(device_idle_configure(&p, timeouts[i], 100));
        assert(!device_idle_tick(&p, 100 + durations[i] - 1) && !p.asleep);
        assert(device_idle_tick(&p, 100 + durations[i]) && p.asleep);
        assert(!device_idle_tick(&p, 101 + durations[i]) && p.asleep);
    }
    device_idle_t p;
    device_idle_init(&p, 0);
    /* No power information exists: default screen-off still applies at five minutes. */
    for (uint64_t now = 10; now < 300000; now += 10)
        assert(!device_idle_tick(&p, now) && !p.asleep);
    assert(device_idle_tick(&p, 300000) && p.asleep);
    assert(device_idle_configure(&p, DEVICE_IDLE_OFF, 300001) && !p.asleep);
    assert(!device_idle_tick(&p, 10000000) && !p.asleep);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, 10000000));
    for (uint64_t now = 10001000; now < 10060000; now += 1000) {
        assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, now));
        assert(!device_idle_tick(&p, now)); /* Repeated settings/render snapshots do not renew idle. */
    }
    assert(device_idle_tick(&p, 10060000));
    assert(!device_idle_configure(&p, (device_idle_timeout_t)2, 10060001) && p.asleep);
    assert(device_idle_activity(&p, 10060002) && !p.asleep); /* Explicit changed theme/brightness/etc. */
    assert(!device_idle_tick(&p, 10120001));
    assert(device_idle_tick(&p, 10120002));

    device_idle_init(&p, 0);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, 0));
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 10) == DEVICE_INPUT_PASS);
    assert(!device_idle_tick(&p, 1000000) && !p.asleep); /* A physical contact is still held. */
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 1000001) == DEVICE_INPUT_PASS);
    assert(!device_idle_tick(&p, 1060000));
    assert(device_idle_tick(&p, 1060001));
}

static void physical_activity_without_queue_edges(void)
{
    device_idle_t p;
    device_idle_init(&p, 0);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, 0));
    device_idle_note_physical_activity(&p, 59000);
    assert(!device_idle_tick(&p, 1000) && !p.clock_regressed); /* New callback can follow last policy tick. */
    device_idle_note_physical_activity(&p, 59000);
    device_idle_note_physical_activity(&p, 100);
    assert(!device_idle_tick(&p, 118999) && !p.asleep);
    assert(device_idle_tick(&p, 119000) && p.asleep);
    device_idle_note_physical_activity(&p, 200000);
    assert(p.asleep && !device_idle_tick(&p, 119001)); /* Callback activity alone never wakes. */
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 119002) == DEVICE_INPUT_CONSUME);
    assert(!p.asleep && p.wake_capture);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 119003) == DEVICE_INPUT_CONSUME);
    assert(!device_idle_tick(&p, 179002));
    assert(device_idle_tick(&p, 179003));
}

static void touch_wake_and_delayed_callbacks(void)
{
    device_idle_t p;
    device_idle_init(&p, 0);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, 0));
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 1) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 2) == DEVICE_INPUT_PASS);
    assert(device_idle_tick(&p, 60002));
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 1, 60003) == DEVICE_INPUT_CONSUME);
    assert(p.asleep); /* Old queue payload must neither wake nor type. */
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 2, 60004) == DEVICE_INPUT_CONSUME);
    assert(!p.asleep && p.wake_capture);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_MOVE, 2, 60005) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 2, 60006) == DEVICE_INPUT_CONSUME);
    assert(!p.wake_capture);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 3, 60007) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 2, 60008) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 1, 60009) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 3, 60010) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 3, 60011) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 3, 70000) == DEVICE_INPUT_INVALID);
    assert(!device_idle_tick(&p, 120010));
    assert(device_idle_tick(&p, 120011)); /* Duplicate actions did not prolong the deadline. */
}

static void ai_wake_double_hold_and_multiple_inputs(void)
{
    device_idle_t p;
    sleep_at_minute(&p);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 1, 60001) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 1, 60901) == DEVICE_INPUT_CONSUME); /* Wake hold cannot unlock. */
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 1, 60902) == DEVICE_INPUT_CONSUME);
    assert(!device_idle_tick(&p, 61181) && p.wake_capture); /* 279 ms after release. */
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 2, 61181) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 61182) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 2, 61190) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 61200) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 2, 61201) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 2, 61202) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 2, 61203) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 2, 61204) == DEVICE_INPUT_CONSUME); /* Wake double sequence. */
    assert(!device_idle_tick(&p, 61469) && p.wake_capture);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 3, 61470) == DEVICE_INPUT_PASS); /* Exact 280 ms boundary. */
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 2, 61471) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 3, 62370) == DEVICE_INPUT_PASS); /* A fresh hold can unlock. */
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 3, 62371) == DEVICE_INPUT_PASS);

    sleep_at_minute(&p);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 60001) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 1, 60002) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 60003) == DEVICE_INPUT_CONSUME);
    assert(p.wake_capture);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 1, 60004) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 2, 60005) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_CANCEL, 2, 60006) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 2, 60007) == DEVICE_INPUT_CONSUME);
}

static void malformed_events_and_clock_boundaries(void)
{
    device_idle_t p;
    device_idle_init(&p, 0);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, 0));
    assert(input(&p, (device_input_source_t)-1, DEVICE_INPUT_BEGIN, 1, 1) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_SOURCE_COUNT, DEVICE_INPUT_BEGIN, 1, 2) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_TOUCH, (device_input_phase_t)99, 1, 3) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 0, 4) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_MOVE, 1, 5) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 1, 6) == DEVICE_INPUT_INVALID);
    assert(device_idle_tick(&p, 60000)); /* Malformed events cannot keep the display awake. */
    assert(device_idle_tick(&p, 1000) && !p.asleep && p.clock_regressed);
    assert(!device_idle_tick(&p, 60999));
    assert(device_idle_tick(&p, 61000));

    device_idle_init(&p, 1000);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 1, 1001) == DEVICE_INPUT_PASS);
    assert(!device_idle_tick(&p, 900) && p.clock_regressed && p.wake_capture);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, 1, 901) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 1, 902) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 2, 1181) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 2, 1182) == DEVICE_INPUT_CONSUME);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 3, 1462) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, 3, 1463) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 3, 1464) == DEVICE_INPUT_INVALID);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, UINT64_MAX, 1465) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_END, UINT64_MAX, 1466) == DEVICE_INPUT_PASS);
    assert(input(&p, DEVICE_INPUT_AI, DEVICE_INPUT_BEGIN, 1, 1467) == DEVICE_INPUT_INVALID);

    device_idle_init(&p, UINT64_MAX - 60000);
    assert(device_idle_configure(&p, DEVICE_IDLE_1_MIN, UINT64_MAX - 60000));
    assert(!device_idle_tick(&p, UINT64_MAX - 1));
    assert(device_idle_tick(&p, UINT64_MAX));
    assert(device_idle_tick(&p, 0) && !p.asleep && p.clock_regressed);
    assert(!device_idle_tick(&p, 59999));
    assert(device_idle_tick(&p, 60000));

    device_idle_init(&p, 1000);
    assert(device_idle_configure(&p, DEVICE_IDLE_10_MIN, 900));
    assert(p.clock_regressed && !p.asleep); /* A changed setting must not hide the clock fault. */
    assert(!device_idle_tick(&p, 600899));
    assert(device_idle_tick(&p, 600900));
}

int main(void)
{
    settings_codec_and_changes();
    rejected_records_and_values();
    idle_boundaries_and_configuration();
    physical_activity_without_queue_edges();
    touch_wake_and_delayed_callbacks();
    ai_wake_double_hold_and_multiple_inputs();
    malformed_events_and_clock_boundaries();
    puts("PASS: settings codec, validation, change detection, idle deadlines and whole-gesture wake routing");
    return 0;
}
