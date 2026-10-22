// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "driver/spi_master.h"
static inline void gpio_set_level(gpio_num_t pin, int value) { (void)pin; (void)value; }
