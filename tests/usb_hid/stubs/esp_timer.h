// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_timer_handle_t;
enum { ESP_TIMER_TASK };
typedef struct {
    void (*callback)(void *argument);
    void *arg;
    int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *handle);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us);
