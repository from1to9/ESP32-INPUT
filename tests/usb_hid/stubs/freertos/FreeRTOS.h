// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define pdPASS 1
#define pdTRUE 1
#define tskIDLE_PRIORITY 0
void usb_test_enter(portMUX_TYPE *lock);
void usb_test_leave(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) usb_test_enter(lock)
#define portEXIT_CRITICAL(lock) usb_test_leave(lock)
