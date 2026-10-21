// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTERACTION_CONTACT_CAPACITY 2U
#define INTERACTION_CONTACT_TIMEOUT_MS 100U

typedef struct { uint8_t id; float x, y; } interaction_contact_t;
typedef struct {
    uint64_t sequence, timestamp_ms, epoch, ui_revision;
    bool supported, fresh, valid, overflow;
    uint8_t count;
    interaction_contact_t contacts[INTERACTION_CONTACT_CAPACITY];
} interaction_contact_snapshot_t;
typedef struct {
    uint64_t epoch, ui_revision;
    /* enabled requires a real multi-contact provider, eligible local pad and
     * the user's gesture preference. Movement-only must pass enabled=false. */
    bool enabled, drag_held;
} interaction_multitouch_context_t;
typedef struct {
    bool consume_pointer, cancel_single, release_required, right_click;
    int8_t scroll_y; /* Positive centroid Y produces positive wheel units. */
    uint64_t sequence; /* Effect belongs to this consumed fresh snapshot only. */
} interaction_multitouch_result_t;
typedef enum {
    INTERACTION_MULTI_IDLE, INTERACTION_MULTI_SINGLE, INTERACTION_MULTI_PAIR,
    INTERACTION_MULTI_PAIR_RELEASE, INTERACTION_MULTI_SCROLL, INTERACTION_MULTI_DRAIN
} interaction_multitouch_phase_t;
typedef struct {
    interaction_multitouch_phase_t phase;
    uint64_t epoch, ui_revision, sequence, now_ms, contact_ms, started_ms, release_ms;
    bool context_seen, clock_seen, sequence_seen, tap_candidate, release_required, exhausted;
    uint8_t ids[2];
    float first_x, first_y, anchor_x, anchor_y, last_y, carry;
    float point_x[2], point_y[2];
} interaction_multitouch_t;

/* Fixed storage, one serialized owner. Input is ONE fresh complete snapshot
 * from the existing touch reader, before routing a primary pointer event.
 * IDs are stable until release; order may change. Never create freshness from
 * cached LVGL PRESSING, or add an independent read that consumes the same data.
 * Two fingers cancel one-finger recognition and consume primary pointer input.
 * Scroll commits after 8 px centroid travel, at 24 px per wheel unit. A still
 * pair <=250 ms gives one right-click; second finger joins within 80 ms and
 * both release within 80 ms. Pair-to-one never becomes pointer/tap input.
 * Contact expires at 100 ms; total gesture at 30 s. Errors drain until fresh
 * all-up; duplicates/old snapshots cannot refresh or release a newer gesture.
 * Apply each returned effect once. Rejected scroll/click admission is consumed
 * without retry. Right-click must be one atomic down/up pair in the HID owner.
 */
void interaction_multitouch_init(interaction_multitouch_t *model);
interaction_multitouch_result_t interaction_multitouch_input(interaction_multitouch_t *,
    const interaction_contact_snapshot_t *, const interaction_multitouch_context_t *, uint64_t now_ms);
interaction_multitouch_result_t interaction_multitouch_tick(interaction_multitouch_t *,
    const interaction_multitouch_context_t *, uint64_t now_ms);
interaction_multitouch_result_t interaction_multitouch_cancel(interaction_multitouch_t *);
/* Only after the coordinator owns held-button neutralization. This does not
 * assert transport delivery; the HID release gate still blocks output. */
void interaction_multitouch_release_owned(interaction_multitouch_t *);
