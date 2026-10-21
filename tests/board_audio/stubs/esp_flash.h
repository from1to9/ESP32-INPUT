// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { int token; } esp_flash_t;
esp_err_t esp_flash_get_size(esp_flash_t *, uint32_t *);
