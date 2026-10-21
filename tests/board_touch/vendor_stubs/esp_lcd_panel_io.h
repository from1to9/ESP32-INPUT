// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_lcd_panel_io_interface.h"
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t, int, void *, size_t);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void *, size_t);
