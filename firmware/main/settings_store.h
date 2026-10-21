// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device_settings.h"
#include "esp_err.h"

typedef struct {
    bool available;
    bool loaded;
    bool pending;
    esp_err_t error;
    uint64_t requested_revision;
    uint64_t saved_revision;
} settings_store_status_t;

/* Startup only, after nvs_flash_init. Installs defaults before attempting a
 * validated 16-byte legacy or 40-byte schema-3 load. Legacy decoding defaults
 * only new fields; unchanged values do not schedule a format upgrade/write.
 * Missing data is normal; corrupt/unsupported data is retained
 * and reported. Never erases NVS or stores defaults merely because of boot. */
esp_err_t settings_store_init(device_settings_t *settings);

/* Nonblocking coalesced save request for an explicit setting change. A worker
 * waits for one second of quiet before committing the latest value. Unchanged
 * records cause no write. Failure is visible and does not retry every frame;
 * a later explicit request may retry. ESP_OK means accepted, not durable. */
esp_err_t settings_store_request(const device_settings_t *settings);
settings_store_status_t settings_store_status(void);
