#pragma once

#include <stdbool.h>
#include "esp_err.h"

enum {
    PASSPORT_BUTTON_UP = 1u << 0,
    PASSPORT_BUTTON_DOWN = 1u << 1,
    PASSPORT_BUTTON_OK = 1u << 2,
    PASSPORT_ALL_BUTTONS = PASSPORT_BUTTON_UP | PASSPORT_BUTTON_DOWN | PASSPORT_BUTTON_OK,
};

esp_err_t passport_display_init(void);
esp_err_t passport_buttons_init(void);
unsigned passport_buttons_observed(void);
bool passport_show_status(const char *network_text, const char *http_status,
                          const char *nimble_status, esp_err_t button_status);
bool passport_wait_for_button(void);
