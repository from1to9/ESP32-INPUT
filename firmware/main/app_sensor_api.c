// SPDX-License-Identifier: Apache-2.0
#include "app_sensor_api.h"
#include "app_build_identity.h"
#include "board_port.h"
#include "sensor_wire.h"
#include "control_json.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* One shared bounded encoder workspace; contenders receive BUSY, never wait
 * while holding a board lock or reserve a large network/coordinator stack. */
static EXT_RAM_BSS_ATTR struct {
    board_sensor_record_t records[BOARD_SENSOR_JOURNAL_READ_MAX];
    board_sensor_batch_t page;
    board_imu_status_t imu;
    board_power_status_t power;
    board_haptic_status_t haptics;
    uint8_t raw[APP_SENSOR_BATCH_MAX_BYTES];
    char base64[1021], board_name[160], temperature[16], percent[16], voltage[16], current[16];
} s_work;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized, s_busy, s_elf_known;
static uint8_t s_boot[16];
static char s_boot_hex[33], s_elf_hex[65];
/* Only the existing LVGL contact callback accesses this change detector. */
static board_sensor_record_t s_last_touch;
static bool s_touch_seen;

static void hex_bytes(const uint8_t *data, size_t count, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = hex[data[i] >> 4]; out[i * 2 + 1] = hex[data[i] & 15U];
    }
    out[count * 2] = 0;
}

void app_sensor_api_init(void)
{
    /* Startup-only, before reader workers. Repeated calls must not replace a
     * boot identity or reset a reader's journal. This is not an auth secret. */
    if (s_initialized) return;
    esp_fill_random(s_boot, sizeof(s_boot));
    hex_bytes(s_boot, sizeof(s_boot), s_boot_hex);
    const esp_app_desc_t *description = esp_app_get_description();
    s_elf_known = description != NULL;
    if (description) hex_bytes(description->app_elf_sha256, 32, s_elf_hex);
    s_initialized = true;
}

static cp_error_t claim(void)
{
    portENTER_CRITICAL(&s_lock);
    cp_error_t result = !s_initialized ? CP_UNAVAILABLE : s_busy ? CP_BUSY : CP_OK;
    if (result == CP_OK) s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

static void release(void)
{
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool hex_boot(const char *boot)
{
    if (!boot || strlen(boot) != 32) return false;
    for (size_t i = 0; i < 32; ++i)
        if (!((boot[i] >= '0' && boot[i] <= '9') || (boot[i] >= 'a' && boot[i] <= 'f'))) return false;
    return true;
}

static cp_error_t read_page(const char *boot, uint64_t after, unsigned limit,
                            uint64_t request_id, uint8_t *raw, size_t capacity, size_t *length)
{
    if (!hex_boot(boot) || !limit || limit > APP_SENSOR_HTTP_MAX_RECORDS) return CP_INVALID;
    if (strcmp(boot, s_boot_hex)) return CP_STALE;
    esp_err_t error = board_sensor_journal_read(after, s_work.records, limit, &s_work.page);
    if (error != ESP_OK) return error == ESP_ERR_INVALID_ARG ? CP_INVALID : CP_UNAVAILABLE;
    size_t encoded = sensor_wire_encode(s_boot, request_id, after, &s_work.page, s_work.records, raw, capacity);
    if (!encoded) return CP_INTERNAL;
    *length = encoded;
    return CP_OK;
}

cp_error_t app_sensor_api_read_batch(const char boot_hex[33], uint64_t after,
    unsigned limit, uint64_t request_id, uint8_t *raw, size_t capacity, size_t *length)
{
    if (!length) return CP_INVALID;
    *length = 0;
    if (!raw) return CP_INVALID;
    cp_error_t result = claim();
    if (result != CP_OK) return result;
    result = read_page(boot_hex, after, limit, request_id, raw, capacity, length);
    release();
    return result;
}

static bool decimal(const char **cursor, uint64_t *value, char delimiter)
{
    const char *p = *cursor;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] != delimiter)) return false;
    uint64_t result = 0;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    if (*p != delimiter) return false;
    *cursor = p;
    *value = result;
    return true;
}

static cp_error_t catalog(char *json, size_t capacity, size_t *length)
{
    board_capabilities_t caps = board_capabilities();
    if (!cj_escape(caps.name ? caps.name : "Unknown", s_work.board_name, sizeof(s_work.board_name))) return CP_INTERNAL;
    int n = snprintf(json, capacity,
        "{\"schema\":1,\"boot\":\"%s\",\"board\":%s,\"git_commit\":\"%s\",\"git_state\":\"%s\","
        "\"elf_sha256\":%s%s%s,\"config_sha256\":%s%s%s,\"wire_version\":%u,\"record_bytes\":88,\"capacity\":256,"
        "\"max_http_records\":8,\"max_usb_records\":3,\"clock\":\"monotonic_microseconds\","
        "\"source_time\":\"acquisition_completion_not_conversion\",\"sources\":["
        "{\"kind\":1,\"name\":\"imu\",\"available\":%s,\"nominal_hz\":100,"
        "\"axes\":\"display_x_right_y_top_z_out\",\"units\":[\"dps\",\"dps\",\"dps\",\"g\",\"g\",\"g\"],\"ranges\":[2000,4]},"
        "{\"kind\":2,\"name\":\"imu_attempt\",\"cadence\":\"rejected_acquisitions\"},"
        "{\"kind\":3,\"name\":\"power\",\"available\":%s,\"nominal_hz\":1,\"stale_after_ms\":5000,"
        "\"units\":[\"gauge_celsius\",\"mA\",\"mV\",\"percent\"],\"external_power\":\"unknown\"},"
        "{\"kind\":4,\"name\":\"touch\",\"available\":%s,\"cadence\":\"observed_changes\","
        "\"units\":\"display_pixels\",\"private_coordinates\":\"redacted\"},"
        "{\"kind\":5,\"name\":\"ai_button\",\"available\":%s,\"cadence\":\"debounced_edges\"}],"
        "\"unavailable\":[\"magnetometer\",\"imu_die_temperature\",\"mcu_temperature\"],"
        "\"microphone\":\"separate_authorized_media_api\"}",
        s_boot_hex, s_work.board_name, APP_BUILD_GIT_COMMIT, APP_BUILD_GIT_STATE,
        s_elf_known ? "\"" : "", s_elf_known ? s_elf_hex : "null", s_elf_known ? "\"" : "",
        APP_BUILD_CONFIG_KNOWN ? "\"" : "", APP_BUILD_CONFIG_KNOWN ? APP_BUILD_CONFIG_SHA256 : "null",
        APP_BUILD_CONFIG_KNOWN ? "\"" : "", SENSOR_WIRE_VERSION, caps.imu ? "true" : "false", caps.battery ? "true" : "false",
        caps.touch ? "true" : "false", caps.lock_button ? "true" : "false");
    if (n < 0 || (size_t)n >= capacity) return CP_INTERNAL;
    *length = (size_t)n;
    return CP_OK;
}

static cp_error_t latest(char *json, size_t capacity, size_t *length)
{
    s_work.imu = board_imu_status();
    esp_err_t power_error = board_power_observe(&s_work.power);
    s_work.haptics = board_haptics_status();
    esp_err_t journal_error = board_sensor_journal_status(&s_work.page);
    if (journal_error != ESP_OK) return CP_UNAVAILABLE;
    const board_power_status_t *p = &s_work.power;
    if (p->percent_valid) (void)snprintf(s_work.percent, sizeof(s_work.percent), "%u", p->percent);
    else strcpy(s_work.percent, "null");
    if (p->voltage_valid) (void)snprintf(s_work.voltage, sizeof(s_work.voltage), "%u", p->voltage_mv);
    else strcpy(s_work.voltage, "null");
    if (p->current_valid) (void)snprintf(s_work.current, sizeof(s_work.current), "%d", p->current_ma);
    else strcpy(s_work.current, "null");
    if (p->temperature_valid) {
        uint32_t bits;
        memcpy(&bits, &p->temperature_c, sizeof(bits));
        (void)snprintf(s_work.temperature, sizeof(s_work.temperature), "\"%08" PRIx32 "\"", bits);
    } else strcpy(s_work.temperature, "null");
    const board_imu_status_t *imu = &s_work.imu;
    int n = snprintf(json, capacity,
        "{\"schema\":1,\"boot\":\"%s\",\"journal\":{\"state\":%u,\"oldest\":\"%" PRIu64 "\","
        "\"latest\":\"%" PRIu64 "\",\"observed_us\":\"%" PRIu64 "\"},"
        "\"imu\":{\"available\":%s,\"fresh\":%s,\"phase\":%u,\"error\":%d,\"samples\":\"%" PRIu64 "\","
        "\"sample_us\":\"%" PRIu64 "\",\"age_known\":%s,\"age_us\":\"%" PRIu64 "\"},"
        "\"power\":{\"error\":%d,\"sample_us\":\"%" PRIu64 "\",\"gauge_available\":%s,"
        "\"percent\":%s,\"voltage_mv\":%s,\"current_ma\":%s,\"temperature_c_bits\":%s,"
        "\"battery_present\":%u,\"external_power\":%u,\"charging\":%u,\"full\":%u},"
        "\"haptics\":{\"state\":%u,\"error\":%d,\"on_confirmed\":%s,\"off_confirmed\":%s},"
        "\"input\":{\"touch\":\"redacted_change_events\",\"button\":\"debounced_edges\"}}",
        s_boot_hex, (unsigned)s_work.page.journal_state, s_work.page.oldest_sequence,
        s_work.page.latest_sequence, s_work.page.observed_us, imu->available ? "true" : "false",
        imu->sample_fresh ? "true" : "false", (unsigned)imu->phase, (int)imu->error, imu->sample_count,
        imu->last_sample_us, imu->sample_age_known ? "true" : "false", imu->sample_age_us,
        (int)power_error, p->timestamp_us, p->gauge_available ? "true" : "false",
        s_work.percent, s_work.voltage, s_work.current, s_work.temperature,
        (unsigned)p->battery_present, (unsigned)p->external_power, (unsigned)p->charging, (unsigned)p->full,
        (unsigned)s_work.haptics.state, (int)s_work.haptics.error,
        s_work.haptics.on_confirmed ? "true" : "false", s_work.haptics.off_confirmed ? "true" : "false");
    if (n < 0 || (size_t)n >= capacity) return CP_INTERNAL;
    *length = (size_t)n;
    return CP_OK;
}

cp_error_t app_sensor_api_read(const char *resource, char *json, size_t capacity, size_t *length)
{
    if (!length) return CP_INVALID;
    *length = 0;
    if (!resource || !json || !capacity) return CP_INVALID;
    json[0] = 0;
    cp_error_t result = claim();
    if (result != CP_OK) return result;
    if (!strcmp(resource, "/api/v1/sensors/catalog")) result = catalog(json, capacity, length);
    else if (!strcmp(resource, "/api/v1/sensors/latest")) result = latest(json, capacity, length);
    else {
        const char prefix[] = "/api/v1/sensors/samples?boot=";
        size_t offset = sizeof(prefix) - 1U;
        result = CP_INVALID;
        if (strncmp(resource, prefix, offset) || strlen(resource + offset) < 32U + 7U) goto done;
        char boot[33];
        memcpy(boot, resource + offset, 32); boot[32] = 0;
        const char *p = resource + offset + 32;
        if (strncmp(p, "&after=", 7)) goto done;
        p += 7;
        uint64_t after, count;
        if (!decimal(&p, &after, '&') || strncmp(p, "&limit=", 7)) goto done;
        p += 7;
        if (!decimal(&p, &count, 0) || !count || count > APP_SENSOR_HTTP_MAX_RECORDS) goto done;
        size_t raw_length = 0;
        result = read_page(boot, after, (unsigned)count, 0, s_work.raw, sizeof(s_work.raw), &raw_length);
        if (result != CP_OK) goto done;
        if (!sensor_wire_base64(s_work.raw, raw_length, s_work.base64, sizeof(s_work.base64))) { result = CP_INTERNAL; goto done; }
        int n = snprintf(json, capacity, "{\"schema\":1,\"encoding\":\"stj2-le-b64\",\"data\":\"%s\"}", s_work.base64);
        if (n < 0 || (size_t)n >= capacity) { result = CP_INTERNAL; goto done; }
        *length = (size_t)n;
    }
done:
    if (result != CP_OK) { *length = 0; json[0] = 0; }
    release();
    return result;
}

void app_sensor_api_touch(const board_touch_snapshot_t *sample, bool public_coordinates)
{
    if (!sample || sample->count > BOARD_TOUCH_CONTACTS) return;
    board_sensor_record_t r = {.kind = BOARD_SENSOR_TOUCH};
    if (sample->timestamp_ms <= UINT64_MAX / 1000U) {
        r.sample_us = sample->timestamp_ms * 1000U;
        r.flags |= BOARD_SENSOR_FLAG_SAMPLE_TIME;
    }
    if (sample->sequence) { r.source_sequence = sample->sequence; r.flags |= BOARD_SENSOR_FLAG_SOURCE_SEQ; }
    /* Invalid/overflow snapshots may contain old coordinates. Never publish
     * them even when the current visible page is the public pad. */
    if (public_coordinates && sample->valid && !sample->overflow) {
        r.validity = BOARD_SENSOR_VALID_INPUT;
        r.payload[0] = sample->count;
        r.payload[1] = (sample->valid ? 1U : 0U) | (sample->overflow ? 2U : 0U);
        for (unsigned i = 0; i < sample->count; ++i) {
            r.payload[2 + i * 3] = sample->contacts[i].id;
            r.payload[3 + i * 3] = sample->contacts[i].x;
            r.payload[4 + i * 3] = sample->contacts[i].y;
        }
    } else r.flags |= BOARD_SENSOR_FLAG_REDACTED;
    if (s_touch_seen && r.flags == s_last_touch.flags && r.validity == s_last_touch.validity &&
        !memcmp(r.payload, s_last_touch.payload, sizeof(r.payload))) return;
    if (board_sensor_journal_publish_input(&r) == ESP_OK) { s_last_touch = r; s_touch_seen = true; }
}

void app_sensor_api_button(bool down, uint64_t timestamp_us)
{
    board_sensor_record_t r = {.kind = BOARD_SENSOR_BUTTON, .sample_us = timestamp_us,
        .flags = BOARD_SENSOR_FLAG_SAMPLE_TIME, .validity = BOARD_SENSOR_VALID_INPUT};
    r.payload[0] = down ? 0U : 1U;
    (void)board_sensor_journal_publish_input(&r);
}
