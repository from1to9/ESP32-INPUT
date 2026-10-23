// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app_control_runtime.h"
#include "control_sensor_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_hid.h"
/* TYPES */
/* PORTS */
static uint64_t time_ms = 100;
static uint64_t now_ms(void) { return time_ms; }
static bool mounted = true, window;
static unsigned staged, json_reads, raw_reads, writes;
static char input[1024], output[16384];
static size_t input_length, input_at, output_length, short_write = SIZE_MAX;
static ssize_t test_read(int fd, void *bytes, size_t capacity)
{
    assert(fd == STDIN_FILENO);
    size_t n = input_length - input_at;
    if (n > capacity) n = capacity;
    memcpy(bytes, input + input_at, n); input_at += n; return (ssize_t)n;
}
static ssize_t test_write(int fd, const void *bytes, size_t count)
{
    assert(fd == STDOUT_FILENO); ++writes;
    if (count > short_write) count = short_write;
    assert(output_length + count < sizeof(output));
    memcpy(output + output_length, bytes, count); output_length += count; output[output_length] = 0;
    return (ssize_t)count;
}
static void wipe(void *p, size_t n) { memset(p, 0, n); }
usb_hid_snapshot_t usb_hid_snapshot(void) { return (usb_hid_snapshot_t){.mounted = mounted}; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out)
{ *out = (app_control_trust_snapshot_t){.known = true, .import_window = window}; }
esp_err_t app_control_runtime_stage_commissioning(const char *json, size_t length)
{ (void)json; (void)length; ++staged; return ESP_OK; }
cp_error_t app_sensor_api_read(const char *resource, char *out, size_t capacity, size_t *length)
{
    assert(!strcmp(resource, "/api/v1/sensors/catalog") || !strcmp(resource, "/api/v1/sensors/latest"));
    assert(capacity > 11); memcpy(out, "{\"known\":1}", 12); *length = 11; ++json_reads; return CP_OK;
}
cp_error_t app_sensor_api_read_batch(const char boot[33], uint64_t after, unsigned limit, uint64_t id,
    uint8_t *raw, size_t capacity, size_t *length)
{
    (void)boot; (void)after; (void)limit; (void)id;
    assert(capacity == 324); memset(raw, 0x5a, capacity); *length = capacity; ++raw_reads; return CP_OK;
}
#define read(...) test_read(__VA_ARGS__)
#define write(...) test_write(__VA_ARGS__)
/* PRODUCTION */
/* TESTS */
static runtime_t storage;
static void submit_line(const char *line)
{
    input_at = 0; input_length = strlen(line); assert(input_length < sizeof(input)); memcpy(input, line, input_length);
    do { size_t before = input_at; poll_usb(window); time_ms += 20; if (input_at == before) break; } while (input_at < input_length);
}
int main(void)
{
    runtime = &storage; storage.usb_attached = true; storage.state_known = true;
    submit_line("CP1 CHALLENGE\n"); assert(!output_length && !staged);
    submit_line("ST1 CATALOG 1111111111111111\n");
    assert(json_reads == 1 && strstr(output, "ST1 END ") && !strstr(output, "TOKEN"));
    output_length = 0; output[0] = 0; window = true;
    submit_line("ST1 CATALOG 2222222222222222\n"); assert(json_reads == 1 && !output_length);
    submit_line("CP1 CHALLENGE\n"); assert(strstr(output, "CP1 TOKEN ") && !staged);
    output_length = 0; output[0] = 0; storage.state.locked = true;
    submit_line("CP1 CHALLENGE\nST1 LATEST 3333333333333333\n"); assert(!output_length && json_reads == 1);
    window = false; storage.state.locked = false; short_write = 13;
    submit_line("ST1 READ 123456789abcdef0 0123456789abcdef0123456789abcdef 0 3\n");
    assert(raw_reads == 1 && storage.sensor_usb.pending && !storage.sensor_usb.offset);
    storage.state.locked = true; size_t prior = output_length; poll_usb(false);
    assert(output_length == prior && !storage.sensor_usb.pending && !storage.sensor_usb.retained);
    short_write = SIZE_MAX; storage.state.locked = false; mounted = false;
    submit_line("ST1 LATEST 4444444444444444\n"); assert(json_reads == 1);
    mounted = true; storage.state_known = false;
    submit_line("ST1 LATEST 5555555555555555\n"); assert(json_reads == 1);
    storage.state_known = true;
    /* An overlong line cannot inject a valid suffix into either parser. */
    memset(input, 'x', 500); strcpy(input + 500, "ST1 LATEST 6666666666666666\n"); input_at = 0; input_length = strlen(input);
    while (input_at < input_length) poll_usb(false);
    assert(json_reads == 1 && !staged);
    submit_line("ST1 LATEST 7777777777777777\n"); assert(json_reads == 2);
    assert(!storage.importing && !staged && writes);
    puts("PASS: actual CDC input owner separates CP1/ST1, lock/mount/unknown gates, partial writes and malformed-line recovery");
    return 0;
}
