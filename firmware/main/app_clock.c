// SPDX-License-Identifier: Apache-2.0
#include "app_clock.h"
#include <string.h>
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_network_task.h"

/* The callback proves a network synchronization event. It does not prove the
 * cryptographic authenticity of an unauthenticated NTP server. All wall-time
 * consumers receive that explicit quality distinction in the snapshot. */
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    app_clock_snapshot_t view;
    char servers[APP_CLOCK_SERVER_CAPACITY][129];
    unsigned count;
    uint32_t timeout, refresh, maximum_age, config_generation;
    uint64_t anchor_ms, anchor_seconds;
    bool active, worker_started;
} state;
static uint64_t monotonic_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void synchronized(struct timeval *tv)
{
    if (!tv || tv->tv_sec < 1577836800LL || tv->tv_sec > 4133980800LL || tv->tv_usec < 0 ||
        tv->tv_usec >= 1000000) {
        return;
    }
    uint64_t now = monotonic_ms();
    portENTER_CRITICAL(&clock_lock);
    if (state.active && state.view.station_ready) {
        state.anchor_ms = now;
        state.anchor_seconds = (uint64_t)tv->tv_sec;
        state.view.known = true;
        state.view.synchronizing = false;
        state.view.error = ESP_OK;
        ++state.view.revision;
    }
    portEXIT_CRITICAL(&clock_lock);
}
static void worker(void *unused)
{
    (void)unused;
    bool initialized = false;
    uint32_t generation = 0;
    uint64_t started = 0, next = 0;
    char names[APP_CLOCK_SERVER_CAPACITY][129];
    for (;;) {
        uint64_t now = monotonic_ms();
        portENTER_CRITICAL(&clock_lock);
        bool ready = state.view.station_ready;
        bool changed = generation != state.config_generation;
        bool synced = state.view.known && state.anchor_ms >= started;
        bool timed_out = initialized && now >= started && now - started >= state.timeout;
        unsigned count = state.count;
        uint32_t refresh = state.refresh;
        if (initialized && (!ready || changed || synced || timed_out)) {
            state.active = false;
            state.view.synchronizing = false;
            if (timed_out && !synced) {
                state.view.error = ESP_ERR_TIMEOUT;
            }
        }
        bool stop = initialized && !state.active;
        if (state.view.known &&
            (now < state.anchor_ms || now - state.anchor_ms > state.maximum_age)) {
            state.view.known = false;
            state.view.error = ESP_ERR_TIMEOUT;
            ++state.view.revision;
        }
        memcpy(names, state.servers, sizeof(names));
        uint32_t proposed_generation = state.config_generation;
        portEXIT_CRITICAL(&clock_lock);
        if (stop) {
            /* SDK stop executes through the TCP/IP owner. Disable admission
             * before draining it, so a late callback cannot refresh an anchor. */
            esp_netif_sntp_deinit();
            initialized = false;
            next = changed ? now : now + refresh;
        }
        if (!initialized && ready && count && (changed || now >= next)) {
            esp_sntp_config_t config = {.smooth_sync = false,
                                        .server_from_dhcp = false,
                                        .wait_for_sync = false,
                                        .start = false,
                                        .sync_cb = synchronized,
                                        .num_of_servers = count};
            for (unsigned i = 0; i < count && i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
                config.servers[i] = names[i];
            }
            esp_err_t error = esp_netif_sntp_init(&config);
            if (error == ESP_OK) {
                initialized = true;
                started = now;
                generation = proposed_generation;
                portENTER_CRITICAL(&clock_lock);
                state.active = state.view.station_ready && state.config_generation == generation;
                state.view.synchronizing = state.active;
                portEXIT_CRITICAL(&clock_lock);
                error = esp_netif_sntp_start();
            }
            portENTER_CRITICAL(&clock_lock);
            state.view.error = error;
            if (error != ESP_OK) {
                state.active = false;
                state.view.synchronizing = false;
            }
            ++state.view.revision;
            portEXIT_CRITICAL(&clock_lock);
            next = now + refresh;
            if (error != ESP_OK) {
                generation = proposed_generation;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
esp_err_t app_clock_init(const app_clock_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t timeout = config->sync_timeout_ms ? config->sync_timeout_ms : 30000U;
    uint32_t refresh = config->refresh_interval_ms ? config->refresh_interval_ms : 900000U;
    uint32_t age = config->maximum_age_ms ? config->maximum_age_ms : 3600000U;
    if (timeout < 1000 || timeout > 60000 || refresh < 60000 || refresh > 86400000 || age < 60000 ||
        age > 86400000) {
        return ESP_ERR_INVALID_ARG;
    }
    char names[APP_CLOCK_SERVER_CAPACITY][129] = {0};
    unsigned count = 0;
    for (unsigned i = 0; i < APP_CLOCK_SERVER_CAPACITY; ++i) {
        if (!config->servers[i]) {
            continue;
        }
        size_t length = strlen(config->servers[i]);
        if (!length || length > 128 || count >= CONFIG_LWIP_SNTP_MAX_SERVERS) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < length; ++j) {
            unsigned char c = (unsigned char)config->servers[i][j];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '.')) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        memcpy(names[count++], config->servers[i], length + 1);
    }
    portENTER_CRITICAL(&clock_lock);
    bool start = !state.worker_started;
    state.worker_started = true;
    state.view.initialized = true;
    state.view.known = false;
    state.view.error = count ? ESP_OK : ESP_ERR_NOT_FOUND;
    state.active = false;
    memcpy(state.servers, names, sizeof(names));
    state.count = count;
    state.timeout = timeout;
    state.refresh = refresh;
    state.maximum_age = age;
    ++state.config_generation;
    ++state.view.revision;
    portEXIT_CRITICAL(&clock_lock);
    if (start && app_network_task_create(worker, "network_clock", 4096, NULL, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&clock_lock);
        state.worker_started = false;
        state.view.initialized = false;
        state.view.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&clock_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void app_clock_station_ready(bool ready)
{
    portENTER_CRITICAL(&clock_lock);
    if (state.view.station_ready != ready) {
        state.view.station_ready = ready;
        ++state.view.revision;
    }
    if (!ready) {
        state.active = false;
    }
    portEXIT_CRITICAL(&clock_lock);
}
void app_clock_snapshot(app_clock_snapshot_t *out)
{
    if (!out) {
        return;
    }
    uint64_t now = monotonic_ms();
    portENTER_CRITICAL(&clock_lock);
    *out = state.view;
    bool valid = out->known && now >= state.anchor_ms && now - state.anchor_ms <= state.maximum_age;
    out->known = valid;
    out->unix_seconds = valid ? state.anchor_seconds + (now - state.anchor_ms) / 1000U : 0;
    out->age_ms = valid ? (uint32_t)(now - state.anchor_ms) : UINT32_MAX;
    out->cryptographically_authenticated = false;
    portEXIT_CRITICAL(&clock_lock);
}
bool app_clock_wall_time(uint64_t *seconds)
{
    app_clock_snapshot_t view;
    app_clock_snapshot(&view);
    if (!seconds || !view.known) {
        return false;
    }
    *seconds = view.unix_seconds;
    return true;
}
