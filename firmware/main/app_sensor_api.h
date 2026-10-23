// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "control_protocol.h"
#include "board_touch_port.h"

#define APP_SENSOR_RECORD_BYTES 88U
#define APP_SENSOR_BATCH_HEADER_BYTES 56U
#define APP_SENSOR_HTTP_MAX_RECORDS 8U
#define APP_SENSOR_USB_MAX_RECORDS 3U
#define APP_SENSOR_BATCH_MAX_BYTES (APP_SENSOR_BATCH_HEADER_BYTES + APP_SENSOR_HTTP_MAX_RECORDS * APP_SENSOR_RECORD_BYTES + 4U)

/* Initialize once before transport workers. This boot identity is independent
 * of HTTP server/session restarts. These copied reads do not authorize callers:
 * the authenticated LAN and unlocked wired adapters own access control. */
void app_sensor_api_init(void);
cp_error_t app_sensor_api_read(const char *resource, char *json, size_t capacity, size_t *length);
cp_error_t app_sensor_api_read_batch(const char boot_hex[33], uint64_t after,
    unsigned limit, uint64_t request_id, uint8_t *raw, size_t capacity, size_t *length);

/* Existing producer callbacks only copy bounded observations. No sensor read,
 * formatting, network work or input authorization occurs here. Coordinates
 * outside the public touchpad are redacted, including private input screens. */
void app_sensor_api_touch(const board_touch_snapshot_t *sample, bool public_coordinates);
void app_sensor_api_button(bool down, uint64_t timestamp_us);
