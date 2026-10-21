// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
#include <stddef.h>
typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t item_size;
    size_t head;
    size_t count;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                uint8_t *bytes, StaticQueue_t *storage);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
BaseType_t xQueueReset(QueueHandle_t queue);
