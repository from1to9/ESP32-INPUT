// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
static int64_t board_test_time_us = 1000000;
static inline int64_t esp_timer_get_time(void) { return board_test_time_us; }
