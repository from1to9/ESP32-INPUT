// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_lcd_types.h"
struct esp_lcd_panel_t {
    esp_err_t (*reset)(esp_lcd_panel_t *);
    esp_err_t (*init)(esp_lcd_panel_t *);
    esp_err_t (*del)(esp_lcd_panel_t *);
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *, int, int, int, int, const void *);
    esp_err_t (*mirror)(esp_lcd_panel_t *, bool, bool);
    esp_err_t (*swap_xy)(esp_lcd_panel_t *, bool);
    esp_err_t (*set_gap)(esp_lcd_panel_t *, int, int);
    esp_err_t (*disp_on_off)(esp_lcd_panel_t *, bool);
    esp_err_t (*disp_sleep)(esp_lcd_panel_t *, bool);
    void *user_data;
};
