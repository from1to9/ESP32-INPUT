// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_sensor_api.c"

static board_sensor_journal_t journal;
static board_sensor_record_t storage[256];
static unsigned random_calls, reads, power_observations;
static bool reenter;
static esp_app_desc_t description;
static cj_document_t document;

void esp_fill_random(void *data, size_t n)
{ ++random_calls; for (size_t i = 0; i < n; ++i) ((uint8_t *)data)[i] = (uint8_t)(i + 1U); }
const esp_app_desc_t *esp_app_get_description(void) { return &description; }
board_capabilities_t board_capabilities(void)
{ return (board_capabilities_t){.name = "A \"test\" board", .imu = true, .touch = true, .lock_button = true}; }
board_imu_status_t board_imu_status(void) { return (board_imu_status_t){.sample_age_known = false}; }
board_haptic_status_t board_haptics_status(void) { return (board_haptic_status_t){.state = BOARD_HAPTIC_NONE}; }
esp_err_t board_power_observe(board_power_status_t *out)
{ ++power_observations; *out = (board_power_status_t){.error = ESP_ERR_INVALID_STATE}; return out->error; }
esp_err_t board_power_read(board_power_status_t *out) { (void)out; abort(); }
esp_err_t board_sensor_journal_read(uint64_t after, board_sensor_record_t *records,
                                  size_t count, board_sensor_batch_t *page)
{
    ++reads;
    if (reenter) {
        char body[128]; size_t length = 123;
        assert(app_sensor_api_read("/api/v1/sensors/catalog", body, sizeof(body), &length) == CP_BUSY);
        assert(length == 0);
    }
    return board_sensor_journal_copy(&journal, after, records, count, 2000, page) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t board_sensor_journal_status(board_sensor_batch_t *page)
{
    board_sensor_record_t record;
    return board_sensor_journal_copy(&journal, journal.latest_sequence, &record, 1, 2000, page) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t board_sensor_journal_publish_input(const board_sensor_record_t *input)
{
    if (!board_sensor_input_valid(input)) return ESP_ERR_INVALID_ARG;
    board_sensor_record_t r = *input;
    r.observed_us = 2000;
    r.flags |= BOARD_SENSOR_FLAG_OBSERVED_TIME;
    return board_sensor_journal_append(&journal, &r) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static uint64_t number(const uint8_t *p, unsigned bytes)
{ uint64_t v = 0; for (unsigned i = 0; i < bytes; ++i) v |= (uint64_t)p[i] << (8U * i); return v; }
static board_sensor_record_t frame(unsigned i)
{
    board_sensor_record_t r = {.kind = BOARD_SENSOR_IMU_SAMPLE, .source_sequence = i + 1U,
        .source_epoch = 9, .sample_us = 1000U + i, .observed_us = 1100U + i,
        .flags = BOARD_SENSOR_FLAG_SOURCE_SEQ | BOARD_SENSOR_FLAG_SOURCE_EPOCH |
                 BOARD_SENSOR_FLAG_SAMPLE_TIME | BOARD_SENSOR_FLAG_OBSERVED_TIME,
        .validity = BOARD_SENSOR_VALID_IMU_AXES};
    r.payload[0] = UINT32_C(0x80000000); r.payload[1] = UINT32_C(0x7fc00123);
    r.payload[2] = UINT32_C(0x3f800000);
    return r;
}
static void request(char out[160], const char *boot, uint64_t after, unsigned limit)
{ (void)snprintf(out, 160, "/api/v1/sensors/samples?boot=%s&after=%" PRIu64 "&limit=%u", boot, after, limit); }

int main(void)
{
    char json[4096], path[160]; uint8_t raw[APP_SENSOR_BATCH_MAX_BYTES]; size_t length = 99;
    assert(app_sensor_api_read("/api/v1/sensors/catalog", json, sizeof(json), &length) == CP_UNAVAILABLE);
    assert(length == 0);
    app_sensor_api_init(); app_sensor_api_init(); assert(random_calls == 1);
    assert(board_sensor_journal_init(&journal, storage, 256));
    assert(app_sensor_api_read("/api/v1/sensors/catalog", json, sizeof(json), &length) == CP_OK);
    assert(cj_parse(&document, json, length));
    assert(strstr(json, "A \\\"test\\\" board") && strstr(json, "config_sha256"));
    assert(app_sensor_api_read("/api/v1/sensors/latest", json, sizeof(json), &length) == CP_OK);
    assert(cj_parse(&document, json, length));
    assert(strstr(json, "\"percent\":null") && strstr(json, "\"age_known\":false") && power_observations == 1);
    for (unsigned i = 0; i < 12; ++i) { board_sensor_record_t r = frame(i); assert(board_sensor_journal_append(&journal, &r)); }
    reenter = true;
    assert(app_sensor_api_read_batch(s_boot_hex, 0, 8, UINT64_MAX, raw, sizeof(raw), &length) == CP_OK);
    assert(length == 764 && number(raw, 1) == 1 && raw[1] == 8 && number(raw + 2, 2) == 88);
    assert(number(raw + 4, 4) == SENSOR_PAGE_MORE && number(raw + 8, 8) == UINT64_MAX);
    assert(!memcmp(raw + 16, s_boot, 16) && number(raw + 40, 8) == 1 && number(raw + 48, 8) == 12);
    assert(number(raw + 56, 8) == 1 && number(raw + 112, 4) == UINT32_C(0x80000000));
    assert(number(raw + 116, 4) == UINT32_C(0x7fc00123));
    assert(number(raw + length - 4, 4) == sensor_wire_crc32(raw, length - 4));
    assert(sensor_wire_crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    uint8_t same[APP_SENSOR_BATCH_MAX_BYTES]; size_t same_length;
    assert(app_sensor_api_read_batch(s_boot_hex, 0, 8, UINT64_MAX, same, sizeof(same), &same_length) == CP_OK);
    assert(length == same_length && !memcmp(raw, same, length));
    request(path, s_boot_hex, 0, 8);
    assert(app_sensor_api_read(path, json, sizeof(json), &length) == CP_OK && length < 1152);
    assert(cj_parse(&document, json, length));
    char tiny[16]; memset(tiny, 'x', sizeof(tiny));
    assert(app_sensor_api_read(path, tiny, sizeof(tiny), &length) == CP_INTERNAL && length == 0 && !tiny[0]);
    unsigned before = reads;
    assert(app_sensor_api_read_batch("00000000000000000000000000000000", 0, 1, 1, raw, sizeof(raw), &length) == CP_STALE);
    assert(reads == before && length == 0);
    assert(app_sensor_api_read_batch(s_boot_hex, 13, 1, 1, raw, sizeof(raw), &length) == CP_INVALID);
    const char *bad[] = {"/api/v1/sensors/latest?x=1", "/api/v1/sensors/catalog/", "/api/v1/sensors/samples",
        "/api/v1/sensors/samples?boot=x&after=0&limit=1"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(app_sensor_api_read(bad[i], json, sizeof(json), &length) == CP_INVALID && length == 0);
    const char *tails[] = {"after=00&limit=1", "after=18446744073709551616&limit=1", "after=0&limit=9",
        "after=0&limit=1&x=1", "limit=1&after=0", "after=-1&limit=1", "after=0&limit=01"};
    for (unsigned i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
        (void)snprintf(path, sizeof(path), "/api/v1/sensors/samples?boot=%s&%s", s_boot_hex, tails[i]);
        assert(app_sensor_api_read(path, json, sizeof(json), &length) == CP_INVALID && length == 0);
    }
    for (unsigned i = 12; i < 300; ++i) { board_sensor_record_t r = frame(i); assert(board_sensor_journal_append(&journal, &r)); }
    assert(app_sensor_api_read_batch(s_boot_hex, 0, 3, 7, raw, sizeof(raw), &length) == CP_OK);
    assert(length == 60 && raw[1] == 0 && number(raw + 4, 4) == (SENSOR_PAGE_GAP | SENSOR_PAGE_MORE));
    assert(number(raw + 40, 8) == 45 && number(raw + 48, 8) == 300);
    assert(app_sensor_api_read_batch(s_boot_hex, 44, 3, 7, raw, sizeof(raw), &length) == CP_OK && raw[1] == 3);
    reenter = false;
    uint64_t previous = journal.latest_sequence;
    board_touch_snapshot_t touch = {.sequence = 1, .timestamp_ms = 1, .count = 1, .valid = true,
        .contacts = {{.id = 1, .x = 210, .y = 200}}};
    app_sensor_api_touch(&touch, true); assert(journal.latest_sequence == previous + 1);
    ++touch.sequence; ++touch.timestamp_ms;
    app_sensor_api_touch(&touch, true); assert(journal.latest_sequence == previous + 1);
    app_sensor_api_touch(&touch, false); assert(journal.latest_sequence == previous + 2);
    board_sensor_batch_t page; board_sensor_record_t out[8];
    assert(board_sensor_journal_read(previous + 1, out, 8, &page) == ESP_OK);
    assert(page.count == 1 && (out[0].flags & BOARD_SENSOR_FLAG_REDACTED) && !out[0].validity);
    for (unsigned i = 0; i < 8; ++i) assert(!out[0].payload[i]);
    touch.contacts[0].x = 123;
    app_sensor_api_touch(&touch, false); assert(journal.latest_sequence == previous + 2);
    app_sensor_api_button(true, 3000); app_sensor_api_button(false, 4000);
    assert(journal.latest_sequence == previous + 4);
    for (unsigned invalid = 0; invalid < 2; ++invalid) {
        touch.valid = true; touch.overflow = false;
        app_sensor_api_touch(&touch, true);
        previous = journal.latest_sequence;
        touch.valid = invalid != 0; touch.overflow = invalid != 0;
        app_sensor_api_touch(&touch, true);
        assert(board_sensor_journal_read(previous, out, 8, &page) == ESP_OK && page.count == 1);
        assert((out[0].flags & BOARD_SENSOR_FLAG_REDACTED) && !out[0].validity);
        for (unsigned i = 0; i < 8; ++i) assert(!out[0].payload[i]);
    }
    /* A contradictory provider page must not erase unavailable state or
     * appear as a healthy empty page. Rejection leaves the output untouched. */
    page = (board_sensor_batch_t){.state = BOARD_SENSOR_READ_UNAVAILABLE,
        .journal_state = BOARD_SENSOR_JOURNAL_UNAVAILABLE};
    assert(sensor_wire_encode(s_boot, 1, 0, &page, NULL, raw, sizeof(raw)) == 60);
    assert(number(raw + 4, 4) == SENSOR_PAGE_UNAVAILABLE);
    page.state = BOARD_SENSOR_READ_EMPTY;
    memset(raw, 0xa5, sizeof(raw));
    assert(!sensor_wire_encode(s_boot, 1, 0, &page, NULL, raw, sizeof(raw)));
    assert(raw[0] == 0xa5);
    page = (board_sensor_batch_t){.state = BOARD_SENSOR_READ_GAP,
        .journal_state = BOARD_SENSOR_JOURNAL_UNAVAILABLE, .oldest_sequence = 2,
        .latest_sequence = 2, .next_after = 1, .lost_records = 1};
    assert(!sensor_wire_encode(s_boot, 1, 0, &page, NULL, raw, sizeof(raw)));
    page = (board_sensor_batch_t){.state = BOARD_SENSOR_READ_EMPTY,
        .journal_state = BOARD_SENSOR_JOURNAL_EXHAUSTED, .oldest_sequence = UINT64_MAX,
        .latest_sequence = UINT64_MAX, .next_after = UINT64_MAX};
    assert(sensor_wire_encode(s_boot, 1, UINT64_MAX, &page, NULL, raw, sizeof(raw)) == 60);
    assert(number(raw + 4, 4) == SENSOR_PAGE_EXHAUSTED);
    page.oldest_sequence = 0;
    assert(!sensor_wire_encode(s_boot, 1, UINT64_MAX, &page, NULL, raw, sizeof(raw)));
    char b64[9]; assert(sensor_wire_base64((const uint8_t *)"foobar", 6, b64, sizeof(b64)) && !strcmp(b64, "Zm9vYmFy"));
    assert(sensor_wire_base64((const uint8_t *)"f", 1, b64, sizeof(b64)) && !strcmp(b64, "Zg=="));
    assert(sensor_wire_base64((const uint8_t *)"fo", 2, b64, sizeof(b64)) && !strcmp(b64, "Zm8="));
    assert(!sensor_wire_base64((const uint8_t *)"foo", 3, b64, 4));
    puts("PASS: production sensor API, binary codec, immutable reads, gap evidence and private-touch redaction");
    return 0;
}
