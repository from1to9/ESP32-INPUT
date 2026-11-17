// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "passport_ble.h"
#include "passport_wifi.h"
#include "passport_theme.h"

// Runs only on the UI owner. No task, USB driver, or snapshot allocation.
void passport_usb_init(void);
void passport_usb_service(const passport_ble_state_t *, const passport_wifi_state_t *,
                          passport_theme_t theme, bool theme_known, bool settings_busy);
// UI-owned persistence/render state, shared with the native appearance flow.
esp_err_t passport_ui_set_theme(passport_theme_t theme);
esp_err_t passport_ui_set_sound(bool enabled);
