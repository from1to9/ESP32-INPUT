// SPDX-License-Identifier: Apache-2.0
#pragma once
void raw_touch_log(const char *, const char *, ...);
#define ESP_LOGD raw_touch_log
#define ESP_LOGI raw_touch_log
#define ESP_LOGW raw_touch_log
#define ESP_LOGE raw_touch_log
