// SPDX-License-Identifier: Apache-2.0
#pragma once
#define ESP_GOTO_ON_FALSE(condition, error, label, tag, ...) \
    do { if (!(condition)) { ret = (error); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(expression, label, tag, ...) \
    do { esp_err_t checked = (expression); if (checked != ESP_OK) { ret = checked; goto label; } } while (0)
#define ESP_RETURN_ON_ERROR(expression, tag, ...) \
    do { esp_err_t checked = (expression); if (checked != ESP_OK) return checked; } while (0)
