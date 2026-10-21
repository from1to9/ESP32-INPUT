// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_flash.h"
#define ESP_PARTITION_TYPE_DATA 1
typedef struct {
    esp_flash_t *flash_chip;
    int type, subtype;
    uint32_t address, size, erase_size;
    char label[17];
    bool encrypted, readonly;
} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
