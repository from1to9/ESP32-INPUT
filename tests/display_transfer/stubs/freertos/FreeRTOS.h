// SPDX-License-Identifier: Apache-2.0
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void display_test_enter(int isr);
void display_test_leave(int isr);
#define portENTER_CRITICAL(lock) ((void)(lock), display_test_enter(0))
#define portEXIT_CRITICAL(lock) ((void)(lock), display_test_leave(0))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock), display_test_enter(1))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock), display_test_leave(1))
