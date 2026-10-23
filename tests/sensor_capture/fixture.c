// SPDX-License-Identifier: Apache-2.0
/* Named cached-board/SDK boundaries; encoders and USB framing are production. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_build_identity.h"
#if defined(FIXTURE_ARCHIVE)
/* Named build-metadata boundary for an actual archive/unknown identity. */
#undef APP_BUILD_GIT_COMMIT
#undef APP_BUILD_GIT_STATE
#define APP_BUILD_GIT_COMMIT ""
#define APP_BUILD_GIT_STATE "unknown"
#endif
#include "../../firmware/main/app_sensor_api.c"
#include "control_sensor_usb.h"

static board_sensor_journal_t fixture_journal;
static board_sensor_record_t fixture_storage[BOARD_SENSOR_JOURNAL_CAPACITY];
static bool unavailable;
static bool partial;
static bool zero_epoch, clock_failure;
static unsigned writes;
static esp_app_desc_t description;

void esp_fill_random(void *data, size_t n)
{ for (size_t i = 0; i < n; ++i) ((uint8_t *)data)[i] = (uint8_t)(i + 1U); }
const esp_app_desc_t *esp_app_get_description(void) { return &description; }
board_capabilities_t board_capabilities(void)
{ return (board_capabilities_t){.name = "Collector fixture", .imu = true, .battery = true, .touch = true, .lock_button = true}; }
board_imu_status_t board_imu_status(void) { return (board_imu_status_t){0}; }
board_haptic_status_t board_haptics_status(void) { return (board_haptic_status_t){.state = BOARD_HAPTIC_NONE}; }
esp_err_t board_power_observe(board_power_status_t *out)
{ *out = (board_power_status_t){.error = ESP_ERR_INVALID_STATE}; return out->error; }
esp_err_t board_power_read(board_power_status_t *out) { (void)out; abort(); }
esp_err_t board_sensor_journal_read(uint64_t after, board_sensor_record_t *records, size_t count, board_sensor_batch_t *page)
{
    if (unavailable) {
        if (after) return ESP_ERR_INVALID_ARG;
        *page = (board_sensor_batch_t){.state = BOARD_SENSOR_READ_UNAVAILABLE};
        return ESP_OK;
    }
    return board_sensor_journal_copy(&fixture_journal, after, records, count, 100000, page) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t board_sensor_journal_status(board_sensor_batch_t *page)
{
    board_sensor_record_t record;
    return board_sensor_journal_read(unavailable ? 0 : fixture_journal.latest_sequence, &record, 1, page);
}
esp_err_t board_sensor_journal_publish_input(const board_sensor_record_t *record)
{ (void)record; abort(); }

const char *cp_error_name(cp_error_t error)
{
    switch (error) {
    case CP_OK: return "ok";
    case CP_INVALID: return "invalid";
    case CP_STALE: return "stale";
    case CP_UNAVAILABLE: return "unavailable";
    case CP_BUSY: return "busy";
    default: return "internal";
    }
}
static bool allowed(void *user) { (void)user; return true; }
static cp_error_t read_resource(void *user, const char *path, char *out, size_t cap, size_t *n)
{ (void)user; return app_sensor_api_read(path, out, cap, n); }
static cp_error_t read_batch(void *user, const char *boot, uint64_t after, unsigned limit,
                              uint64_t rid, uint8_t *out, size_t cap, size_t *n)
{ (void)user; return app_sensor_api_read_batch(boot, after, limit, rid, out, cap, n); }
static size_t wire_write(void *user, const char *data, size_t n)
{
    (void)user;
    size_t count = partial && !writes++ && n > 9 ? n - 9 : n;
    assert(fwrite(data, 1, count, stdout) == count);
    return count;
}

static void append(unsigned number)
{
    board_sensor_record_t r = {.sample_us = 1000U + number * 10000U,
        .observed_us = 1100U + number * 10000U,
        .flags = BOARD_SENSOR_FLAG_SAMPLE_TIME | BOARD_SENSOR_FLAG_OBSERVED_TIME};
    switch (number) {
    case 1:
    case 7:
        r.kind = BOARD_SENSOR_IMU_SAMPLE;
        r.source_sequence = number == 1 ? 1 : 2;
        r.source_epoch = 9;
        r.flags |= BOARD_SENSOR_FLAG_SOURCE_SEQ | BOARD_SENSOR_FLAG_SOURCE_EPOCH;
        r.validity = BOARD_SENSOR_VALID_IMU_AXES | BOARD_SENSOR_VALID_READ_START;
        r.payload[0] = UINT32_C(0x80000000);
        r.payload[1] = number == 1 ? UINT32_C(0x7fc00123) : 0;
        r.payload[2] = UINT32_C(0x3f800000);
        r.payload[5] = UINT32_C(0x3f800000);
        r.payload[6] = 500U + number * 10000U;
        break;
    case 2:
        r.kind = BOARD_SENSOR_IMU_REJECTED; r.error = -4;
        r.discontinuities = BOARD_IMU_DISCONTINUITY_REJECTED;
        r.validity = BOARD_SENSOR_VALID_READ_START | BOARD_SENSOR_VALID_READ_END |
            BOARD_SENSOR_VALID_DURATION | BOARD_SENSOR_VALID_SOURCE_ERROR;
        r.payload[0] = 100; r.payload[2] = 20200; r.payload[4] = 20100;
        r.payload[6] = 1U | 192U << 8 | BOARD_SENSOR_ATTEMPT_LATE << 16 | BOARD_SENSOR_STAGE_BATCH_TIME << 24;
        if (clock_failure) {
            r.payload[0] = 20200; r.payload[2] = 100; r.payload[4] = 0;
            r.validity &= ~(uint32_t)BOARD_SENSOR_VALID_DURATION;
            r.payload[6] = 1U | 192U << 8 | BOARD_SENSOR_ATTEMPT_FATAL << 16 | BOARD_SENSOR_STAGE_CLOCK << 24;
            r.discontinuities = BOARD_IMU_DISCONTINUITY_CLOCK;
        }
        break;
    case 3:
        r.kind = BOARD_SENSOR_POWER; r.validity = 31;
        r.payload[0] = UINT32_C(0x41cc0000); r.payload[1] = (uint32_t)-42;
        r.payload[2] = 3700; r.payload[3] = 61; r.payload[4] = 1; r.payload[5] = 82;
        break;
    case 4:
        r.kind = BOARD_SENSOR_TOUCH; r.validity = BOARD_SENSOR_VALID_INPUT;
        r.payload[0] = 1; r.payload[1] = 1; r.payload[2] = 7; r.payload[3] = 20; r.payload[4] = 30;
        break;
    case 5:
        r.kind = BOARD_SENSOR_BUTTON; r.validity = BOARD_SENSOR_VALID_INPUT;
        break;
    case 6:
        r.kind = BOARD_SENSOR_TOUCH; r.flags |= BOARD_SENSOR_FLAG_REDACTED;
        break;
    default:
        r.kind = BOARD_SENSOR_POWER;
        break;
    }
    if (zero_epoch && (r.flags & BOARD_SENSOR_FLAG_SOURCE_EPOCH)) r.source_epoch = 0;
    assert(board_sensor_journal_append(&fixture_journal, &r));
}

/* Persistent acquisition boundary: the journal, encoders and USB responder are
 * unchanged production code. Four records per 40 ms model a 100 Hz producer;
 * the slow-reader case advances sixteen per 160 ms, without pausing it. */
static void live_producer(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t sequence = fixture_journal.latest_sequence + 1U;
        board_sensor_record_t r = {.kind = BOARD_SENSOR_IMU_SAMPLE,
            .source_sequence = sequence, .source_epoch = 9,
            .sample_us = sequence * 10000U, .observed_us = sequence * 10000U + 100U,
            .flags = BOARD_SENSOR_FLAG_SOURCE_SEQ | BOARD_SENSOR_FLAG_SOURCE_EPOCH |
                BOARD_SENSOR_FLAG_SAMPLE_TIME | BOARD_SENSOR_FLAG_OBSERVED_TIME,
            .validity = BOARD_SENSOR_VALID_IMU_AXES, .payload = {0, 0, 0, 0, 0, UINT32_C(0x3f800000)}};
        assert(board_sensor_journal_append(&fixture_journal, &r));
    }
}
static void live_requests(unsigned advance)
{
    assert(board_sensor_journal_init(&fixture_journal, fixture_storage, BOARD_SENSOR_JOURNAL_CAPACITY));
    live_producer(300);
    cp_sensor_usb_t usb = {0};
    const cp_sensor_usb_port_t port = {.allowed = allowed, .read = read_resource, .batch = read_batch, .write = wire_write};
    char command[192]; uint64_t now = 1000;
    while (fgets(command, sizeof(command), stdin)) {
        command[strcspn(command, "\r\n")] = 0;
        if (!strncmp(command, "ST1 READ ", 9)) { live_producer(advance); now += advance * 10U; }
        else now += 20;
        assert(cp_sensor_usb_receive(&usb, command, now, &port));
        for (unsigned poll = 0; usb.pending && poll < 32; ++poll) cp_sensor_usb_service(&usb, now + poll * 20U, &port);
        assert(!usb.pending);
        assert(fflush(stdout) == 0);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    unavailable = !strcmp(argv[1], "unavailable");
    partial = !strcmp(argv[1], "partial");
    zero_epoch = !strcmp(argv[1], "zero_epoch");
    clock_failure = !strcmp(argv[1], "clock");
    if (!strcmp(argv[1], "image")) memset(description.app_elf_sha256, 17, sizeof description.app_elf_sha256);
    app_sensor_api_init();
    if (!strcmp(argv[1], "live") || !strcmp(argv[1], "live-slow")) {
        live_requests(!strcmp(argv[1], "live") ? 4U : 16U); return 0;
    }
    assert(board_sensor_journal_init(&fixture_journal, fixture_storage, !strcmp(argv[1], "gap") ? 4 : 8));
    if (!strcmp(argv[1], "max")) {
        fixture_journal.latest_sequence = UINT64_MAX - 2U;
        append(1); append(2);
    } else {
        for (unsigned i = 1; i <= 8; ++i) append(i);
    }
    if (!strcmp(argv[2], "usb")) {
        cp_sensor_usb_t usb = {0};
        const cp_sensor_usb_port_t port = {.allowed = allowed, .read = read_resource, .batch = read_batch, .write = wire_write};
        assert(cp_sensor_usb_receive(&usb, argv[3], 1000, &port));
        for (uint64_t now = 1000; usb.pending && now < 3000; now += 20) cp_sensor_usb_service(&usb, now, &port);
        assert(!usb.pending);
    } else {
        char body[4096]; size_t length = 0;
        cp_error_t error = app_sensor_api_read(argv[3], body, sizeof(body), &length);
        if (error == CP_OK) assert(fwrite(body, 1, length, stdout) == length);
        else (void)printf("{\"error\":\"%s\"}", cp_error_name(error));
    }
    return 0;
}
