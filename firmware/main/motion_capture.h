// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Optional diagnostic observer, owned exclusively by the coordinator. Caller
 * supplies bounded storage. Capturing only copies data; formatting and output
 * happen after finish. Full storage ends observation, never device input.
 * Float values are retained as IEEE-754 bits, including invalid observations.
 * Neither capture completion nor delivery continuity proves physical motion. */
typedef struct {
    uint64_t sequence, epoch, sample_us, observed_us, input_epoch;
    uint32_t axes[6]; /* gx, gy, gz (degrees/s), ax, ay, az (g). */
    uint32_t flags;
    uint32_t discontinuity;
    uint64_t dropped_frames;
    uint32_t source_error, delivery_state;
} motion_capture_frame_t;

typedef enum {
    MOTION_CAPTURE_IDLE,
    MOTION_CAPTURE_RECORDING,
    MOTION_CAPTURE_READY,
} motion_capture_phase_t;

typedef enum {
    MOTION_CAPTURE_STOP_REQUESTED = 1,
    MOTION_CAPTURE_STOP_CAPACITY,
} motion_capture_stop_t;

typedef struct {
    motion_capture_frame_t *frames;
    size_t capacity, count;
    uint64_t session, started_us, finished_us;
    motion_capture_phase_t phase;
    motion_capture_stop_t stop;
} motion_capture_t;

bool motion_capture_init(motion_capture_t *, motion_capture_frame_t *, size_t capacity);
bool motion_capture_start(motion_capture_t *, uint64_t now_us);
bool motion_capture_append(motion_capture_t *, const motion_capture_frame_t *);
void motion_capture_finish(motion_capture_t *, uint64_t now_us);
/* Explicitly releases a completed observation after its output attempt. It
 * never interrupts recording, zeroes counters or reuses a session identity. */
bool motion_capture_release(motion_capture_t *);

/* Stable wire format. Returns false if output would truncate. Header records
 * version, session, count, start/end, reason; rows include an exact index;
 * footer repeats session/count. A reader must reject missing/duplicate rows.
 * Formatting does not mutate capture state. 320 bytes covers every line. */
bool motion_capture_header(const motion_capture_t *, char *, size_t);
bool motion_capture_row(const motion_capture_t *, size_t index, char *, size_t);
bool motion_capture_footer(const motion_capture_t *, char *, size_t);
