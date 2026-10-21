// SPDX-License-Identifier: Apache-2.0
#include "settings_store.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define SAVE_QUIET_US 1000000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static settings_store_status_t s_status;
static bool s_started, s_dirty, s_saved_known;
static uint8_t s_requested[DEVICE_SETTINGS_RECORD_SIZE];
static uint8_t s_saved[DEVICE_SETTINGS_RECORD_SIZE];
static int64_t s_changed_us;
static nvs_handle_t s_handle;

static bool take_pending(uint8_t *record, uint64_t *revision, int64_t now_us)
{
    bool ready = false;
    portENTER_CRITICAL(&s_lock);
    if (s_dirty && now_us < s_changed_us) s_changed_us = now_us;
    if (s_dirty && now_us >= s_changed_us && now_us - s_changed_us >= SAVE_QUIET_US) {
        memcpy(record, s_requested, sizeof(s_requested));
        *revision = s_status.requested_revision;
        s_dirty = false;
        ready = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

static void save_once(void)
{
    uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
    uint64_t revision;
    if (!take_pending(record, &revision, esp_timer_get_time())) return;
    portENTER_CRITICAL(&s_lock);
    bool unchanged = s_saved_known && memcmp(s_saved, record, sizeof(record)) == 0;
    portEXIT_CRITICAL(&s_lock);
    /* Only this worker accesses the open NVS handle after initialization. */
    esp_err_t error = ESP_OK;
    if (!unchanged) {
        error = nvs_set_blob(s_handle, "record", record, sizeof(record));
        if (error == ESP_OK) error = nvs_commit(s_handle);
    }
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK) {
        memcpy(s_saved, record, sizeof(s_saved));
        s_saved_known = true;
        s_status.saved_revision = revision;
    } else {
        /* A failing flash operation may already have reached storage. The old
         * cache cannot justify skipping a later request that restores it. */
        s_saved_known = false;
    }
    s_status.error = error;
    /* A newer request remains pending, including one arriving during commit. */
    s_status.pending = s_dirty;
    portEXIT_CRITICAL(&s_lock);
}

static void store_worker(void *argument)
{
    (void)argument;
    for (;;) {
        save_once();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t settings_store_init(device_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_started = true;
    device_settings_defaults(settings);
    esp_err_t error = nvs_open("input_prefs", NVS_READWRITE, &s_handle);
    if (error != ESP_OK) {
        s_status.error = error;
        return error;
    }
    uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
    size_t length = sizeof(record);
    error = nvs_get_blob(s_handle, "record", record, &length);
    if (error == ESP_OK) {
        if (device_settings_decode(record, length, settings) == DEVICE_SETTINGS_OK) {
            s_status.loaded = true;
            s_saved_known = true;
            memcpy(s_saved, record, sizeof(s_saved));
        } else {
            error = ESP_ERR_INVALID_ARG;
        }
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    s_status.error = error;
    if (xTaskCreate(store_worker, "input_settings", 3072, NULL, 2, NULL) != pdPASS) {
        nvs_close(s_handle);
        s_status.error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    s_status.available = true;
    return error;
}

esp_err_t settings_store_request(const device_settings_t *settings)
{
    uint8_t record[DEVICE_SETTINGS_RECORD_SIZE];
    if (device_settings_encode(settings, record, sizeof(record)) != DEVICE_SETTINGS_OK)
        return ESP_ERR_INVALID_ARG;
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (!s_status.available) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_dirty && memcmp(s_requested, record, sizeof(record)) == 0) ||
        (!s_status.pending && s_status.error == ESP_OK && s_saved_known &&
         memcmp(s_saved, record, sizeof(record)) == 0)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (s_status.requested_revision == UINT64_MAX) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_requested, record, sizeof(s_requested));
    ++s_status.requested_revision;
    s_changed_us = now_us;
    s_dirty = true;
    s_status.pending = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

settings_store_status_t settings_store_status(void)
{
    portENTER_CRITICAL(&s_lock);
    settings_store_status_t status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}
