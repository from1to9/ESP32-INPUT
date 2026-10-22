// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "freertos/task.h"
BaseType_t xTaskCreateWithCaps(TaskFunction_t, const char *, uint32_t, void *, unsigned, TaskHandle_t *, unsigned);
