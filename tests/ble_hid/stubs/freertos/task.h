// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*task)(void *), const char *name, unsigned int stack, void *argument, unsigned int priority, void *handle);
TickType_t xTaskGetTickCount(void);
