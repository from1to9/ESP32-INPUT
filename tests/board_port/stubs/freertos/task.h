// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *argument);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle);
void xTaskNotifyGive(TaskHandle_t handle);
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskDelayUntil(TickType_t *deadline, TickType_t increment);
void vTaskDelay(TickType_t delay);
void vTaskDelete(TaskHandle_t handle);
