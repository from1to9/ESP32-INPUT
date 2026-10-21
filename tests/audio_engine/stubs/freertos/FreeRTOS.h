// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <pthread.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) ((void)pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) ((void)pthread_mutex_unlock(lock))
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
