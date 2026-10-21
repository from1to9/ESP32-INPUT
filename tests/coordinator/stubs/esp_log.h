// SPDX-License-Identifier: Apache-2.0
#pragma once
void coordinator_test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(tag, ...) coordinator_test_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) coordinator_test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) coordinator_test_log(tag, __VA_ARGS__)
