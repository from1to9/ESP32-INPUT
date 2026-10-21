// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"

/* Idempotent single TinyUSB owner: CDC diagnostics + standard composite HID.
 * No command parser, reset handler, auto-download, audio or competing install.
 * usb_hid_init() starts this same manager. Partial startup failure is retained
 * until reboot; repeated starts return that error rather than installing twice. */
esp_err_t usb_console_start(void);
