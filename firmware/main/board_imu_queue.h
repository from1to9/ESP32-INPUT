// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include "board_imu_stream.h"

/* Private portable state. Zero initialization is sufficient. The adapter must
 * serialize every call with the same lock; this module does not own a task or
 * synchronization primitive. One producer and one attached consumer only. */
typedef struct {
    board_imu_frame_t frames[BOARD_IMU_STREAM_CAPACITY];
    uint64_t owner_counter, owner, epoch, latest_sequence, last_timestamp_us;
    uint64_t dropped_frames;
    uint32_t head, count, pending, inflight_reasons;
    bool attached, failed, dropped_frames_saturated;
} board_imu_queue_t;

bool board_imu_queue_attach(board_imu_queue_t *queue, board_imu_consumer_t *consumer);
bool board_imu_queue_owns(const board_imu_queue_t *queue, board_imu_consumer_t consumer);
bool board_imu_queue_fence(board_imu_queue_t *queue, board_imu_consumer_t consumer, uint64_t *epoch);
bool board_imu_queue_detach(board_imu_queue_t *queue, board_imu_consumer_t consumer);
/* Begin the sole producer's acquisition, never just peek at publication. This
 * retains invalidation reasons until the in-flight result is published, even
 * if a consumer already acknowledged the corresponding discontinuity. */
uint64_t board_imu_queue_ticket(board_imu_queue_t *queue);
void board_imu_queue_publish(board_imu_queue_t *queue, uint64_t ticket, const board_imu_sample_t *sample);
void board_imu_queue_break(board_imu_queue_t *queue, uint32_t reasons);
/* False means invalid arguments/token; all caller outputs remain untouched.
 * True acknowledges at most one discontinuity or copies <= capacity frames. */
bool board_imu_queue_drain(board_imu_queue_t *queue, board_imu_consumer_t consumer,
    board_imu_frame_t *frames, size_t capacity, uint64_t now_us,
    bool source_available, int32_t source_error, board_imu_batch_t *batch);
