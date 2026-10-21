// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
typedef void *TimerHandle_t;
TimerHandle_t xTimerCreate(const char *name, TickType_t period, int reload,
                          void *identifier, void (*callback)(TimerHandle_t));
int xTimerStart(TimerHandle_t timer, TickType_t timeout);
