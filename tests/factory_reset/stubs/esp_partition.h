// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
typedef struct { uint32_t address, size; unsigned type, subtype; const char *label; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(unsigned, unsigned, const char *);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
