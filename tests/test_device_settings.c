// SPDX-License-Identifier: Apache-2.0
#include "device_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Frozen CRC fixtures were independently calculated with Python zlib.crc32. */
static const uint8_t default_record[56] = {
    0x44, 0x53, 0x45, 0x54, 0x04, 0x38, 0x01, 0x00,
    0x01, 0x32, 0x05, 0x00, 0x01, 0x0b, 0x45, 0x53,
    0x50, 0x33, 0x32, 0x2d, 0x49, 0x4e, 0x50, 0x55,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x90, 0x01, 0x90, 0x01,
    0xf4, 0x01, 0x1e, 0x00, 0x96, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x89, 0x9e, 0xf8, 0x2a
};
static const uint8_t default_v5_record[56] = {
    0x44, 0x53, 0x45, 0x54, 0x05, 0x38, 0x01, 0x00,
    0x01, 0x32, 0x05, 0x00, 0x01, 0x0b, 0x45, 0x53,
    0x50, 0x33, 0x32, 0x2d, 0x49, 0x4e, 0x50, 0x55,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x90, 0x01, 0x90, 0x01,
    0xf4, 0x01, 0x1e, 0x00, 0x96, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x55, 0x72, 0x49, 0x17,
};
static const uint8_t v3_record[DEVICE_SETTINGS_V3_RECORD_SIZE] = {
    0x44, 0x53, 0x45, 0x54, 0x03, 0x28, 0x01, 0x00,
    0x01, 0x32, 0x05, 0x00, 0x01, 0x0b, 0x45, 0x53,
    0x50, 0x33, 0x32, 0x2d, 0x49, 0x4e, 0x50, 0x55,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0c, 0xf9, 0x20, 0xa7
};
static const uint8_t legacy_record[16] = {
    0x44,0x53,0x45,0x54,1,16,1,0,1,70,5,0,0xfc,0x00,0x35,0x2f
};
static const uint8_t legacy_v2_record[16] = {
    0x44,0x53,0x45,0x54,2,16,1,0,1,70,5,0,0x1f,0x07,0xba,0xa1
};
static const uint8_t legacy_usb_record[16] = {
    0x44,0x53,0x45,0x54,2,16,1,0,1,70,5,1,0x89,0x37,0xbd,0xd6
};

static void settings_codec_and_changes(void)
{
    device_settings_t settings, decoded;
    device_settings_defaults(&settings);
    assert(device_settings_validate(&settings));
    assert(settings.theme == DEVICE_THEME_DARK && settings.host_modifier == DEVICE_HOST_CTRL);
    assert(settings.haptics_enabled && settings.brightness_percent == 50);
    assert(settings.idle_timeout == DEVICE_IDLE_5_MIN && settings.tap_to_click);
    assert(settings.preferred_transport == DEVICE_TRANSPORT_BLUETOOTH && !settings.navigation);
    assert(strcmp(settings.bluetooth_name, "ESP32") == 0);
    uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
    assert(device_settings_encode(&settings, record, sizeof record) == DEVICE_SETTINGS_OK);
    assert(memcmp(record, default_v5_record, sizeof record) == 0);
    assert(device_settings_decode(default_record, sizeof default_record, &decoded) == DEVICE_SETTINGS_OK);
    assert(device_settings_equal(&settings, &decoded));
    assert(device_settings_decode(v3_record, sizeof v3_record, &decoded) == DEVICE_SETTINGS_OK);
    assert(device_settings_equal(&settings, &decoded));
    const uint8_t *const legacy[] = {legacy_record, legacy_v2_record, legacy_usb_record};
    for (size_t i = 0; i < sizeof legacy / sizeof legacy[0]; ++i) {
        device_settings_t expected = settings;
        expected.brightness_percent = 70;
        expected.preferred_transport = i == 2 ? DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH;
        decoded.tap_to_click = false;
        strcpy(decoded.bluetooth_name, "Different name");
        assert(device_settings_decode(legacy[i], 16, &decoded) == DEVICE_SETTINGS_OK);
        assert(device_settings_equal(&expected, &decoded));
        assert(device_settings_encode(&decoded, record, sizeof record) == DEVICE_SETTINGS_OK);
        assert(record[4] == 5 && record[5] == 56 && record[9] == 70);
        assert(device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
        assert(device_settings_equal(&expected, &decoded));
        assert(device_settings_apply(&expected, &decoded) == DEVICE_SETTINGS_APPLY_UNCHANGED);
    }
    const device_idle_timeout_t timeouts[] = {
        DEVICE_IDLE_OFF, DEVICE_IDLE_1_MIN, DEVICE_IDLE_5_MIN, DEVICE_IDLE_10_MIN, DEVICE_IDLE_30_MIN
    };
    for (unsigned theme = 0; theme < 2; ++theme)
        for (unsigned host = 0; host < 2; ++host)
            for (unsigned haptics = 0; haptics < 2; ++haptics)
                for (unsigned brightness = 10; brightness <= 100; brightness += 10)
                    for (size_t t = 0; t < sizeof timeouts / sizeof timeouts[0]; ++t)
                        for (unsigned transport = 0; transport < 2; ++transport)
                            for (unsigned tap = 0; tap < 2; ++tap) {
                                device_settings_t candidate = settings;
                                candidate.theme = (device_theme_t)theme;
                                candidate.host_modifier = (device_host_modifier_t)host;
                                candidate.haptics_enabled = haptics != 0;
                                candidate.brightness_percent = (uint8_t)brightness;
                                candidate.idle_timeout = timeouts[t];
                                candidate.preferred_transport = (device_transport_t)transport;
                                candidate.tap_to_click = tap != 0;
                                assert(device_settings_encode(&candidate, record, sizeof record) == DEVICE_SETTINGS_OK);
                                assert(device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
                                assert(device_settings_equal(&candidate, &decoded));
                            }
    device_settings_t candidate = settings;
    for (size_t i = 0; i < 100000; ++i)
        assert(device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_UNCHANGED);
    unsigned saves = 0;
    candidate.theme = DEVICE_THEME_LIGHT;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.host_modifier = DEVICE_HOST_COMMAND;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.haptics_enabled = false;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.brightness_percent = 100;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.idle_timeout = DEVICE_IDLE_30_MIN;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.preferred_transport = DEVICE_TRANSPORT_USB;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.tap_to_click = false;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    strcpy(candidate.bluetooth_name, "Desk Pad");
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    candidate.navigation = true;
    if (device_settings_apply(&settings, &candidate) == DEVICE_SETTINGS_APPLY_CHANGED) ++saves;
    assert(saves == 9 && device_settings_equal(&settings, &candidate));
    assert(device_settings_encode(&settings, record, sizeof record) == DEVICE_SETTINGS_OK);
    assert(record[47] == 1 && device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
    assert(decoded.navigation && device_settings_equal(&settings, &decoded));
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
    sentinel.tap_to_click = false;
    strcpy(sentinel.bluetooth_name, "Sentinel");
    for (size_t byte = 0; byte < sizeof default_record; ++byte)
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint8_t corrupt[DEVICE_SETTINGS_RECORD_SIZE];
            memcpy(corrupt, default_record, sizeof corrupt);
            corrupt[byte] ^= (uint8_t)(1U << bit);
            device_settings_t output = sentinel;
            assert(device_settings_decode(corrupt, sizeof corrupt, &output) != DEVICE_SETTINGS_OK);
            assert(device_settings_equal(&output, &sentinel));
        }
    const uint8_t *const legacy[] = {legacy_record, legacy_v2_record, legacy_usb_record};
    for (size_t fixture = 0; fixture < sizeof legacy / sizeof legacy[0]; ++fixture)
        for (size_t byte = 0; byte < 16; ++byte)
            for (unsigned bit = 0; bit < 8; ++bit) {
                uint8_t corrupt[16]; memcpy(corrupt, legacy[fixture], sizeof corrupt);
                corrupt[byte] ^= (uint8_t)(1U << bit);
                device_settings_t output = sentinel;
                assert(device_settings_decode(corrupt, sizeof corrupt, &output) != DEVICE_SETTINGS_OK);
                assert(device_settings_equal(&output, &sentinel));
            }
    /* Each CRC below was calculated independently after that one-byte mutation. */
    const struct { size_t at; uint8_t value; uint32_t crc; } invalid[] = {
        {6, 2, UINT32_C(0xaaa2323c)},
        {7, 2, UINT32_C(0xb6a5c850)},
        {8, 2, UINT32_C(0x5480d62f)},
        {9, 0, UINT32_C(0x8185fb64)},
        {9, 15, UINT32_C(0xda8380df)},
        {9, 110, UINT32_C(0xd7256612)},
        {10, 2, UINT32_C(0xfbb60c81)},
        {11, 2, UINT32_C(0x21a6b98a)},
        {12, 2, UINT32_C(0x00442e01)},
        {13, 0, UINT32_C(0xe0b8d5f9)},
        {13, 19, UINT32_C(0x00956614)},
        {14, 0, UINT32_C(0x47985957)},
        {14, 32, UINT32_C(0xf78cb9cc)},
        {24, 32, UINT32_C(0x6cb05fcd)},
        {14, 194, UINT32_C(0x9160eb0a)},
        {25, 1, UINT32_C(0x447485c8)},
        {33, 1, UINT32_C(0xfd1a1ed1)},
        {35, 1, UINT32_C(0xad5e55ca)},
        {37, 255, UINT32_C(0x8f50aceb)},
        {39, 255, UINT32_C(0xca80cbaa)},
        {41, 40, UINT32_C(0xd7292fdb)},
        {43, 4, UINT32_C(0x7714cf85)},
        {45, 4, UINT32_C(0xdeb7ba9a)},
        {46, 4, UINT32_C(0xb169dc9f)},
        {47, 1, UINT32_C(0x1798b739)},
        {51, 1, UINT32_C(0x5dffae1f)}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
        memcpy(record, default_record, sizeof record);
        record[invalid[i].at] = invalid[i].value;
        for (unsigned byte = 0; byte < 4; ++byte)
            record[52 + byte] = (uint8_t)(invalid[i].crc >> (8U * byte));
        device_settings_t output = sentinel;
        assert(device_settings_decode(record, sizeof record, &output) == DEVICE_SETTINGS_INVALID_VALUE);
        assert(device_settings_equal(&output, &sentinel));
    }
    /* Frozen independent CRC for a v5 navigation flag outside its wire range. */
    uint8_t invalid_navigation[DEVICE_SETTINGS_RECORD_SIZE];
    memcpy(invalid_navigation, default_v5_record, sizeof invalid_navigation);
    invalid_navigation[47] = 2;
    uint32_t navigation_crc = UINT32_C(0x6d892135);
    for (unsigned byte = 0; byte < 4; ++byte)
        invalid_navigation[52 + byte] = (uint8_t)(navigation_crc >> (8U * byte));
    device_settings_t preserved = sentinel;
    assert(device_settings_decode(invalid_navigation, sizeof invalid_navigation, &preserved) == DEVICE_SETTINGS_INVALID_VALUE);
    assert(device_settings_equal(&preserved, &sentinel));
    static const uint8_t invalid_legacy[][16] = {
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
    for (size_t i = 0; i < sizeof invalid_legacy / sizeof invalid_legacy[0]; ++i) {
        device_settings_t output = sentinel;
        assert(device_settings_decode(invalid_legacy[i], 16, &output) == DEVICE_SETTINGS_INVALID_VALUE);
        assert(device_settings_equal(&output, &sentinel));
    }
    for (size_t length = 0; length < 65; ++length) {
        if (length == sizeof default_record) continue;
        device_settings_t output = sentinel;
        assert(device_settings_decode(default_record, length, &output) == DEVICE_SETTINGS_INVALID_LENGTH);
        assert(device_settings_equal(&output, &sentinel));
        uint8_t buffer[64]; memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&sentinel, buffer, length) == DEVICE_SETTINGS_INVALID_LENGTH);
        for (size_t i = 0; i < sizeof buffer; ++i) assert(buffer[i] == 0xaa);
    }
    assert(device_settings_decode(NULL, DEVICE_SETTINGS_RECORD_SIZE, &sentinel) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(device_settings_decode(default_record, DEVICE_SETTINGS_RECORD_SIZE, NULL) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(!device_settings_validate(NULL) && !device_settings_equal(NULL, &sentinel));
    assert(device_settings_encode(NULL, (uint8_t[DEVICE_SETTINGS_RECORD_SIZE]){0}, DEVICE_SETTINGS_RECORD_SIZE) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    assert(device_settings_encode(&sentinel, NULL, DEVICE_SETTINGS_RECORD_SIZE) == DEVICE_SETTINGS_INVALID_ARGUMENT);
    for (unsigned brightness = 0; brightness < 256; ++brightness) {
        device_settings_t value = sentinel;
        value.brightness_percent = (uint8_t)brightness;
        assert(device_settings_validate(&value) ==
            (brightness >= 10 && brightness <= 100 && brightness % 10 == 0));
    }
    const int invalid_timeouts[] = {-1,2,3,4,6,29,31,255};
    for (size_t i = 0; i < sizeof invalid_timeouts / sizeof invalid_timeouts[0]; ++i) {
        device_settings_t value = sentinel, current = sentinel;
        value.idle_timeout = (device_idle_timeout_t)invalid_timeouts[i];
        uint64_t duration = 123;
        assert(!device_idle_timeout_ms(value.idle_timeout, &duration) && duration == 123);
        assert(device_settings_apply(&current, &value) == DEVICE_SETTINGS_APPLY_INVALID);
        assert(device_settings_equal(&current, &sentinel));
        uint8_t buffer[DEVICE_SETTINGS_RECORD_SIZE]; memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&value, buffer, DEVICE_SETTINGS_RECORD_SIZE) == DEVICE_SETTINGS_INVALID_VALUE);
        for (size_t j = 0; j < sizeof buffer; ++j) assert(buffer[j] == 0xaa);
    }
    assert(!device_idle_timeout_ms(DEVICE_IDLE_1_MIN, NULL));
    const int invalid_transports[] = {-1,2,255};
    for (size_t i = 0; i < sizeof invalid_transports / sizeof invalid_transports[0]; ++i) {
        device_settings_t value = sentinel;
        value.preferred_transport = (device_transport_t)invalid_transports[i];
        assert(!device_settings_validate(&value));
        assert(device_settings_apply(&sentinel, &value) == DEVICE_SETTINGS_APPLY_INVALID);
        assert(sentinel.preferred_transport == DEVICE_TRANSPORT_USB);
        uint8_t buffer[DEVICE_SETTINGS_RECORD_SIZE]; memset(buffer, 0xaa, sizeof buffer);
        assert(device_settings_encode(&value, buffer, DEVICE_SETTINGS_RECORD_SIZE) == DEVICE_SETTINGS_INVALID_VALUE);
        for (size_t j = 0; j < sizeof buffer; ++j) assert(buffer[j] == 0xaa);
    }
    uint8_t damaged_legacy[16]; memcpy(damaged_legacy, legacy_record, sizeof damaged_legacy);
    damaged_legacy[15] ^= 1;
    device_settings_t output = sentinel;
    assert(device_settings_decode(damaged_legacy, 16, &output) == DEVICE_SETTINGS_INVALID_CHECKSUM);
    uint8_t future[DEVICE_SETTINGS_RECORD_SIZE]; memcpy(future, default_record, sizeof future); future[4] = 6;
    assert(device_settings_decode(future, DEVICE_SETTINGS_RECORD_SIZE, &output) == DEVICE_SETTINGS_UNSUPPORTED_VERSION);
    assert(device_settings_equal(&output, &sentinel));
}

static void bluetooth_name_validation(void)
{
    const char *const valid[] = {"A", "ESP32-INPUT", "Desk Pad", "ABCDEFGHIJKLMNOPQR",
        "\xc3\xa9", "\xc2\xa0", "\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\x41\x42"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i)
        assert(device_settings_bluetooth_name_valid(valid[i], strlen(valid[i])));
    const char *const invalid[] = {"", " ", " Leading", "Trailing ", "ABCDEFGHIJKLMNOPQRS",
        "A\nB", "\x7f", "\xc2\x80", "\xc2\x9f", "\x80", "\xc0\xaf", "\xc2",
        "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        assert(!device_settings_bluetooth_name_valid(invalid[i], strlen(invalid[i])));
    assert(!device_settings_bluetooth_name_valid(NULL, 1));
    assert(!device_settings_bluetooth_name_valid("A", SIZE_MAX));
    assert(!device_settings_bluetooth_name_valid("A\0B", 3));
    const char exact[3] = {'A','B','C'};
    assert(device_settings_bluetooth_name_valid(exact, sizeof exact));
    device_settings_t settings;
    device_settings_defaults(&settings);
    memset(settings.bluetooth_name, 'A', sizeof settings.bluetooth_name);
    assert(!device_settings_validate(&settings));
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
        memset(settings.bluetooth_name, 0, sizeof settings.bluetooth_name);
        memcpy(settings.bluetooth_name, valid[i], strlen(valid[i]));
        uint8_t record[DEVICE_SETTINGS_RECORD_SIZE]; device_settings_t decoded;
        assert(device_settings_encode(&settings, record, sizeof record) == DEVICE_SETTINGS_OK);
        assert(device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
        assert(device_settings_equal(&settings, &decoded));
    }
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

static const uint8_t v3_tap_off[40] = {
    0x44,0x53,0x45,0x54,0x03,0x28,0x01,0x00,
    0x01,0x32,0x05,0x00,0x00,0x0b,0x45,0x53,
    0x50,0x33,0x32,0x2d,0x49,0x4e,0x50,0x55,
    0x54,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2b,0x9c,0x05,0x26
};

static void motion_preferences_and_v3_compatibility(void)
{
    device_settings_t defaults, value, decoded;
    device_settings_defaults(&defaults);
    assert(defaults.motion_gain_x_centi == 400 && defaults.motion_gain_y_centi == 400);
    assert(defaults.motion_deadzone_mdps == 500 && defaults.motion_filter_ms == 30);
    assert(defaults.motion_freeze_ms == 150 && !defaults.motion_invert_x && !defaults.motion_invert_y);
    assert(device_settings_decode(v3_tap_off, sizeof v3_tap_off, &decoded) == DEVICE_SETTINGS_OK);
    value = defaults; value.tap_to_click = false;
    assert(device_settings_equal(&decoded, &value));
    for (size_t byte = 0; byte < sizeof v3_tap_off; ++byte)
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint8_t corrupt[40]; memcpy(corrupt, v3_tap_off, sizeof corrupt);
            corrupt[byte] ^= (uint8_t)(1U << bit);
            decoded = value;
            assert(device_settings_decode(corrupt, sizeof corrupt, &decoded) != DEVICE_SETTINGS_OK);
            assert(device_settings_equal(&decoded, &value));
        }
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned gain = 0; gain <= 10001; ++gain) {
            value = defaults;
            if (axis) value.motion_gain_y_centi = (uint16_t)gain;
            else value.motion_gain_x_centi = (uint16_t)gain;
            assert(device_settings_validate(&value) == (gain >= 25 && gain <= 10000));
        }
    for (unsigned field = 0; field < 7; ++field) {
        value = defaults;
        if (field == 0) value.motion_gain_x_centi = 25;
        if (field == 1) value.motion_gain_y_centi = 10000;
        if (field == 2) value.motion_deadzone_mdps = 0;
        if (field == 3) value.motion_filter_ms = 1000;
        if (field == 4) value.motion_freeze_ms = 1000;
        if (field == 5) value.motion_invert_x = true;
        if (field == 6) value.motion_invert_y = true;
        assert(!device_settings_equal(&value, &defaults));
        uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
        assert(device_settings_encode(&value, record, sizeof record) == DEVICE_SETTINGS_OK);
        assert(device_settings_decode(record, sizeof record, &decoded) == DEVICE_SETTINGS_OK);
        assert(device_settings_equal(&decoded, &value));
        device_settings_t applied = defaults;
        assert(device_settings_apply(&applied, &value) == DEVICE_SETTINGS_APPLY_CHANGED);
        assert(device_settings_apply(&applied, &value) == DEVICE_SETTINGS_APPLY_UNCHANGED);
    }
    for (unsigned field = 0; field < 3; ++field) {
        value = defaults;
        if (field == 0) value.motion_deadzone_mdps = 10001;
        if (field == 1) value.motion_filter_ms = 1001;
        if (field == 2) value.motion_freeze_ms = 1001;
        assert(!device_settings_validate(&value));
    }
}

static void explicit_sleep_preserves_fresh_gesture_gate(void)
{
    device_idle_t p;
    device_idle_init(&p, 1000);
    assert(!device_idle_request_sleep(NULL, 1000));
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 1, 1001) == DEVICE_INPUT_PASS);
    assert(!device_idle_request_sleep(&p, 1002) && !p.asleep);
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 1, 1003) == DEVICE_INPUT_PASS);
    uint64_t activity = p.last_activity_ms;
    assert(device_idle_request_sleep(&p, 1004) && p.asleep && p.last_activity_ms == activity);
    assert(device_idle_request_sleep(&p, 1005));
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 1, 1006) != DEVICE_INPUT_PASS && p.asleep);
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 2, 1007) == DEVICE_INPUT_CONSUME && !p.asleep);
    assert(!device_idle_request_sleep(&p, 1008));
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 2, 1009) == DEVICE_INPUT_CONSUME);
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_BEGIN, 3, 1010) == DEVICE_INPUT_PASS);
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_END, 3, 1011) == DEVICE_INPUT_PASS);
    assert(!device_idle_request_sleep(&p, 900) && !p.asleep && p.clock_regressed);
    assert(device_idle_request_sleep(&p, 901));
    assert(device_idle_input(&p, DEVICE_INPUT_TOUCH, DEVICE_INPUT_ACTION, 3, 902) != DEVICE_INPUT_PASS);
}

int main(void)
{
    explicit_sleep_preserves_fresh_gesture_gate();
    settings_codec_and_changes();
    rejected_records_and_values();
    bluetooth_name_validation();
    motion_preferences_and_v3_compatibility();
    idle_boundaries_and_configuration();
    physical_activity_without_queue_edges();
    touch_wake_and_delayed_callbacks();
    ai_wake_double_hold_and_multiple_inputs();
    malformed_events_and_clock_boundaries();
    puts("PASS: settings codec, validation, change detection, idle deadlines and whole-gesture wake routing");
    return 0;
}
