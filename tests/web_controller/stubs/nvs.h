// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_INVALID_CRC 0x109
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
