// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Private SDK boundary. The key never leaves the Bluedroid owners. */
typedef struct { uint8_t bytes[32]; } bt_classic_bond_id_t;
bool bt_classic_bond_id(const uint8_t key[16], bt_classic_bond_id_t *out);
esp_err_t bt_classic_forget_review(const uint8_t peer[6], bt_classic_bond_id_t *out);
esp_err_t bt_classic_forget_runtime(const uint8_t peer[6], const bt_classic_bond_id_t *expected, bool clear);
esp_err_t bt_classic_forget_storage(const uint8_t peer[6], const bt_classic_bond_id_t *expected);

/* Caller holds the SDK config lock. Only the existing one-blob format is
 * eligible for an atomic selected-record update. */
esp_err_t bt_classic_forget_config_check(void);
bool bt_classic_forget_config_save(void);
