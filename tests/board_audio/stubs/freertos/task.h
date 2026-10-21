// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *argument);
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
