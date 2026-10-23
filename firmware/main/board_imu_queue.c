// SPDX-License-Identifier: Apache-2.0
#include "board_imu_queue.h"

static void count_dropped(board_imu_queue_t *queue, uint64_t count)
{
    if (UINT64_MAX - queue->dropped_frames < count) {
        queue->dropped_frames = UINT64_MAX;
        queue->dropped_frames_saturated = true;
    } else queue->dropped_frames += count;
}

static void clear_frames(board_imu_queue_t *queue)
{
    count_dropped(queue, queue->count);
    queue->head = queue->count = 0;
}

static void fail_ids(board_imu_queue_t *queue)
{
    queue->failed = true;
    queue->pending |= BOARD_IMU_DISCONTINUITY_ID_EXHAUSTED;
    clear_frames(queue);
}

static bool advance_epoch(board_imu_queue_t *queue)
{
    if (queue->epoch == UINT64_MAX) {
        fail_ids(queue);
        return false;
    }
    ++queue->epoch;
    return true;
}

bool board_imu_queue_owns(const board_imu_queue_t *queue, board_imu_consumer_t consumer)
{
    return queue && queue->attached && consumer && queue->owner == consumer;
}

bool board_imu_queue_attach(board_imu_queue_t *queue, board_imu_consumer_t *consumer)
{
    if (!queue || !consumer || queue->attached || queue->failed) return false;
    if (queue->owner_counter == UINT64_MAX) {
        fail_ids(queue);
        return false;
    }
    if (!advance_epoch(queue)) return false;
    clear_frames(queue);
    queue->owner = ++queue->owner_counter;
    queue->attached = true;
    queue->pending = BOARD_IMU_DISCONTINUITY_ATTACH;
    queue->inflight_reasons |= BOARD_IMU_DISCONTINUITY_ATTACH;
    *consumer = queue->owner;
    return true;
}

void board_imu_queue_break(board_imu_queue_t *queue, uint32_t reasons)
{
    if (!queue->attached || queue->failed) return;
    queue->pending |= reasons;
    queue->inflight_reasons |= reasons;
    clear_frames(queue);
    (void)advance_epoch(queue);
}

bool board_imu_queue_fence(board_imu_queue_t *queue, board_imu_consumer_t consumer, uint64_t *epoch)
{
    if (!epoch || !board_imu_queue_owns(queue, consumer) || queue->failed) return false;
    board_imu_queue_break(queue, BOARD_IMU_DISCONTINUITY_FENCE);
    if (queue->failed) return false;
    *epoch = queue->epoch;
    return true;
}

bool board_imu_queue_detach(board_imu_queue_t *queue, board_imu_consumer_t consumer)
{
    if (!board_imu_queue_owns(queue, consumer)) return false;
    clear_frames(queue);
    queue->inflight_reasons |= BOARD_IMU_DISCONTINUITY_FENCE;
    queue->attached = false;
    queue->owner = 0;
    /* The next attach advances epoch before accepting a new acquisition. */
    return true;
}

uint64_t board_imu_queue_ticket(board_imu_queue_t *queue)
{
    queue->inflight_reasons = 0;
    return queue->attached && !queue->failed ? queue->epoch : 0;
}

void board_imu_queue_publish(board_imu_queue_t *queue, uint64_t ticket, const board_imu_sample_t *sample)
{
    if (queue->latest_sequence == UINT64_MAX) {
        fail_ids(queue);
        if (queue->attached) count_dropped(queue, 1);
        return;
    }
    ++queue->latest_sequence;
    const bool ordered = queue->latest_sequence == 1 || sample->timestamp_us > queue->last_timestamp_us;
    if (ordered) queue->last_timestamp_us = sample->timestamp_us;
    if (!queue->attached) {
        if (ticket) count_dropped(queue, 1); /* Acquisition was subscribed before detach. */
        return;
    }
    if (queue->failed) {
        count_dropped(queue, 1);
        return;
    }
    if (!ticket || ticket != queue->epoch) {
        count_dropped(queue, 1);
        queue->pending |= queue->inflight_reasons ? queue->inflight_reasons : BOARD_IMU_DISCONTINUITY_REJECTED;
        return;
    }
    if (!ordered || queue->count == BOARD_IMU_STREAM_CAPACITY) {
        const uint32_t reason = !ordered ? BOARD_IMU_DISCONTINUITY_ORDER : BOARD_IMU_DISCONTINUITY_OVERFLOW;
        count_dropped(queue, 1);
        board_imu_queue_break(queue, reason);
        return;
    }
    const uint32_t tail = (queue->head + queue->count) % BOARD_IMU_STREAM_CAPACITY;
    queue->frames[tail] = (board_imu_frame_t){
        .sample = *sample, .sequence = queue->latest_sequence, .delivery_epoch = queue->epoch,
    };
    ++queue->count;
}

bool board_imu_queue_drain(board_imu_queue_t *queue, board_imu_consumer_t consumer,
    board_imu_frame_t *frames, size_t capacity, uint64_t now_us,
    bool source_available, int32_t source_error, board_imu_batch_t *batch)
{
    if (!frames || !batch || !capacity || capacity > BOARD_IMU_STREAM_CAPACITY ||
            !board_imu_queue_owns(queue, consumer)) return false;
    uint32_t reasons = 0;
    for (uint32_t i = 0; i < queue->count; ++i) {
        const uint64_t time_us = queue->frames[(queue->head + i) % BOARD_IMU_STREAM_CAPACITY].sample.timestamp_us;
        if (now_us < time_us) reasons |= BOARD_IMU_DISCONTINUITY_CLOCK;
        else if (now_us - time_us > BOARD_IMU_STREAM_MAX_AGE_US) reasons |= BOARD_IMU_DISCONTINUITY_STALE;
    }
    if (!source_available && queue->count) reasons |= BOARD_IMU_DISCONTINUITY_SOURCE;
    if (reasons) board_imu_queue_break(queue, reasons);
    *batch = (board_imu_batch_t){
        .state = BOARD_IMU_DELIVERY_EMPTY, .delivery_epoch = queue->epoch,
        .latest_sequence = queue->latest_sequence, .dropped_frames = queue->dropped_frames,
        .dropped_frames_saturated = queue->dropped_frames_saturated, .source_error = source_error,
    };
    if (queue->failed) {
        batch->state = BOARD_IMU_DELIVERY_FAILED;
        batch->discontinuities = queue->pending;
    } else if (queue->pending) {
        batch->state = BOARD_IMU_DELIVERY_DISCONTINUITY;
        batch->discontinuities = queue->pending;
        queue->pending = 0;
    } else if (!source_available) batch->state = BOARD_IMU_DELIVERY_UNAVAILABLE;
    else if (queue->count) {
        batch->state = BOARD_IMU_DELIVERY_DATA;
        batch->count = queue->count < capacity ? queue->count : (uint32_t)capacity;
        for (uint32_t i = 0; i < batch->count; ++i) {
            frames[i] = queue->frames[queue->head];
            queue->head = (queue->head + 1U) % BOARD_IMU_STREAM_CAPACITY;
        }
        queue->count -= batch->count;
    }
    return true;
}
