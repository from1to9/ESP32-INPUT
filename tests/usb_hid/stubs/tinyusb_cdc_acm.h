// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
typedef struct { int unused; } tinyusb_config_cdcacm_t;
enum { TINYUSB_CDC_ACM_0 };
esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config);
