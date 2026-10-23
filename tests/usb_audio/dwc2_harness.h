// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t ctl, tsiz, dctl, gintsts, gintmsk;
    uint32_t submitted, completed, failed, bytes;
    bool target_odd;
} dwc2_test_snapshot_t;
void dwc2_test_init(bool high_speed);
void dwc2_test_frame(uint32_t frame);
bool dwc2_test_open(const void *descriptor);
bool dwc2_test_submit(uint8_t ep, uint8_t *data, uint16_t bytes);
bool dwc2_test_poll(uint8_t ep, uint16_t bytes, unsigned irq_delay);
dwc2_test_snapshot_t dwc2_test_snapshot(uint8_t ep);
void dwc2_test_completion(bool (*callback)(uint8_t, uint8_t, uint32_t));
void dwc2_test_reset(void);
uint32_t dwc2_test_mode_fault(uint32_t reject);
void dwc2_test_unknown_mode(void);
bool dwc2_test_out_drop(void);
void dwc2_test_failed_in(uint8_t endpoint);
void dwc2_test_reject_out_rearm(void);
uint16_t dwc2_test_copy_pending(uint8_t endpoint, uint8_t *out, uint16_t capacity);
