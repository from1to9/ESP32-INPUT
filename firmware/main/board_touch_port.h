// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define BOARD_TOUCH_CONTACTS 2U
typedef struct { uint8_t id; uint16_t x, y; } board_touch_contact_t;
typedef struct {
    uint64_t sequence, timestamp_ms;
    bool valid, overflow;
    uint8_t count;
    board_touch_contact_t contacts[BOARD_TOUCH_CONTACTS];
} board_touch_snapshot_t;
/* Runs under the existing LVGL input-reader lock, before its primary events.
 * Coordinates are the same display frame used by that input reader. The
 * callback must only copy/enqueue; false rejects this scan and triggers the
 * coordinator's bounded release path. No second sensor reader is installed. */
typedef bool (*board_touch_snapshot_fn)(const board_touch_snapshot_t *);
esp_err_t board_touch_observe(board_touch_snapshot_fn);
