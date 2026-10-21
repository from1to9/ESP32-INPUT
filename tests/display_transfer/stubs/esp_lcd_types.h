// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_lcd_panel_t esp_lcd_panel_t;
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct { int reserved; } esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
typedef struct { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done; } esp_lcd_panel_io_callbacks_t;
