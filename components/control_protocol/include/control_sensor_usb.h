// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"

#define CP_SENSOR_USB_FRAME_BYTES 480U
#define CP_SENSOR_USB_FRAGMENT_BYTES 192U
#define CP_SENSOR_USB_BATCH_BYTES 324U
#define CP_SENSOR_USB_DEADLINE_MS UINT64_C(2000)

typedef enum {
    CP_SENSOR_RESOURCE_NONE, CP_SENSOR_RESOURCE_INVALID,
    CP_SENSOR_RESOURCE_CATALOG, CP_SENSOR_RESOURCE_LATEST, CP_SENSOR_RESOURCE_SAMPLES
} cp_sensor_resource_t;
/* Exact paths and canonical boot/after/limit query. No decoding or aliases. */
cp_sensor_resource_t cp_sensor_resource(const char *);

typedef struct {
    void *user;
    bool (*allowed)(void *);
    cp_error_t (*read)(void *, const char *, char *, size_t, size_t *);
    cp_error_t (*batch)(void *, const char boot[33], uint64_t after, unsigned limit,
                        uint64_t request_id, uint8_t *, size_t, size_t *);
    /* One nonblocking write. A short write is never a delivered frame. */
    size_t (*write)(void *, const char *, size_t);
} cp_sensor_usb_port_t;

/* Single transport-worker ownership. Allocate with its existing bounded
 * workspace, never on a latency-sensitive callback's stack. No retained ports. */
typedef struct {
    char body[CP_MAX_RESPONSE_BYTES + 1U];
    char frame[CP_SENSOR_USB_FRAME_BYTES + 1U];
    char command[128], request_id[17], digest[65];
    size_t length, offset;
    uint64_t deadline_ms, started_ms, generation;
    bool pending, binary, retained;
} cp_sensor_usb_t;

void cp_sensor_usb_cancel(cp_sensor_usb_t *);
/* Only ST1 requests are recognized. The caller retains sole CDC input and
 * independently gates CP1 commissioning; this codec cannot issue commands. */
bool cp_sensor_usb_receive(cp_sensor_usb_t *, const char *, uint64_t now_ms,
                           const cp_sensor_usb_port_t *);
/* At most one <=480-byte write; repeats a whole damaged frame with a fresh
 * newline marker. No cursor or offset advances on zero/partial writes. */
void cp_sensor_usb_service(cp_sensor_usb_t *, uint64_t now_ms, const cp_sensor_usb_port_t *);
