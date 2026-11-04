// SPDX-License-Identifier: Apache-2.0
#include "app_touch_trace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    app_touch_trace_snapshot_t out;
    assert(!app_touch_trace_active() && !app_touch_trace_take(&out));
    app_touch_trace_start(1000000, 7, 91, 990);
    app_touch_trace_start(2000000, 8, 92, 1990);
    if (!strcmp(argv[1], "flow")) {
        app_touch_trace_scan(10, 1000, true, false, true, true, 2);
        app_touch_trace_scan(11, 1015, true, false, true, true, 4);
        app_touch_trace_scan(13, 1045, false, true, false, false, 8);
        app_touch_trace_primary(1001, TOUCH_TRACE_MOVE, true, 3);
        app_touch_trace_primary(1002, TOUCH_TRACE_END, false, 5);
        app_touch_trace_consume(false, 1000, 1016);
        app_touch_trace_consume(true, 1000, 1017);
        app_touch_trace_consume(false, 1050, 1040); /* Preserve a future source as an error. */
        app_touch_trace_displacement(true, true, 1000, 1020);
        app_touch_trace_displacement(false, false, 1000, 1025);
        app_touch_trace_send_begin(true, 1000, 1050500);
        app_touch_trace_send(false);
        app_touch_trace_step(0); app_touch_trace_step(3); app_touch_trace_step(4);
        app_touch_trace_loop(1000, 7, true, true);
        app_touch_trace_loop(1015, 7, true, true);
        app_touch_trace_loop(1030, 7, false, true);
        uint32_t id = app_touch_trace_ble_begin(1051000);
        app_touch_trace_ble_return(id, 0, 1051100);
        app_touch_trace_ble_complete(id + 1, 0, 1052000); /* Unmatched callback cannot complete it. */
        app_touch_trace_ble_complete(id, 0, 1061000);
        app_touch_trace_ble_complete(id, 1, 1062000); /* Duplicate cannot overwrite success. */
        id = app_touch_trace_ble_begin(1071000);
        app_touch_trace_ble_complete(id, 0, 1071100); /* SDK callback before its call returns. */
        app_touch_trace_ble_return(id, 0, 1071200);
        id = app_touch_trace_ble_begin(1081000);
        app_touch_trace_ble_return(id, -1, 1081200);
        id = app_touch_trace_ble_begin(20999999); /* Unfinished at capture boundary. */
        app_touch_trace_ble_complete(id, 0, 21000000);
        assert(app_touch_trace_take(&out));
        assert(out.start_us == 1000000 && out.end_us == 21000000 && out.generation == 7 && out.gesture == 91);
        assert(out.scans == 3 && out.single_pad_scans == 2 && out.invalid_scans == 1 && out.overflow_scans == 1);
        assert(out.missing_scans == 1 && out.contact_queue_drops == 1 && out.queue_peak == 8);
        assert(out.primary[TOUCH_TRACE_MOVE] == 1 && out.primary_drops[TOUCH_TRACE_END] == 1);
        assert(out.motion_enqueues == 2 && out.motion_merged == 1 && out.motion_rejected == 1);
        assert(out.send_calls == 1 && out.send_rejected == 1 && out.steps[0] == 1 && out.steps[3] == 1 && out.steps[4] == 1);
        assert(out.sdk_started == 4 && out.sdk_accepted == 2 && out.sdk_rejected == 1);
        assert(out.complete_ok == 2 && out.complete_failed == 0 && out.early_complete == 1);
        assert(out.outstanding && out.return_unobserved && out.clock_errors == 1);
        assert(out.max_congested_us == 30000 && out.max_congested_at_us == 1030000);
        assert(out.metrics[TOUCH_TRACE_SCAN_INTERVAL].count == 1 && out.metrics[TOUCH_TRACE_SCAN_INTERVAL].max_us == 15000);
        assert(out.metrics[TOUCH_TRACE_PRIMARY_AGE].max_us == 16000);
        assert(out.metrics[TOUCH_TRACE_SEND_AGE].max_us == 50500);
        assert(out.metrics[TOUCH_TRACE_COMPLETE_DURATION].max_us == 10000);
        for (unsigned i = 0; i < TOUCH_TRACE_METRICS; ++i) {
            uint32_t sum = 0;
            for (unsigned b = 0; b < APP_TOUCH_TRACE_BUCKETS; ++b) sum += out.metrics[i].buckets[b];
            assert(sum == out.metrics[i].count);
        }
    } else if (!strcmp(argv[1], "link")) {
        app_touch_trace_loop(1010, 8, false, false);
        assert(app_touch_trace_take(&out) && out.link_changed && out.end_us == 1010000);
    } else if (!strcmp(argv[1], "failure")) {
        uint32_t id = app_touch_trace_ble_begin(1010000);
        app_touch_trace_ble_return(id, 0, 1010100);
        app_touch_trace_ble_complete(id, 5, 1020000);
        app_touch_trace_loop(21000, 7, false, false);
        assert(app_touch_trace_take(&out) && out.complete_failed == 1 && out.last_complete_status == 5 && !out.outstanding);
    } else return 2;
    assert(!app_touch_trace_active() && !app_touch_trace_take(&out));
    app_touch_trace_start(22000000, 9, 99, 22000);
    assert(!app_touch_trace_active());
    printf("PASS observer %s snapshot_bytes=%zu\n", argv[1], sizeof out);
    return 0;
}
