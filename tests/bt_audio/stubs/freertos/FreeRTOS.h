// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#ifdef TEST_BT_AUDIO_LOCKS
void test_bt_lock(portMUX_TYPE *lock);
void test_bt_unlock(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_bt_lock(lock)
#define portEXIT_CRITICAL(lock) test_bt_unlock(lock)
#else
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#endif
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTICKS_TO_MS(ticks) (ticks)
