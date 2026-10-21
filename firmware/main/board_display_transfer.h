// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#define BOARD_DISPLAY_TRANSFER_RECTS 32U

typedef struct { int16_t x0, y0, x1, y1; } board_display_rect_t; /* Half-open. */
typedef struct {
    uint64_t display_generation;
    uint64_t capture_generation;
    uint64_t indicator_revision;
    uint64_t refresh_sequence;
} board_display_token_t;

typedef struct {
    bool installed;
    bool callback_attached;
    bool association_lost;
    bool refresh_sealed;
    bool coverage_complete;
    bool indicator_transfer_confirmed;
    uint64_t display_generation;
    /* Advances on meaningful invalidation, not an empty/unrelated refresh of
     * the same indicator identity/region. Saturation fails observation closed. */
    uint64_t invalidation_revision;
    board_display_token_t token;
    board_display_rect_t required;
    uint8_t transfers;
    uint8_t completed_transfers;
    uint64_t completed_us;
    esp_err_t error;
} board_display_receipt_t;

/* Boot only: after bsp_display_new and BEFORE adapter registration/start.
 * Decorates the public driver interfaces, preserving original pointers,
 * arguments, draw results and adapter callback/context/yield. No managed-file
 * edits, extra flush waits or audio callbacks. Current qualification scope is
 * the pinned 480x480 CO5300 QSPI TE_SYNC path with zero panel gaps.
 */
esp_err_t board_display_transfer_install(esp_lcd_panel_handle_t, esp_lcd_panel_io_handle_t);

/* UI/LVGL owner calls begin before a forced indicator invalidation/refresh and
 * end at the matching refresh finish. Sequence must increase. Same indicator
 * text with a new capture identity requires a new explicit transfer. Required
 * coordinates are 0..480, half-open. Invalidate on hidden/replaced/cancelled UI.
 * No transfer, an intersecting subset or a bounding box is never full coverage.
 * An in-flight transfer from an older refresh prevents beginning a new one.
 */
esp_err_t board_display_transfer_begin(const board_display_token_t *, const board_display_rect_t *);
esp_err_t board_display_transfer_end(const board_display_token_t *);
void board_display_transfer_invalidate(void);

/* Bounded observation only: copy static receipt records under a short lock,
 * then compute exact rectangle-union coverage in task context. No LVGL, IO,
 * allocation, display lock, wait, logging or mutation. ISR only latches a
 * completion and forwards the actual saved adapter callback. Confirmation
 * needs BOTH ESP_OK draw return and its matching final-color ISR, even when
 * the ISR precedes draw return. Optical readability/scanout is not established;
 * brightness, UI liveness, token ownership and privacy policy remain separate.
 */
board_display_receipt_t board_display_transfer_receipt(void);
