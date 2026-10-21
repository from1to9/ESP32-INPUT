// SPDX-License-Identifier: Apache-2.0
#pragma once
void board_test_log(const char *tag, const char *format, ...);
#define ESP_LOGE(...) board_test_log(__VA_ARGS__)
#define ESP_LOGW(...) board_test_log(__VA_ARGS__)
#define ESP_LOGI(...) board_test_log(__VA_ARGS__)
