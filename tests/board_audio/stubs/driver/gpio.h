// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ENABLE 1
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);
