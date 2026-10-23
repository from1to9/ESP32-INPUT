// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "board_sensor_journal.h"

#define SENSOR_WIRE_VERSION 2U
#define SENSOR_WIRE_HEADER_BYTES 56U
#define SENSOR_WIRE_RECORD_BYTES 88U
enum {
    SENSOR_PAGE_GAP = 1U,
    SENSOR_PAGE_MORE = 2U,
    SENSOR_PAGE_EXHAUSTED = 4U,
    SENSOR_PAGE_UNAVAILABLE = 8U,
};

/* Explicit LE fields and trailing IEEE CRC32. Caller owns all storage. No
 * allocation, I/O or mutation of the copied page. Zero means invalid/capacity. */
size_t sensor_wire_encode(const uint8_t boot[16], uint64_t request_id, uint64_t after,
    const board_sensor_batch_t *, const board_sensor_record_t *, uint8_t *, size_t);
uint32_t sensor_wire_crc32(const uint8_t *, size_t);
bool sensor_wire_base64(const uint8_t *, size_t, char *, size_t);
