// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_APP_TOUCH_LATENCY_DIAGNOSTICS) && CONFIG_APP_TOUCH_LATENCY_DIAGNOSTICS
#define APP_TOUCH_TRACE_ENABLED 1
#define APP_TOUCH_TRACE_WINDOW_US UINT64_C(20000000)
#define APP_TOUCH_TRACE_BUCKETS 8
typedef enum {
    TOUCH_TRACE_SCAN_INTERVAL, TOUCH_TRACE_PRIMARY_AGE, TOUCH_TRACE_CONTACT_AGE,
    TOUCH_TRACE_LOOP_INTERVAL, TOUCH_TRACE_MOTION_AGE, TOUCH_TRACE_SUBMIT_INTERVAL,
    TOUCH_TRACE_SUBMIT_DURATION, TOUCH_TRACE_COMPLETE_DURATION, TOUCH_TRACE_SEND_AGE, TOUCH_TRACE_METRICS
} app_touch_trace_metric_t;
typedef enum { TOUCH_TRACE_BEGIN, TOUCH_TRACE_MOVE, TOUCH_TRACE_END } app_touch_trace_phase_t;
typedef struct {
    uint32_t count, buckets[APP_TOUCH_TRACE_BUCKETS];
    uint64_t max_us, max_at_us, max_source;
} app_touch_trace_metric_value_t;
typedef struct {
    uint64_t start_us, end_us, generation, trigger_source_ms;
    uint32_t gesture;
    bool link_changed, outstanding, return_unobserved;
    uint32_t scans, single_pad_scans, invalid_scans, overflow_scans, missing_scans, contact_queue_drops;
    uint32_t primary[3], primary_drops[3], contacts_consumed, primary_consumed, queue_peak;
    uint32_t motion_enqueues, motion_merged, motion_rejected, send_calls, send_rejected, send_age_unknown;
    uint32_t steps[5]; /* hid_engine_step_result_t order, checked by the caller. */
    uint32_t sdk_started, sdk_accepted, sdk_rejected, complete_ok, complete_failed, early_complete;
    uint32_t clock_errors, congested_polls;
    int32_t last_submit_error, last_complete_status;
    uint64_t max_congested_us, max_congested_at_us;
    app_touch_trace_metric_value_t metrics[TOUCH_TRACE_METRICS];
} app_touch_trace_snapshot_t;

bool app_touch_trace_active(void);
void app_touch_trace_start(uint64_t now_us, uint64_t generation, uint32_t gesture, uint64_t source_ms);
void app_touch_trace_scan(uint64_t sequence, uint64_t sample_ms, bool valid, bool overflow,
                          bool single_pad, bool queued, size_t depth);
void app_touch_trace_primary(uint64_t source_ms, app_touch_trace_phase_t phase, bool queued, size_t depth);
void app_touch_trace_consume(bool contacts, uint64_t source_ms, uint64_t now_ms);
void app_touch_trace_displacement(bool accepted, bool merged, uint64_t source_ms, uint64_t now_ms);
void app_touch_trace_loop(uint64_t now_ms, uint64_t generation, bool congested, bool pad_active);
void app_touch_trace_send(bool accepted);
void app_touch_trace_send_begin(bool source_known, uint64_t source_ms, uint64_t now_us);
void app_touch_trace_step(unsigned result);
uint32_t app_touch_trace_ble_begin(uint64_t now_us);
void app_touch_trace_ble_return(uint32_t id, int32_t error, uint64_t now_us);
void app_touch_trace_ble_complete(uint32_t id, int32_t status, uint64_t now_us);
bool app_touch_trace_take(app_touch_trace_snapshot_t *out);
#else
#define APP_TOUCH_TRACE_ENABLED 0
#define app_touch_trace_active() false
#define app_touch_trace_start(...) ((void)0)
#define app_touch_trace_scan(...) ((void)0)
#define app_touch_trace_primary(...) ((void)0)
#define app_touch_trace_consume(...) ((void)0)
#define app_touch_trace_displacement(...) ((void)0)
#define app_touch_trace_loop(...) ((void)0)
#define app_touch_trace_send(...) ((void)0)
#define app_touch_trace_send_begin(...) ((void)0)
#define app_touch_trace_step(...) ((void)0)
#endif
