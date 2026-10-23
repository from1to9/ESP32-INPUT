// SPDX-License-Identifier: Apache-2.0
#include "motion_capture.h"

#include <inttypes.h>
#include <stdio.h>

bool motion_capture_init(motion_capture_t *m, motion_capture_frame_t *frames, size_t capacity)
{
    if (!m || !frames || !capacity) return false;
    *m = (motion_capture_t){.frames = frames, .capacity = capacity};
    return true;
}

bool motion_capture_start(motion_capture_t *m, uint64_t now_us)
{
    if (!m || !m->frames || !m->capacity || m->phase != MOTION_CAPTURE_IDLE ||
        m->session == UINT64_MAX) return false;
    ++m->session;
    m->count = 0;
    m->started_us = now_us;
    m->finished_us = 0;
    m->stop = 0;
    m->phase = MOTION_CAPTURE_RECORDING;
    return true;
}

bool motion_capture_append(motion_capture_t *m, const motion_capture_frame_t *frame)
{
    if (!m || !frame || m->phase != MOTION_CAPTURE_RECORDING || m->count >= m->capacity)
        return false;
    m->frames[m->count++] = *frame;
    if (m->count == m->capacity) {
        m->phase = MOTION_CAPTURE_READY;
        m->stop = MOTION_CAPTURE_STOP_CAPACITY;
        m->finished_us = frame->observed_us;
    }
    return true;
}

void motion_capture_finish(motion_capture_t *m, uint64_t now_us)
{
    if (!m || m->phase != MOTION_CAPTURE_RECORDING) return;
    m->phase = MOTION_CAPTURE_READY;
    m->stop = MOTION_CAPTURE_STOP_REQUESTED;
    m->finished_us = now_us;
}

bool motion_capture_release(motion_capture_t *m)
{
    if (!m || m->phase != MOTION_CAPTURE_READY) return false;
    m->phase = MOTION_CAPTURE_IDLE;
    return true;
}

static bool fits(int count, char *output, size_t capacity)
{
    if (count < 0 || (size_t)count >= capacity) {
        if (capacity) output[0] = '\0';
        return false;
    }
    return true;
}

bool motion_capture_header(const motion_capture_t *m, char *output, size_t capacity)
{
    if (!m || !output || !capacity || m->phase != MOTION_CAPTURE_READY) return false;
    return fits(snprintf(output, capacity,
        "IMUCAP BEGIN 1 %" PRIu64 " %zu %" PRIu64 " %" PRIu64 " %u",
        m->session, m->count, m->started_us, m->finished_us, (unsigned)m->stop), output, capacity);
}

bool motion_capture_row(const motion_capture_t *m, size_t index, char *output, size_t capacity)
{
    if (!m || !output || !capacity || m->phase != MOTION_CAPTURE_READY || index >= m->count)
        return false;
    const motion_capture_frame_t *f = &m->frames[index];
    return fits(snprintf(output, capacity,
        "IMUCAP ROW %" PRIu64 " %zu %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
        " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
        " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %" PRIu64 " %08" PRIx32 " %08" PRIx32,
        m->session, index, f->sequence, f->epoch, f->sample_us, f->observed_us, f->input_epoch,
        f->axes[0], f->axes[1], f->axes[2], f->axes[3], f->axes[4], f->axes[5],
        f->flags, f->discontinuity, f->dropped_frames, f->source_error, f->delivery_state), output, capacity);
}

bool motion_capture_footer(const motion_capture_t *m, char *output, size_t capacity)
{
    if (!m || !output || !capacity || m->phase != MOTION_CAPTURE_READY) return false;
    return fits(snprintf(output, capacity, "IMUCAP END %" PRIu64 " %zu",
        m->session, m->count), output, capacity);
}
