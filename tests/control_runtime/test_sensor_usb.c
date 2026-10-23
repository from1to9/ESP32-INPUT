// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "control_sensor_usb.h"

static cp_sensor_usb_t state;
static bool permitted = true, revoke_during_read;
static unsigned reads, batches, writes;
static size_t write_limit = SIZE_MAX, json_length = 4096;
static char last[CP_SENSOR_USB_FRAME_BYTES + 1U];
static bool allowed(void *u) { (void)u; return permitted; }
static cp_error_t read_json(void *u, const char *path, char *out, size_t capacity, size_t *length)
{
    (void)u; assert(!strcmp(path, "/api/v1/sensors/catalog") || !strcmp(path, "/api/v1/sensors/latest"));
    ++reads; assert(capacity > json_length);
    memset(out, 'a', json_length); memcpy(out, "{\"value\":\"", 10);
    out[json_length - 2U] = '"'; out[json_length - 1U] = '}'; out[json_length] = 0;
    *length = json_length;
    if (revoke_during_read) permitted = false;
    return CP_OK;
}
static cp_error_t batch(void *u, const char boot[33], uint64_t after, unsigned limit, uint64_t id,
                        uint8_t *out, size_t capacity, size_t *length)
{
    (void)u; assert(!strcmp(boot, "0123456789abcdef0123456789abcdef") && after == UINT64_MAX &&
        limit == 3 && id == UINT64_C(0x123456789abcdef0) && capacity == 324);
    ++batches;
    for (size_t i = 0; i < capacity; ++i) out[i] = (uint8_t)i;
    *length = capacity;
    return CP_OK;
}
static size_t write_frame(void *u, const char *bytes, size_t length)
{
    (void)u; assert(permitted && length <= 480); ++writes;
    memcpy(last, bytes, length); last[length] = 0;
    return length < write_limit ? length : write_limit;
}
static const cp_sensor_usb_port_t port = {.allowed = allowed, .read = read_json, .batch = batch, .write = write_frame};
static const char request[] = "ST1 READ 123456789abcdef0 0123456789abcdef0123456789abcdef 18446744073709551615 3";
static void begin(void)
{
    memset(&state, 0, sizeof(state)); permitted = true; revoke_during_read = false;
    reads = batches = writes = 0; write_limit = SIZE_MAX; json_length = 4096;
}
int main(void)
{
    assert(cp_sensor_resource("/api/v1/sensors/catalog") == CP_SENSOR_RESOURCE_CATALOG);
    const char *good = "/api/v1/sensors/samples?boot=0123456789abcdef0123456789abcdef&after=18446744073709551615&limit=8";
    assert(cp_sensor_resource(good) == CP_SENSOR_RESOURCE_SAMPLES);
    const char *bad[] = {"/api/v1/sensors", "/api/v1/sensors/catalog?x=1", "/api/v1/sensors/latest/",
        "/api/v1/sensors/samples?boot=0123456789abcdef0123456789abcdef&after=00&limit=3",
        "/api/v1/sensors/samples?boot=0123456789abcdef0123456789abcdef&after=18446744073709551616&limit=3",
        "/api/v1/sensors/samples?boot=0123456789abcdef0123456789abcdef&after=0&limit=3&limit=1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) assert(cp_sensor_resource(bad[i]) == CP_SENSOR_RESOURCE_INVALID);
    begin(); assert(!cp_sensor_usb_receive(&state, "CP1 CHALLENGE", 100, &port) && !reads);
    assert(cp_sensor_usb_receive(&state, request, 100, &port) && batches == 1 && state.pending);
    write_limit = 0; cp_sensor_usb_service(&state, 101, &port); assert(!state.offset && state.pending);
    char original[481]; strcpy(original, last);
    assert(strlen(last) == 461 && !strncmp(last, "\nST1 R 123456789abcdef0 324 ", 27));
    write_limit = 17; cp_sensor_usb_service(&state, 102, &port);
    assert(!strcmp(last, original) && !state.offset);
    assert(cp_sensor_usb_receive(&state, "ST1 CATALOG 2222222222222222", 103, &port) && !reads);
    write_limit = SIZE_MAX; cp_sensor_usb_service(&state, 104, &port);
    assert(!state.pending && state.offset == 324 && !strcmp(last, original));
    assert(cp_sensor_usb_receive(&state, request, 105, &port) && batches == 1);
    cp_sensor_usb_service(&state, 106, &port); assert(!strcmp(last, original));
    assert(cp_sensor_usb_receive(&state, "ST1 CATALOG 123456789abcdef0", 107, &port) && !reads);
    begin(); assert(cp_sensor_usb_receive(&state, "ST1 READ 123456789abcdef0 0123456789abcdef0123456789abcdef 0 4", 100, &port));
    assert(!batches && !state.binary && strstr(state.body, "invalid"));
    cp_sensor_usb_service(&state, 100, &port); assert(strstr(last, "ST1 END "));
    begin(); assert(cp_sensor_usb_receive(&state, "ST1 CATALOG 1111111111111111", 100, &port) && reads == 1);
    size_t previous = 0;
    while (state.pending) {
        cp_sensor_usb_service(&state, 100 + writes * 20U, &port);
        assert(state.offset > previous); previous = state.offset;
    }
    assert(previous == 4096 && writes == 22 && strstr(last, "ST1 END 1111111111111111 4096 "));
    uint8_t digest[32]; char expected[65]; cp_sha256(state.body, state.length, digest);
    for (size_t i = 0; i < 32; ++i) (void)snprintf(expected + 2U * i, 3, "%02x", digest[i]);
    assert(strstr(last, expected));
    begin(); cp_sensor_usb_receive(&state, "ST1 CATALOG 1111111111111111", 100, &port);
    cp_sensor_usb_service(&state, 2100, &port); assert(!writes && !state.pending && !state.body[0]);
    begin(); cp_sensor_usb_receive(&state, request, 100, &port); permitted = false;
    cp_sensor_usb_service(&state, 101, &port); assert(!writes && !state.pending && !state.body[0]);
    begin(); revoke_during_read = true;
    cp_sensor_usb_receive(&state, "ST1 LATEST 1111111111111111", 100, &port);
    assert(!state.pending && !state.retained);
    begin(); cp_sensor_usb_receive(&state, request, 100, &port);
    cp_sensor_usb_service(&state, 99, &port); assert(!writes && !state.pending);
    puts("PASS: production sensor USB bounds, retry, independent read framing, expiry and access revocation");
    return 0;
}
