// SPDX-License-Identifier: Apache-2.0
#include "interaction_multitouch.h"
#include <math.h>
#include <string.h>

static bool consuming(const interaction_multitouch_t *m)
{
    return m->phase >= INTERACTION_MULTI_PAIR || m->release_required || m->exhausted;
}

static interaction_multitouch_result_t result(const interaction_multitouch_t *m)
{
    return (interaction_multitouch_result_t){.consume_pointer = consuming(m),
        .cancel_single = consuming(m), .release_required = m->release_required, .sequence = m->sequence};
}

void interaction_multitouch_init(interaction_multitouch_t *m)
{
    *m = (interaction_multitouch_t){0};
}

interaction_multitouch_result_t interaction_multitouch_cancel(interaction_multitouch_t *m)
{
    if (m->phase != INTERACTION_MULTI_IDLE) m->phase = INTERACTION_MULTI_DRAIN;
    m->tap_candidate = false;
    m->carry = 0;
    return result(m);
}

void interaction_multitouch_release_owned(interaction_multitouch_t *m)
{
    m->release_required = false;
}

interaction_multitouch_result_t interaction_multitouch_tick(interaction_multitouch_t *m,
    const interaction_multitouch_context_t *c, uint64_t now_ms)
{
    const bool boundary = m->context_seen && (m->epoch != c->epoch || m->ui_revision != c->ui_revision);
    const bool regressed = m->clock_seen && now_ms < m->now_ms;
    m->context_seen = true; m->clock_seen = true;
    m->epoch = c->epoch; m->ui_revision = c->ui_revision; m->now_ms = now_ms;
    const bool expired = m->phase != INTERACTION_MULTI_IDLE &&
        (now_ms < m->contact_ms || now_ms - m->contact_ms >= INTERACTION_CONTACT_TIMEOUT_MS ||
         now_ms < m->started_ms || now_ms - m->started_ms >= 30000);
    if (boundary || regressed || expired || !c->enabled) {
        (void)interaction_multitouch_cancel(m);
        if (consuming(m) && c->drag_held) m->release_required = true;
    }
    return result(m);
}

static bool contacts_valid(const interaction_contact_snapshot_t *s)
{
    if (!s->valid || s->overflow || s->count > INTERACTION_CONTACT_CAPACITY) return false;
    for (uint8_t i = 0; i < s->count; ++i)
        if (!isfinite(s->contacts[i].x) || !isfinite(s->contacts[i].y) ||
            fabsf(s->contacts[i].x) > 4096 || fabsf(s->contacts[i].y) > 4096) return false;
    return s->count != 2 || s->contacts[0].id != s->contacts[1].id;
}

static int find_contact(const interaction_contact_snapshot_t *s, uint8_t id)
{
    for (uint8_t i = 0; i < s->count; ++i) if (s->contacts[i].id == id) return i;
    return -1;
}

static bool moved(float x, float y, float start_x, float start_y)
{
    return hypotf(x - start_x, y - start_y) > 8;
}

interaction_multitouch_result_t interaction_multitouch_input(interaction_multitouch_t *m,
    const interaction_contact_snapshot_t *s, const interaction_multitouch_context_t *c, uint64_t now_ms)
{
    (void)interaction_multitouch_tick(m, c, now_ms);
    if (!s || !s->supported) {
        (void)interaction_multitouch_cancel(m);
        return result(m);
    }
    if (!s->fresh || s->epoch != c->epoch || s->ui_revision != c->ui_revision) return result(m);
    if (m->exhausted || !s->sequence || (m->sequence_seen && s->sequence <= m->sequence))
        return result(m);
    const bool gap = m->sequence_seen && s->sequence - m->sequence > 1;
    m->sequence = s->sequence; m->sequence_seen = true;
    if (s->sequence == UINT64_MAX) {
        m->exhausted = true;
        if (c->drag_held) m->release_required = true;
    }
    if (!contacts_valid(s) || s->timestamp_ms > now_ms ||
        now_ms - s->timestamp_ms >= INTERACTION_CONTACT_TIMEOUT_MS ||
        s->timestamp_ms < m->contact_ms || (m->phase != INTERACTION_MULTI_IDLE && gap)) {
        if (m->phase == INTERACTION_MULTI_IDLE) m->phase = INTERACTION_MULTI_DRAIN;
        (void)interaction_multitouch_cancel(m);
        if (c->drag_held) m->release_required = true;
        return result(m);
    }
    m->contact_ms = s->timestamp_ms;
    if (!s->count) {
        const bool click = c->enabled && !m->release_required && m->tap_candidate &&
            (m->phase == INTERACTION_MULTI_PAIR || m->phase == INTERACTION_MULTI_PAIR_RELEASE) &&
            s->timestamp_ms - m->started_ms <= 250 &&
            (m->phase != INTERACTION_MULTI_PAIR_RELEASE || s->timestamp_ms - m->release_ms <= 80);
        const bool consumed = consuming(m);
        m->phase = INTERACTION_MULTI_IDLE; m->tap_candidate = false; m->carry = 0;
        interaction_multitouch_result_t r = result(m);
        r.consume_pointer |= consumed; r.cancel_single |= consumed;
        r.right_click = click && !m->exhausted;
        return r;
    }
    if (!c->enabled || m->phase == INTERACTION_MULTI_DRAIN || m->release_required || m->exhausted)
        return result(m);
    if (m->phase == INTERACTION_MULTI_IDLE) {
        m->phase = INTERACTION_MULTI_SINGLE;
        m->started_ms = s->timestamp_ms; m->tap_candidate = true;
        m->ids[0] = s->contacts[0].id;
        m->first_x = s->contacts[0].x; m->first_y = s->contacts[0].y;
    }
    if (m->phase == INTERACTION_MULTI_SINGLE) {
        const int first = find_contact(s, m->ids[0]);
        if (first < 0) {
            m->phase = INTERACTION_MULTI_DRAIN;
            if (c->drag_held) m->release_required = true;
            return result(m);
        }
        m->tap_candidate = m->tap_candidate &&
            !moved(s->contacts[first].x, s->contacts[first].y, m->first_x, m->first_y);
        if (s->count < 2) return result(m);
        m->tap_candidate = m->tap_candidate && s->timestamp_ms - m->started_ms <= 80;
        m->phase = INTERACTION_MULTI_PAIR;
        const uint8_t first_id = m->ids[0];
        for (size_t i = 0; i < 2; ++i) {
            m->ids[i] = s->contacts[i].id;
            m->point_x[i] = s->contacts[i].id == first_id ? m->first_x : s->contacts[i].x;
            m->point_y[i] = s->contacts[i].id == first_id ? m->first_y : s->contacts[i].y;
        }
        m->anchor_x = (s->contacts[0].x + s->contacts[1].x) / 2;
        m->anchor_y = (s->contacts[0].y + s->contacts[1].y) / 2;
        m->last_y = m->anchor_y;
        if (c->drag_held) {
            m->release_required = true;
            (void)interaction_multitouch_cancel(m);
        }
        return result(m);
    }
    if (m->phase == INTERACTION_MULTI_PAIR_RELEASE) {
        const int remaining = find_contact(s, m->ids[0]);
        if (s->count != 1 || remaining < 0 || s->timestamp_ms - m->release_ms > 80 ||
            moved(s->contacts[remaining].x, s->contacts[remaining].y, m->point_x[0], m->point_y[0]))
            (void)interaction_multitouch_cancel(m);
        return result(m);
    }
    const int first = find_contact(s, m->ids[0]), second = find_contact(s, m->ids[1]);
    if (s->count == 1 && (first >= 0 || second >= 0)) {
        const int remaining = first >= 0 ? first : second;
        const size_t original = first >= 0 ? 0U : 1U;
        m->tap_candidate = m->tap_candidate && m->phase == INTERACTION_MULTI_PAIR &&
            !moved(s->contacts[remaining].x, s->contacts[remaining].y,
                   m->point_x[original], m->point_y[original]);
        m->ids[0] = s->contacts[remaining].id;
        m->point_x[0] = m->point_x[original]; m->point_y[0] = m->point_y[original];
        m->phase = INTERACTION_MULTI_PAIR_RELEASE;
        m->release_ms = s->timestamp_ms; m->carry = 0;
        return result(m);
    }
    if (first < 0 || second < 0) {
        (void)interaction_multitouch_cancel(m);
        return result(m);
    }
    for (size_t i = 0; i < 2; ++i) {
        const int index = i ? second : first;
        m->tap_candidate = m->tap_candidate && !moved(s->contacts[index].x, s->contacts[index].y,
                                                     m->point_x[i], m->point_y[i]);
    }
    const float x = (s->contacts[first].x + s->contacts[second].x) / 2;
    const float y = (s->contacts[first].y + s->contacts[second].y) / 2;
    if (m->phase == INTERACTION_MULTI_PAIR && moved(x, y, m->anchor_x, m->anchor_y)) {
        m->phase = INTERACTION_MULTI_SCROLL; m->tap_candidate = false;
    }
    interaction_multitouch_result_t r = result(m);
    if (m->phase == INTERACTION_MULTI_SCROLL) {
        const float total = m->carry + (y - m->last_y) / 24;
        if (total >= 127) { r.scroll_y = 127; m->carry = 0; }
        else if (total <= -127) { r.scroll_y = -127; m->carry = 0; }
        else { r.scroll_y = (int8_t)total; m->carry = total - (float)r.scroll_y; }
        m->last_y = y;
    }
    return r;
}
