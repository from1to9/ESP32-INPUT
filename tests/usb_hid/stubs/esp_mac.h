// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_BASE } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *address, esp_mac_type_t type);
