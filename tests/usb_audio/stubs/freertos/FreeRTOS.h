// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../usb_hid/stubs/freertos/FreeRTOS.h"
#define portENTER_CRITICAL_ISR(lock) usb_test_enter(lock)
#define portEXIT_CRITICAL_ISR(lock) usb_test_leave(lock)
