// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_DEVICE_RESTART_IDLE, APP_DEVICE_RESTART_PENDING,
    APP_DEVICE_RESTART_CANCELLED, APP_DEVICE_RESTART_DISPATCHED,
    APP_DEVICE_RESTART_UNCERTAIN,
} app_device_restart_state_t;
typedef struct {
    uint64_t request_id;
    app_device_restart_state_t state;
    esp_err_t error;
} app_device_restart_status_t;

/* One dedicated worker; never restarts during init, request or a read. A copied
 * request must receive fresh coordinator authorization (<=50ms old), actual
 * input-release/microphone-off maintenance evidence and a finite deadline.
 * There is a 200ms reply interval before dispatch. Loss of authority cancels
 * pending work permanently. DISPATCHED means esp_restart is being entered;
 * this boot can never claim that another boot was observed successfully. */
esp_err_t app_device_restart_init(void);
esp_err_t app_device_restart_request(uint64_t deadline_ms, uint64_t *request_id);
void app_device_restart_authorize(uint64_t request_id, bool authorized, bool maintenance_ready);
esp_err_t app_device_restart_cancel(uint64_t request_id);
app_device_restart_status_t app_device_restart_status(void);
