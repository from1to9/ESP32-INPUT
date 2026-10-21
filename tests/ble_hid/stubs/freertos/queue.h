// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned int count, unsigned int size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned int timeout);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned int timeout);
