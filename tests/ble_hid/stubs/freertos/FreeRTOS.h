// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_port_enter_critical(portMUX_TYPE *lock);
void test_port_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_port_enter_critical(lock)
#define portEXIT_CRITICAL(lock) test_port_exit_critical(lock)
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTICKS_TO_MS(ticks) (ticks)
