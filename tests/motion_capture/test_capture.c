// SPDX-License-Identifier: Apache-2.0
#include "motion_capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    motion_capture_t capture;
    struct {
        motion_capture_frame_t before;
        motion_capture_frame_t rows[2];
        motion_capture_frame_t after;
    } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    motion_capture_frame_t sentinel = guarded.before;
    char line[320];
    assert(!motion_capture_init(NULL, guarded.rows, 2));
    assert(!motion_capture_init(&capture, NULL, 2));
    assert(!motion_capture_init(&capture, guarded.rows, 0));
    assert(motion_capture_init(&capture, guarded.rows, 2));
    assert(!motion_capture_release(&capture));
    assert(!motion_capture_header(&capture, line, sizeof line));
    assert(motion_capture_start(&capture, 100));
    assert(!motion_capture_start(&capture, 200));
    assert(!motion_capture_release(&capture));
    motion_capture_frame_t row = {.sequence = 1, .epoch = 2, .sample_us = 120,
        .observed_us = 130, .input_epoch = 5, .axes = {0x3f800000, 0xff800000, 0x7fc00000},
        .flags = 1, .discontinuity = 0};
    assert(motion_capture_append(&capture, &row));
    /* Preserve invalid raw bits; observer must never normalize sensor evidence. */
    assert(memcmp(&guarded.rows[0], &row, sizeof row) == 0);
    assert(!motion_capture_row(&capture, 0, line, sizeof line));
    row.sequence = 2;
    row.observed_us = 150;
    assert(motion_capture_append(&capture, &row));
    assert(capture.phase == MOTION_CAPTURE_READY && capture.count == 2);
    assert(capture.stop == MOTION_CAPTURE_STOP_CAPACITY && capture.finished_us == 150);
    assert(!motion_capture_append(&capture, &row));
    motion_capture_finish(&capture, 999);
    assert(capture.finished_us == 150);
    assert(memcmp(&guarded.before, &sentinel, sizeof sentinel) == 0);
    assert(memcmp(&guarded.after, &sentinel, sizeof sentinel) == 0);
    assert(motion_capture_header(&capture, line, sizeof line));
    assert(strcmp(line, "IMUCAP BEGIN 1 1 2 100 150 2") == 0);
    puts(line);
    assert(motion_capture_row(&capture, 0, line, sizeof line));
    puts(line);
    assert(motion_capture_row(&capture, 1, line, sizeof line));
    puts(line);
    assert(!motion_capture_row(&capture, 2, line, sizeof line));
    assert(motion_capture_footer(&capture, line, sizeof line));
    puts(line);
    assert(motion_capture_release(&capture));
    assert(motion_capture_start(&capture, 1000) && capture.session == 2 && capture.count == 0);
    motion_capture_finish(&capture, 1001);
    assert(capture.stop == MOTION_CAPTURE_STOP_REQUESTED);
    assert(motion_capture_header(&capture, line, sizeof line));
    assert(strcmp(line, "IMUCAP BEGIN 1 2 0 1000 1001 1") == 0);
    /* Maximum integer widths fit the wire contract; undersized buffers fail
     * explicitly instead of returning a plausible truncated record. */
    capture.count = 1;
    capture.session = UINT64_MAX;
    memset(&guarded.rows[0], 0xff, sizeof guarded.rows[0]);
    assert(motion_capture_row(&capture, 0, line, sizeof line));
    assert(!motion_capture_row(&capture, 0, line, 8) && line[0] == '\0');
    assert(motion_capture_release(&capture));
    assert(!motion_capture_start(&capture, 2000));
    puts("Motion capture ownership, bounds, raw-bit preservation and wire tests passed.");
    return 0;
}
