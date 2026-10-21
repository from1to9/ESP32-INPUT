// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void presenter_test_enter(portMUX_TYPE *lock);
void presenter_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) presenter_test_enter(lock)
#define portEXIT_CRITICAL(lock) presenter_test_exit(lock)
