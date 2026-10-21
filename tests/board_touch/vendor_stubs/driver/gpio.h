// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define BIT64(n) (UINT64_C(1) << (n))
#define GPIO_NUM_NC -1
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<64)
#define GPIO_IS_VALID_OUTPUT_GPIO GPIO_IS_VALID_GPIO
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_NEGEDGE 1
#define GPIO_INTR_POSEDGE 2
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
