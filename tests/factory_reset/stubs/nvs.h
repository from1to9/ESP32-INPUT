// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_ANY 0
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE 16
typedef unsigned nvs_handle_t;
typedef struct reset_iterator *nvs_iterator_t;
typedef struct { char namespace_name[16]; char key[16]; int type; } nvs_entry_info_t;
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_entry_find(const char *, const char *, int, nvs_iterator_t *);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
void nvs_release_iterator(nvs_iterator_t);
