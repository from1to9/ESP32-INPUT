// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ble_hid.h"

/* Single BLE control-worker owner. Explicit versioned serialization in the
 * existing default NVS partition, namespace hid_targets / key preferences.
 * No auto erase, exported keys, callback I/O, allocation or raw struct storage. */
typedef struct {
    uint64_t revision;
    bool selected;
    ble_hid_target_t target;
    uint8_t count;
    ble_hid_target_t order[BLE_HID_MAX_BONDS];
} ble_hid_preferences_t;

esp_err_t ble_hid_preferences_load(ble_hid_preferences_t *, bool *exists);
/* uncertain becomes true before the first NVS mutation and remains true on
 * any unverified outcome. Success requires commit and exact valid readback. */
esp_err_t ble_hid_preferences_save(const ble_hid_preferences_t *, bool *uncertain);
