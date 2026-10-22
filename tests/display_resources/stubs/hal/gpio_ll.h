// SPDX-License-Identifier: Apache-2.0
#pragma once
static int GPIO;
static inline void gpio_ll_set_level(int *gpio, int pin, unsigned int value)
{ (void)gpio; (void)pin; (void)value; }
static inline void gpio_ll_output_enable(int *gpio, int pin) { (void)gpio; (void)pin; }
static inline void gpio_ll_output_disable(int *gpio, int pin) { (void)gpio; (void)pin; }
