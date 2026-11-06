// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

struct osi_thread;

/* Audio owner. A false release result is completed by the drained callback. */
esp_err_t bt_classic_scan_prepare(uint64_t request, const uint8_t peer[6]);
bool bt_classic_scan_release(uint64_t request, bool *restore);
void bt_audio_classic_scan_result(uint64_t request, esp_err_t result);
void bt_audio_classic_scan_drained(uint64_t request, bool restore);

/* Pinned BTU lifecycle and Write Scan Enable path. */
bool bt_classic_scan_bind(struct osi_thread *thread);
void bt_classic_scan_enable(void);
void bt_classic_scan_disable(void);
void bt_classic_scan_unbind(void);
bool bt_classic_scan_write(uint8_t mask, uint32_t epoch);
void bt_classic_scan_complete(uint32_t epoch, bool accepted);
