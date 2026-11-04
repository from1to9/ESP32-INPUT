// SPDX-License-Identifier: Apache-2.0
#include "bt_acl_diag.h"
#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define CAPTURE_US INT64_C(180000000)

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bt_acl_diag_t view;
static atomic_bool capturing; /* Fast off path for SDK callbacks. */
static int64_t deadline, sampled_since;
static uintptr_t sampled_packet;
static uint16_t classic_handle, last_sequence[2];
static struct {
    uint32_t remaining;
    bool seen_start;
} assembly[2];

/* Only this observer lock is acquired. Never enter a Bluetooth, application,
 * logging or allocator path while holding it. Only one capture can be armed. */
static bool active(int64_t now)
{
    if (view.state == BT_ACL_ACTIVE && now >= deadline) {
        view.state = BT_ACL_TIME_LIMIT;
        atomic_store(&capturing, false);
        sampled_packet = 0;
    }
    return view.state == BT_ACL_ACTIVE;
}

void bt_acl_diag_binding(bool enabled)
{
    portENTER_CRITICAL(&lock);
    if (active(esp_timer_get_time())) {
        view.state = BT_ACL_CLOSED;
        atomic_store(&capturing, false);
        sampled_packet = 0;
    } else if (enabled && view.state == BT_ACL_WAITING) {
        deadline = esp_timer_get_time() + CAPTURE_US;
        view.state = BT_ACL_ACTIVE;
        atomic_store(&capturing, true);
    }
    portEXIT_CRITICAL(&lock);
}

void bt_acl_diag_snapshot(bt_acl_diag_t *out)
{
    portENTER_CRITICAL(&lock);
    active(esp_timer_get_time());
    *out = view;
    portEXIT_CRITICAL(&lock);
}

static uint16_t little16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* S31 porting_btdm reserves handle bit 11 for BR/EDR. Inspect only the H4
 * type and ACL header, plus the two L2CAP length bytes on a START. */
static bool classic_acl(const uint8_t *p, uint16_t len)
{
    return len >= 5 && p[0] == 2 && (p[2] & 0x08U);
}

static void fragment(unsigned stage, const uint8_t *p, uint16_t len)
{
    uint16_t flags = little16(p + 1);
    uint16_t handle = flags & 0x0fffU;
    if (classic_handle && classic_handle != handle) {
        view.state = BT_ACL_HANDLE_CHANGE;
        atomic_store(&capturing, false);
        sampled_packet = 0;
        return;
    }
    classic_handle = handle;
    bt_acl_stage_t *s = stage ? &view.dequeue : &view.input;
    uint16_t bytes = little16(p + 3);
    unsigned pb = (flags >> 12) & 3U;
    ++s->fragments;
    if (bytes != len - 5U || (pb != 1 && pb != 2) || (pb == 2 && (bytes < 2 || little16(p + 5) > UINT16_MAX - 8U))) {
        ++s->invalid;
        assembly[stage].seen_start = false;
        assembly[stage].remaining = 0;
        return;
    }
    if (pb == 2) {
        if (assembly[stage].remaining) ++s->replaced;
        assembly[stage].seen_start = true;
        assembly[stage].remaining = 4U + little16(p + 5);
    } else if (!assembly[stage].remaining) {
        /* The first observed fragment may be a continuation across arm. */
        if (assembly[stage].seen_start) ++s->orphan;
        return;
    }
    if (bytes > assembly[stage].remaining) {
        ++s->invalid;
        assembly[stage].remaining = 0;
        return;
    }
    assembly[stage].remaining -= bytes;
    if (!assembly[stage].remaining) ++s->completed;
}

int64_t bt_acl_diag_input(const uint8_t *h4, uint16_t len)
{
    if (!atomic_load_explicit(&capturing, memory_order_relaxed) || !classic_acl(h4, len)) return 0;
    portENTER_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    if (active(now)) fragment(0, h4, len);
    bool tracked = view.state == BT_ACL_ACTIVE;
    portEXIT_CRITICAL(&lock);
    return tracked ? now : 0;
}

void bt_acl_diag_input_failed(bool allocation)
{
    portENTER_CRITICAL(&lock);
    if (active(esp_timer_get_time())) {
        if (allocation) ++view.alloc_failed;
        else ++view.host_unready;
    }
    portEXIT_CRITICAL(&lock);
}

void bt_acl_diag_before_enqueue(uintptr_t packet)
{
    portENTER_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    if (active(now) && !sampled_packet) {
        sampled_packet = packet;
        sampled_since = now;
    }
    portEXIT_CRITICAL(&lock);
}

void bt_acl_diag_after_enqueue(int64_t arrival_us, uintptr_t packet, bool queued)
{
    portENTER_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    if (active(now)) {
        uint32_t elapsed = (uint32_t)(now - arrival_us);
        if (elapsed > view.max_admission_us) view.max_admission_us = elapsed;
        if (queued) ++view.queued;
        else {
            ++view.enqueue_failed;
            if (sampled_packet == packet) sampled_packet = 0;
        }
    }
    portEXIT_CRITICAL(&lock);
}

void bt_acl_diag_dequeue(const uint8_t *h4, uint16_t len, uintptr_t packet)
{
    if (!atomic_load_explicit(&capturing, memory_order_relaxed) || !classic_acl(h4, len)) return;
    portENTER_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    if (active(now)) {
        fragment(1, h4, len);
        if (sampled_packet == packet) {
            uint32_t elapsed = (uint32_t)(now - sampled_since);
            if (elapsed > view.max_handoff_us) view.max_handoff_us = elapsed;
            if (elapsed > 10000U) ++view.handoff_over_10ms;
            ++view.queue_samples;
            sampled_packet = 0;
        }
    }
    portEXIT_CRITICAL(&lock);
}

/* BTU pressure applies to the shared reassembled ACL queue, including LE.
 * Use the depth returned by the SDK enqueue, without another queue read. */
void bt_acl_diag_btu_post(bool accepted, uint32_t depth, uint32_t high_water)
{
    if (!atomic_load_explicit(&capturing, memory_order_relaxed)) return;
    portENTER_CRITICAL(&lock);
    if (active(esp_timer_get_time())) {
        if (accepted) {
            ++view.btu_posts;
            if (depth > view.btu_peak) view.btu_peak = depth;
            if (depth >= high_water) ++view.btu_high_water_posts;
        } else ++view.btu_rejected;
    }
    portEXIT_CRITICAL(&lock);
}

/* HCI Host Number Of Completed Packets submitted to the controller. Only
 * the bounded handle/count entries of this single opcode are inspected. */
void bt_acl_diag_credit_command(const uint8_t *p, uint16_t len)
{
    if (!atomic_load_explicit(&capturing, memory_order_relaxed) ||
        len < 4 || little16(p) != 0x0c35 || p[2] != len - 3U || len != 4U + 4U * p[3]) return;
    portENTER_CRITICAL(&lock);
    if (active(esp_timer_get_time())) {
        for (unsigned i = 4; i < len; i += 4) {
            if (classic_handle && little16(p + i) == classic_handle)
                view.credits_submitted += little16(p + i + 2);
        }
    }
    portEXIT_CRITICAL(&lock);
}

void bt_acl_diag_sequence(bool decoder, uint16_t sequence)
{
    if (!atomic_load_explicit(&capturing, memory_order_relaxed)) return;
    portENTER_CRITICAL(&lock);
    if (active(esp_timer_get_time())) {
        bt_acl_sequence_t *s = decoder ? &view.decode : &view.encoded;
        if (s->packets) {
            uint16_t distance = (uint16_t)(sequence - last_sequence[decoder]);
            if (!distance || distance >= 0x8000U) ++s->nonforward;
            else if (distance > 1) { ++s->gaps; s->skipped += distance - 1U; }
        }
        ++s->packets;
        last_sequence[decoder] = sequence;
    }
    portEXIT_CRITICAL(&lock);
}
