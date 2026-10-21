// SPDX-License-Identifier: Apache-2.0
#include "interaction_multitouch.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

typedef struct {
    interaction_multitouch_t model;
    interaction_multitouch_context_t context;
    interaction_contact_snapshot_t sample;
} fixture_t;

static fixture_t fixture(void)
{
    fixture_t f = {.context = {.epoch = 3, .ui_revision = 5, .enabled = true},
        .sample = {.epoch = 3, .ui_revision = 5, .supported = true, .fresh = true, .valid = true,
            .contacts = {{.id = 7, .x = 100, .y = 100}, {.id = 9, .x = 140, .y = 100}}}};
    interaction_multitouch_init(&f.model);
    return f;
}

static interaction_multitouch_result_t feed(fixture_t *f, uint64_t ms, uint8_t contacts)
{
    ++f->sample.sequence;
    f->sample.timestamp_ms = ms;
    f->sample.count = contacts;
    return interaction_multitouch_input(&f->model, &f->sample, &f->context, ms);
}

static void pair_tap_release_order_and_rejection_are_consumed(void)
{
    for (unsigned order = 0; order < 3; ++order) {
        fixture_t f = fixture();
        assert(!feed(&f, 10, 1).consume_pointer);
        interaction_multitouch_result_t r = feed(&f, 20, 2);
        assert(r.consume_pointer && r.cancel_single && !r.right_click && !r.release_required);
        if (order) {
            if (order == 2) f.sample.contacts[0] = f.sample.contacts[1];
            r = feed(&f, 40, 1);
            assert(r.consume_pointer && !r.right_click);
        }
        r = feed(&f, 60, 0);
        assert(r.consume_pointer && r.cancel_single && r.right_click && r.scroll_y == 0);
        const uint64_t effect_sequence = r.sequence;
        /* Simulate failed atomic click admission: the result is consumed, and
         * duplicate polling or a later all-up never retries that click. */
        r = interaction_multitouch_input(&f.model, &f.sample, &f.context, 61);
        assert(!r.right_click && r.sequence == effect_sequence);
        assert(!feed(&f, 70, 0).right_click);
        assert(!feed(&f, 80, 1).consume_pointer);
    }
}

static void scroll_fraction_order_clamp_and_pair_to_single(void)
{
    fixture_t f = fixture();
    assert(feed(&f, 10, 2).consume_pointer);
    int total = 0;
    for (unsigned i = 1; i <= 24; ++i) {
        f.sample.contacts[0].y += 1;
        f.sample.contacts[1].y += 1;
        if (i % 2) {
            const interaction_contact_t swap = f.sample.contacts[0];
            f.sample.contacts[0] = f.sample.contacts[1]; f.sample.contacts[1] = swap;
        }
        const interaction_multitouch_result_t r = feed(&f, 10 + i, 2);
        assert(r.consume_pointer && !r.right_click);
        total += r.scroll_y;
    }
    assert(total == 1);
    for (unsigned i = 0; i < 24; ++i) {
        f.sample.contacts[0].y -= 1;
        f.sample.contacts[1].y -= 1;
        total += feed(&f, 40 + i, 2).scroll_y;
    }
    assert(total == 0);
    f.sample.contacts[0].y = 4096; f.sample.contacts[1].y = 4096;
    assert(feed(&f, 70, 2).scroll_y == 127);
    assert(feed(&f, 80, 2).scroll_y == 0); /* Drop excess instead of replay. */
    assert(feed(&f, 90, 1).consume_pointer);
    f.sample.contacts[0].x += 100;
    interaction_multitouch_result_t r = feed(&f, 100, 1);
    assert(r.consume_pointer && !r.right_click && !r.scroll_y);
    assert(!feed(&f, 110, 0).right_click);
    assert(!feed(&f, 120, 1).consume_pointer);
}

static void motion_excursion_join_and_release_deadlines(void)
{
    for (unsigned fault = 0; fault < 7; ++fault) {
        fixture_t f = fixture();
        (void)feed(&f, 10, 1);
        if (fault == 0) { f.sample.contacts[0].x += 9; (void)feed(&f, 20, 1); }
        if (fault == 0) f.sample.contacts[0].x -= 9;
        if (fault == 5) { f.sample.contacts[0].x += 7; (void)feed(&f, 20, 1); }
        (void)feed(&f, fault == 1 ? 91 : 30, 2);
        if (fault == 5) { f.sample.contacts[0].x += 7; (void)feed(&f, 40, 2); }
        if (fault == 6) {
            f.sample.contacts[0].x += 7; (void)feed(&f, 40, 1);
            f.sample.contacts[0].x += 7; (void)feed(&f, 50, 1);
        }
        if (fault == 2) {
            f.sample.contacts[0].x += 9; (void)feed(&f, 40, 2);
            f.sample.contacts[0].x -= 9;
        }
        if (fault == 3) {
            (void)feed(&f, 100, 2); (void)feed(&f, 190, 2);
            (void)feed(&f, 250, 2);
        }
        if (fault == 4) {
            (void)feed(&f, 40, 1); (void)feed(&f, 110, 1);
        }
        const interaction_multitouch_result_t r = feed(&f, fault == 3 ? 261 : fault == 4 ? 121 : 99, 0);
        assert(!r.right_click && !r.scroll_y);
    }
}

static void invalid_missing_stale_and_unavailable_evidence(void)
{
    for (unsigned fault = 0; fault < 12; ++fault) {
        fixture_t f = fixture();
        (void)feed(&f, 10, 2);
        f.sample.timestamp_ms = 20; ++f.sample.sequence;
        if (fault == 0) f.sample.contacts[1].id = f.sample.contacts[0].id;
        if (fault == 1) f.sample.count = 3;
        if (fault == 2) f.sample.overflow = true;
        if (fault == 3) f.sample.valid = false;
        if (fault == 4) f.sample.contacts[0].x = NAN;
        if (fault == 5) f.sample.contacts[0].y = 4097;
        if (fault == 6) f.sample.timestamp_ms = 21;
        if (fault == 7) f.sample.timestamp_ms = 9;
        if (fault == 8) ++f.sample.sequence;
        if (fault == 9) f.sample.supported = false;
        if (fault == 10) f.context.ui_revision++;
        if (fault == 11) f.context.enabled = false;
        interaction_multitouch_result_t r = interaction_multitouch_input(&f.model, &f.sample, &f.context, 20);
        assert(r.consume_pointer && !r.right_click && !r.scroll_y);
        f.sample.valid = true; f.sample.overflow = false; f.sample.supported = true;
        f.sample.epoch = f.context.epoch; f.sample.ui_revision = f.context.ui_revision;
        f.sample.contacts[0].x = 100; f.sample.contacts[0].y = 100;
        assert(!feed(&f, 30, 0).right_click);
    }
    fixture_t f = fixture();
    (void)feed(&f, 10, 2);
    f.sample.fresh = false;
    for (uint64_t now = 20; now < 110; now += 10) {
        const interaction_multitouch_result_t r = interaction_multitouch_input(&f.model, &f.sample, &f.context, now);
        assert(r.consume_pointer && !r.right_click);
    }
    assert(interaction_multitouch_tick(&f.model, &f.context, 110).consume_pointer);
    f.sample.fresh = true;
    assert(!feed(&f, 120, 0).right_click);
    (void)feed(&f, 130, 2);
    f.sample.sequence--; /* Old release cannot end the active new pair. */
    f.sample.count = 0; f.sample.timestamp_ms = 140;
    assert(interaction_multitouch_input(&f.model, &f.sample, &f.context, 140).consume_pointer);
    assert(f.model.phase == INTERACTION_MULTI_PAIR && f.model.contact_ms == 130);
}

static void held_cleanup_preferences_clock_and_exhaustion(void)
{
    fixture_t f = fixture();
    f.context.drag_held = true;
    interaction_multitouch_result_t r = feed(&f, 10, 2);
    assert(r.release_required && r.cancel_single && r.consume_pointer && !r.scroll_y);
    f.sample.contacts[0].y += 100; f.sample.contacts[1].y += 100;
    assert(feed(&f, 20, 2).release_required); /* Failed release admission stays latched. */
    assert(feed(&f, 30, 0).release_required);
    interaction_multitouch_release_owned(&f.model);
    f.context.drag_held = false;
    f.context.enabled = false;
    r = feed(&f, 40, 2);
    assert(!r.consume_pointer && !r.right_click && !r.scroll_y); /* Movement-only. */
    f.context.enabled = true;
    (void)feed(&f, 50, 2);
    assert(interaction_multitouch_tick(&f.model, &f.context, 49).consume_pointer);
    assert(!feed(&f, 60, 0).right_click);
    (void)feed(&f, 70, 2);
    f.sample.sequence = UINT64_MAX - 1;
    (void)feed(&f, 80, 2);
    assert(f.model.exhausted);
    assert(!feed(&f, 90, 0).right_click); /* Wrapping provider sequence fails closed. */
    assert(f.model.exhausted);
}

int main(void)
{
    pair_tap_release_order_and_rejection_are_consumed();
    scroll_fraction_order_clamp_and_pair_to_single();
    motion_excursion_join_and_release_deadlines();
    invalid_missing_stale_and_unavailable_evidence();
    held_cleanup_preferences_clock_and_exhaustion();
    puts("PASS: multi-contact tap/scroll, contact identity, cleanup, freshness, interruption and no replay");
    return 0;
}
