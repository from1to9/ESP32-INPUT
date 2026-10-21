// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Pinned Bluedroid boundary (383dda6c1), declared in btc/btc_storage.h.
 * The public Classic/LE lists omit incomplete address sections; their sum
 * cannot prove spare capacity in the SDK's shared eviction domain. This
 * existing SDK function counts that exact domain under the SDK config lock.
 * Keep this dependency explicit and re-review it on every SDK update. */
int btc_storage_get_num_all_bond_devices(void);
