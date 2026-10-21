// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define APP_CLOCK_SERVER_CAPACITY 2U
typedef struct {
    const char *servers[APP_CLOCK_SERVER_CAPACITY];
    uint32_t sync_timeout_ms;     /* 1000..60000; default 30000. */
    uint32_t refresh_interval_ms; /* 60000..86400000; default 900000. */
    uint32_t maximum_age_ms;      /* 60000..86400000; default 3600000. */
} app_clock_config_t;
typedef struct {
    bool initialized, station_ready, synchronizing, known;
    bool cryptographically_authenticated; /* Plain SNTP is explicitly false. */
    uint64_t unix_seconds;
    uint32_t age_ms, revision;
    esp_err_t error;
} app_clock_snapshot_t;
/* Boot is unknown until the real SNTP synchronization callback. Date range or
 * existing system time never creates known status. No persisted extrapolation
 * across power loss, auto erase or hidden network service before STA/policy. */
esp_err_t app_clock_init(const app_clock_config_t *);
void app_clock_station_ready(bool ready);
bool app_clock_wall_time(uint64_t *unix_seconds);
void app_clock_snapshot(app_clock_snapshot_t *);
