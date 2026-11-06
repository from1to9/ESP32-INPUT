// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

void bt_classic_connect_diag_tx(const uint8_t *h4, size_t len);
void bt_classic_connect_diag_rx(const uint8_t *h4, size_t len);
void bt_classic_connect_diag_poll(uint64_t now_ms);
void bt_classic_connect_diag_scan_end(uint8_t link, const void *event);
