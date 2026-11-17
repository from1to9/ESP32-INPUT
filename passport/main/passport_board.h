#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "passport_view.h"

enum {
    PASSPORT_BUTTON_UP = 1u << 0, PASSPORT_BUTTON_DOWN = 1u << 1,
    PASSPORT_BUTTON_OK = 1u << 2,
    PASSPORT_ALL_BUTTONS = PASSPORT_BUTTON_UP | PASSPORT_BUTTON_DOWN | PASSPORT_BUTTON_OK,
};
typedef enum { PASSPORT_UP, PASSPORT_DOWN, PASSPORT_OK } passport_button_t;
typedef enum { PASSPORT_PRESS, PASSPORT_CLICKED, PASSPORT_DOUBLE, PASSPORT_LONG, PASSPORT_RELEASE } passport_button_kind_t;
typedef struct {
    passport_button_t button;
    passport_button_kind_t kind;
    uint64_t time_ms;
} passport_button_event_t;

esp_err_t passport_display_init(void);
esp_err_t passport_buttons_init(void);
unsigned passport_buttons_observed(void);
unsigned passport_button_click_guard_ms(void);
bool passport_render(const passport_view_t *view);
bool passport_next_button(passport_button_event_t *event);
bool passport_button_overflow(void);
void passport_backlight(bool on);
bool passport_board_diagnostics(void);
