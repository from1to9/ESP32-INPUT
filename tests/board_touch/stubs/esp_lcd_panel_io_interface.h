// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
struct esp_lcd_panel_io_t {
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
};
