// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef struct { int owner; } portMUX_TYPE;
#define portMUX_FREE_VAL 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
