// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if CONFIG_APP_A2DP_ACL_DIAGNOSTICS
typedef enum { BT_ACL_WAITING, BT_ACL_ACTIVE, BT_ACL_CLOSED, BT_ACL_TIME_LIMIT, BT_ACL_HANDLE_CHANGE } bt_acl_state_t;

typedef struct {
    uint32_t fragments, completed, replaced, orphan, invalid;
} bt_acl_stage_t;
typedef struct {
    uint32_t packets, gaps, skipped, nonforward;
} bt_acl_sequence_t;
typedef struct {
    bt_acl_state_t state;
    bt_acl_stage_t input, dequeue;
    bt_acl_sequence_t encoded, decode;
    uint32_t host_unready, alloc_failed, queued, enqueue_failed, credits_submitted;
    uint32_t btu_posts, btu_rejected, btu_peak, btu_high_water_posts; /* Shared ACL queue. */
    uint32_t queue_samples, handoff_over_10ms, max_handoff_us, max_admission_us;
} bt_acl_diag_t;

/* Called only on actual grant changes. Capture once per diagnostic boot. */
void bt_acl_diag_binding(bool a2dp_enabled);
void bt_acl_diag_snapshot(bt_acl_diag_t *out);

/* Pinned SDK hooks. No packet data or addresses are copied to the snapshot. */
int64_t bt_acl_diag_input(const uint8_t *h4, uint16_t len);
void bt_acl_diag_input_failed(bool allocation);
void bt_acl_diag_before_enqueue(uintptr_t packet);
void bt_acl_diag_after_enqueue(int64_t arrival_us, uintptr_t packet, bool queued);
void bt_acl_diag_dequeue(const uint8_t *h4, uint16_t len, uintptr_t packet);
void bt_acl_diag_btu_post(bool accepted, uint32_t depth, uint32_t high_water);
void bt_acl_diag_credit_command(const uint8_t *command, uint16_t len);
void bt_acl_diag_sequence(bool decoder, uint16_t sequence);
#else
static inline void bt_acl_diag_binding(bool a2dp_enabled) { (void)a2dp_enabled; }
#endif
