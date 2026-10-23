// SPDX-License-Identifier: Apache-2.0
#pragma once
void audio_diagnostics_test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(...) audio_diagnostics_test_log(__VA_ARGS__)
