// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_lcd_touch.h"
typedef struct { int value; } lv_indev_t;
typedef struct {
    void (*on_interrupt)(esp_lcd_touch_handle_t, void *);
    esp_err_t (*custom_touch_read)(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t *, uint8_t *, uint8_t, void *);
    void *user_ctx;
} esp_lv_adapter_touch_callbacks_t;
esp_err_t esp_lv_adapter_set_touch_callbacks(lv_indev_t *, const esp_lv_adapter_touch_callbacks_t *);
