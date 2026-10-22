// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "driver/gpio.h"
static inline void gpio_output_enable(gpio_num_t pin) { (void)pin; }
static inline void gpio_output_disable(gpio_num_t pin) { (void)pin; }
