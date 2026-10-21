// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void settings_test_enter(portMUX_TYPE *lock);
void settings_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) settings_test_enter(lock)
#define portEXIT_CRITICAL(lock) settings_test_exit(lock)
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
