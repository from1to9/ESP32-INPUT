// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bt_acl_diag.h"

static int64_t clock_us = 1000;
int64_t esp_timer_get_time(void) { return clock_us; }

static bt_acl_diag_t snapshot(void)
{
    bt_acl_diag_t v;
    bt_acl_diag_snapshot(&v);
    return v;
}

/* H4 type, Classic handle 0x801 with PB flags, ACL length, L2CAP length.
 * A continuation has only its ACL length inspected. */
static const uint8_t start[] = {2,1,0x28,4,0,2,0,0x40,0};
static const uint8_t cont[] = {2,1,0x18,2,0,0,0};

static void packet(const uint8_t *data, uint16_t len, bool queued)
{
    const uintptr_t id = 1; /* Reuse after completion/rejection. */
    int64_t arrival = bt_acl_diag_input(data, len);
    assert(arrival == clock_us);
    clock_us += 2;
    bt_acl_diag_before_enqueue(id);
    clock_us += 3;
    bt_acl_diag_after_enqueue(arrival, id, queued);
    if (queued) {
        clock_us += 11000;
        bt_acl_diag_dequeue(data, len, id);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !bt_acl_diag_input(start, sizeof(start)));
    bt_acl_diag_binding(false);
    assert(snapshot().state == BT_ACL_WAITING);
    bt_acl_diag_binding(true);
    if (!strcmp(argv[1], "before-host") || !strcmp(argv[1], "enqueue")) {
        const bool fail = !strcmp(argv[1], "enqueue");
        packet(start, sizeof(start), true);
        if (fail) packet(cont, sizeof(cont), false);
        packet(start, sizeof(start), true);
        packet(cont, sizeof(cont), true);
        bt_acl_diag_t v = snapshot();
        assert(v.dequeue.replaced == 1 && v.dequeue.completed == 1 && !v.dequeue.orphan && !v.dequeue.invalid);
        assert(v.input.completed == (fail ? 2U : 1U) && v.input.replaced == (fail ? 0U : 1U));
        assert(v.enqueue_failed == (fail ? 1U : 0U) && v.queued == 3 && v.dequeue.fragments == 3);
        assert(v.queue_samples == 3 && v.handoff_over_10ms == 3 && v.max_handoff_us == 11003 && v.max_admission_us == 5);
    } else if (!strcmp(argv[1], "headers")) {
        const uint8_t ble[] = {2,1,0x20,0,0}, key_event[] = {4,0x18,2,0,0};
        assert(!bt_acl_diag_input(ble, sizeof(ble)) && !bt_acl_diag_input(key_event, sizeof(key_event)));
        for (uint16_t i = 0; i < 5; ++i) assert(!bt_acl_diag_input(start, i));
        packet(cont, sizeof(cont), true); /* Window opens mid-PDU. */
        packet(start, sizeof(start), true); packet(cont, sizeof(cont), true);
        packet(cont, sizeof(cont), true); packet(start, 6, true);
        const uint8_t credit[] = {0x35,0x0c,9,2,1,0,12,0,1,8,3,0};
        bt_acl_diag_credit_command(credit, sizeof(credit)); bt_acl_diag_credit_command(credit, 6);
        bt_acl_diag_input_failed(false); bt_acl_diag_input_failed(true);
        bt_acl_diag_btu_post(true, 2, 75); bt_acl_diag_btu_post(true, 75, 75);
        bt_acl_diag_btu_post(false, 0, 75);
        const uint16_t seq[] = {65535,0,3,3};
        for (unsigned i = 0; i < 4; ++i) {
            bt_acl_diag_sequence(false, seq[i]);
            if (i != 1) bt_acl_diag_sequence(true, seq[i]);
        }
        bt_acl_diag_t v = snapshot();
        assert(v.input.fragments == 5 && v.input.orphan == 1 && v.input.invalid == 1 && v.input.completed == 1);
        assert(v.btu_posts == 2 && v.btu_rejected == 1 && v.btu_peak == 75 && v.btu_high_water_posts == 1);
        assert(v.credits_submitted == 3 && v.host_unready == 1 && v.alloc_failed == 1);
        assert(v.encoded.packets == 4 && v.encoded.gaps == 1 && v.encoded.skipped == 2 && v.encoded.nonforward == 1);
        assert(v.decode.packets == 3 && v.decode.gaps == 1 && v.decode.skipped == 3 && v.decode.nonforward == 1);
    } else if (!strcmp(argv[1], "window")) {
        packet(start, sizeof(start), true);
        clock_us += 180000000;
        assert(!bt_acl_diag_input(cont, sizeof(cont)));
        assert(snapshot().state == BT_ACL_TIME_LIMIT);
    } else {
        assert(!strcmp(argv[1], "handle"));
        packet(start, sizeof(start), true);
        uint8_t other[sizeof(start)]; memcpy(other, start, sizeof(start)); other[1] = 2;
        assert(!bt_acl_diag_input(other, sizeof(other)) && snapshot().state == BT_ACL_HANDLE_CHANGE);
    }
    bt_acl_diag_binding(false);
    bt_acl_diag_t prior = snapshot();
    bt_acl_diag_binding(true);
    assert(!bt_acl_diag_input(start, sizeof(start)));
    bt_acl_diag_sequence(false, 123);
    bt_acl_diag_t after = snapshot();
    assert(prior.state == after.state && prior.input.fragments == after.input.fragments &&
        prior.encoded.packets == after.encoded.packets && prior.state != BT_ACL_ACTIVE);
    printf("PASS: bounded ACL diagnostic %s\n", argv[1]);
    return 0;
}
