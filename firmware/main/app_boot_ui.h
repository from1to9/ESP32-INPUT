// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    APP_BOOT_UI_STARTING,
    APP_BOOT_UI_READY,
    APP_BOOT_UI_FAULT
} app_boot_ui_state_t;

typedef struct {
    app_boot_ui_state_t state;
    /* NUL-terminated printable ASCII; stage is required, detail may be empty. */
    char stage[48];
    char detail[96];
    /* Actual completed startup steps, never an estimate. Unknown means unavailable. */
    bool steps_known;
    uint16_t completed_steps;
    uint16_t total_steps;
} app_boot_ui_view_t;

/* All calls require the caller's display lock. One 480 x 480 instance is allowed.
 * Create covers the active screen without changing it or allocating a framebuffer.
 * No I/O, timers, animation, delays or readiness inference are performed here.
 * LVGL's configured allocation-failure policy applies to object creation. */
bool app_boot_ui_create(lv_display_t *display);
/* Invalid snapshots are rejected atomically. Strings are copied, never borrowed.
 * READY means local controls ready only; it does not assert transport readiness. */
bool app_boot_ui_update(const app_boot_ui_view_t *view);
/* Idempotent. Deletes only the owned root; call before creating the product UI
 * under the same lock, with no forced intermediate flush or minimum show time. */
void app_boot_ui_destroy(void);
bool app_boot_ui_active(void);
