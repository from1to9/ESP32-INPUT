// SPDX-License-Identifier: Apache-2.0
#include "app_touch_trace.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"

/* Tasks only. No allocation, logging, SDK call or other lock is taken while
 * this bounded observer lock is held. Bluetooth may call it under state_lock;
 * the observer never calls back into Bluetooth or the coordinator. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool collecting;
static atomic_bool started;
static bool delivered;
static app_touch_trace_snapshot_t view;
static uint64_t previous_scan, previous_sequence, previous_loop, previous_submit, busy_since;
static bool previous_single;
static struct {
    uint32_t id;
    uint64_t start_us;
    bool returned, completed;
} request;

bool app_touch_trace_active(void) { return atomic_load(&collecting); }

static void close_window(uint64_t end_us)
{
    view.end_us = end_us;
    view.outstanding = request.id && !request.completed &&
        (!request.returned || view.last_submit_error == 0);
    view.return_unobserved = request.id && !request.returned;
    atomic_store(&collecting, false);
}

static bool in_window(uint64_t now_us)
{
    if (!atomic_load(&collecting)) return false;
    if (now_us < view.start_us) { ++view.clock_errors; return false; }
    if (now_us - view.start_us >= APP_TOUCH_TRACE_WINDOW_US) {
        close_window(view.start_us + APP_TOUCH_TRACE_WINDOW_US);
        return false;
    }
    return true;
}

static void metric(app_touch_trace_metric_t kind, uint64_t us, uint64_t now_us, uint64_t source)
{
    static const uint32_t upper_us[] = {1000, 5000, 10000, 20000, 50000, 100000, 250000};
    app_touch_trace_metric_value_t *m = &view.metrics[kind];
    unsigned bucket = 0;
    while (bucket < APP_TOUCH_TRACE_BUCKETS - 1 && us > upper_us[bucket]) ++bucket;
    ++m->count; ++m->buckets[bucket];
    if (us > m->max_us || m->count == 1) {
        m->max_us = us; m->max_at_us = now_us; m->max_source = source;
    }
}

static void age(app_touch_trace_metric_t kind, uint64_t source_ms, uint64_t now_ms)
{
    if (now_ms < source_ms) { ++view.clock_errors; return; }
    metric(kind, (now_ms - source_ms) * 1000U, now_ms * 1000U, source_ms);
}

static void depth_peak(size_t depth)
{
    if (depth > view.queue_peak) view.queue_peak = (uint32_t)depth;
}

void app_touch_trace_start(uint64_t now_us, uint64_t generation, uint32_t gesture, uint64_t source_ms)
{
    if (atomic_load(&started)) return;
    portENTER_CRITICAL(&lock);
    if (!started) {
        started = true;
        view.start_us = now_us; view.generation = generation;
        view.gesture = gesture; view.trigger_source_ms = source_ms;
        atomic_store(&collecting, true);
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_scan(uint64_t sequence, uint64_t sample_ms, bool valid, bool overflow,
                          bool single_pad, bool queued, size_t depth)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    const uint64_t now_us = sample_ms * 1000U;
    if (in_window(now_us)) {
        ++view.scans; view.invalid_scans += !valid; view.overflow_scans += overflow;
        view.single_pad_scans += single_pad;
        view.contact_queue_drops += !queued; depth_peak(depth);
        if (previous_sequence && sequence > previous_sequence + 1)
            view.missing_scans += (uint32_t)(sequence - previous_sequence - 1);
        if (previous_single && single_pad && sequence == previous_sequence + 1) {
            if (now_us >= previous_scan) metric(TOUCH_TRACE_SCAN_INTERVAL, now_us - previous_scan, now_us, sequence);
            else ++view.clock_errors;
        }
        previous_scan = now_us; previous_sequence = sequence; previous_single = single_pad;
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_primary(uint64_t source_ms, app_touch_trace_phase_t phase, bool queued, size_t depth)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    if (in_window(source_ms * 1000U)) {
        ++view.primary[phase]; view.primary_drops[phase] += !queued; depth_peak(depth);
        if (phase != TOUCH_TRACE_MOVE) previous_submit = 0;
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_consume(bool contacts, uint64_t source_ms, uint64_t now_ms)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    if (in_window(now_ms * 1000U)) {
        if (contacts) ++view.contacts_consumed; else ++view.primary_consumed;
        age(contacts ? TOUCH_TRACE_CONTACT_AGE : TOUCH_TRACE_PRIMARY_AGE, source_ms, now_ms);
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_displacement(bool accepted, bool merged, uint64_t source_ms, uint64_t now_ms)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    if (in_window(now_ms * 1000U)) {
        ++view.motion_enqueues; view.motion_rejected += !accepted;
        view.motion_merged += accepted && merged;
        age(TOUCH_TRACE_MOTION_AGE, source_ms, now_ms);
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_loop(uint64_t now_ms, uint64_t generation, bool congested, bool pad_active)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    uint64_t now_us = now_ms * 1000U;
    if (in_window(now_us)) {
        if (generation != view.generation) {
            view.link_changed = true; close_window(now_us);
        } else {
            if (pad_active && previous_loop && now_us >= previous_loop)
                metric(TOUCH_TRACE_LOOP_INTERVAL, now_us - previous_loop, now_us, 0);
            previous_loop = pad_active ? now_us : 0;
            view.congested_polls += congested;
            if (congested && !busy_since) busy_since = now_us;
            if (busy_since) {
                if (now_us - busy_since > view.max_congested_us) {
                    view.max_congested_us = now_us - busy_since;
                    view.max_congested_at_us = now_us;
                }
                if (!congested) busy_since = 0;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_send(bool accepted)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    if (atomic_load(&collecting)) view.send_rejected += !accepted;
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_send_begin(bool source_known, uint64_t source_ms, uint64_t now_us)
{
    portENTER_CRITICAL(&lock);
    if (in_window(now_us)) {
        ++view.send_calls;
        if (!source_known) ++view.send_age_unknown;
        else if (source_ms * 1000U <= now_us)
            metric(TOUCH_TRACE_SEND_AGE, now_us - source_ms * 1000U, now_us, source_ms);
        else ++view.clock_errors;
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_step(unsigned result)
{
    if (!app_touch_trace_active()) return;
    portENTER_CRITICAL(&lock);
    if (atomic_load(&collecting)) ++view.steps[result];
    portEXIT_CRITICAL(&lock);
}

uint32_t app_touch_trace_ble_begin(uint64_t now_us)
{
    uint32_t id = 0;
    portENTER_CRITICAL(&lock);
    if (in_window(now_us)) {
        id = ++view.sdk_started;
        request.id = id; request.start_us = now_us; request.returned = request.completed = false;
        if (previous_submit && now_us >= previous_submit)
            metric(TOUCH_TRACE_SUBMIT_INTERVAL, now_us - previous_submit, now_us, id);
        previous_submit = now_us;
    }
    portEXIT_CRITICAL(&lock);
    return id;
}

void app_touch_trace_ble_return(uint32_t id, int32_t error, uint64_t now_us)
{
    portENTER_CRITICAL(&lock);
    if (in_window(now_us) && id && id == request.id) {
        request.returned = true; view.last_submit_error = error;
        view.sdk_accepted += error == 0; view.sdk_rejected += error != 0;
        metric(TOUCH_TRACE_SUBMIT_DURATION, now_us - request.start_us, now_us, id);
    }
    portEXIT_CRITICAL(&lock);
}

void app_touch_trace_ble_complete(uint32_t id, int32_t status, uint64_t now_us)
{
    portENTER_CRITICAL(&lock);
    if (in_window(now_us) && id && id == request.id && !request.completed) {
        request.completed = true; view.last_complete_status = status;
        view.complete_ok += status == 0; view.complete_failed += status != 0;
        view.early_complete += !request.returned;
        metric(TOUCH_TRACE_COMPLETE_DURATION, now_us - request.start_us, now_us, id);
    }
    portEXIT_CRITICAL(&lock);
}

bool app_touch_trace_take(app_touch_trace_snapshot_t *out)
{
    bool ready = false;
    portENTER_CRITICAL(&lock);
    if (started && !atomic_load(&collecting) && !delivered) {
        delivered = true; ready = true;
    }
    portEXIT_CRITICAL(&lock);
    /* Closed data is immutable; keep the snapshot copy outside the lock. */
    if (ready) *out = view;
    return ready;
}
