// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdint.h>
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif
typedef struct { int value; } lv_indev_t;
#include "esp_lcd_panel_io_interface.h"
typedef struct { esp_lcd_panel_io_t *io; } test_touch_t;
typedef test_touch_t *esp_lcd_touch_handle_t;
typedef struct { uint8_t track_id; uint16_t x, y, strength; } esp_lcd_touch_point_data_t;
typedef struct {
    void (*on_interrupt)(esp_lcd_touch_handle_t, void *);
    esp_err_t (*custom_touch_read)(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t *, uint8_t *, uint8_t, void *);
    void *user_ctx;
} esp_lv_adapter_touch_callbacks_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t *, uint8_t *, uint8_t);
esp_err_t esp_lv_adapter_set_touch_callbacks(lv_indev_t *, const esp_lv_adapter_touch_callbacks_t *);
