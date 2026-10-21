// SPDX-License-Identifier: Apache-2.0
#pragma once
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGE(tag, ...) test_log(tag, __VA_ARGS__)
