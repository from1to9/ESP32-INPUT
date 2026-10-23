// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_imu_queue.h"

static board_imu_frame_t frames[BOARD_IMU_STREAM_CAPACITY];
static board_imu_batch_t batch;

static board_imu_sample_t sample(uint64_t us)
{
    return (board_imu_sample_t){.x_dps = -0.0f, .y_dps = 10.5f, .z_dps = -20.25f,
        .x_g = 0.125f, .y_g = -0.25f, .z_g = 1.0f, .timestamp_us = us};
}

static void publish(board_imu_queue_t *q, uint64_t us)
{
    const board_imu_sample_t value = sample(us);
    board_imu_queue_publish(q, board_imu_queue_ticket(q), &value);
}

static void drain(board_imu_queue_t *q, board_imu_consumer_t c, size_t cap, uint64_t us)
{
    memset(frames, 0xa5, sizeof(frames));
    assert(board_imu_queue_drain(q, c, frames, cap, us, true, 0, &batch));
    assert(batch.count <= cap);
    if (batch.state != BOARD_IMU_DELIVERY_DATA) {
        assert(batch.count == 0);
        const unsigned char *bytes = (const unsigned char *)frames;
        for (size_t i = 0; i < sizeof(frames); ++i) assert(bytes[i] == 0xa5);
    }
}

static board_imu_consumer_t attach(board_imu_queue_t *q)
{
    board_imu_consumer_t c = 0;
    assert(board_imu_queue_attach(q, &c) && c);
    drain(q, c, 8, q->last_timestamp_us);
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        batch.discontinuities == BOARD_IMU_DISCONTINUITY_ATTACH);
    return c;
}

static void test_fifo_partial_wrap_and_sequence(void)
{
    board_imu_queue_t q = {0};
    publish(&q, 1000); /* Detached publications still have operational identity. */
    const board_imu_consumer_t c = attach(&q);
    for (unsigned i = 1; i <= 5; ++i) publish(&q, 1000 + i * 10000);
    drain(&q, c, 3, 51000);
    assert(batch.state == BOARD_IMU_DELIVERY_DATA && batch.count == 3 && batch.latest_sequence == 6);
    const uint64_t epoch = batch.delivery_epoch;
    for (unsigned i = 0; i < 3; ++i) {
        const board_imu_sample_t expected = sample(11000 + i * 10000);
        assert(frames[i].sequence == i + 2 && frames[i].delivery_epoch == epoch);
        assert(memcmp(&frames[i].sample, &expected, sizeof(expected)) == 0);
    }
    publish(&q, 61000); publish(&q, 71000);
    drain(&q, c, 8, 71000);
    assert(batch.count == 4 && batch.dropped_frames == 0);
    for (unsigned i = 0; i < 4; ++i) assert(frames[i].sequence == i + 5);
    drain(&q, c, 8, 71000);
    assert(batch.state == BOARD_IMU_DELIVERY_EMPTY && !batch.discontinuities);
}

static void test_exactly_once_under_mismatched_rates(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    uint64_t delivered = 0;
    for (uint64_t tick = 1; tick <= 300000; ++tick) {
        if (tick % 2 == 0) publish(&q, tick * 5000);
        if (tick % 3 == 0) {
            drain(&q, c, 8, tick * 5000);
            assert(batch.state == BOARD_IMU_DELIVERY_DATA);
            for (uint32_t i = 0; i < batch.count; ++i) {
                assert(frames[i].sequence == ++delivered);
                assert(frames[i].sample.timestamp_us == delivered * 10000);
                assert(frames[i].delivery_epoch == batch.delivery_epoch);
            }
            assert(!batch.dropped_frames && !batch.discontinuities);
        }
    }
    assert(delivered == 150000 && q.count == 0);
}

static void test_overflow_and_sticky_reasons(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    const uint64_t old_epoch = q.epoch;
    for (unsigned i = 1; i <= 9; ++i) publish(&q, i * 1000);
    assert(!q.count && q.epoch > old_epoch);
    uint64_t fenced = 0;
    assert(board_imu_queue_fence(&q, c, &fenced));
    publish(&q, 10000);
    drain(&q, c, 8, 10000);
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.dropped_frames == 9);
    assert(batch.discontinuities == (BOARD_IMU_DISCONTINUITY_OVERFLOW | BOARD_IMU_DISCONTINUITY_FENCE));
    drain(&q, c, 8, 10000);
    assert(batch.count == 1 && frames[0].sequence == 10 && frames[0].delivery_epoch == fenced);
}

static void test_stale_prefix_and_clock_boundaries(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    publish(&q, 10000); publish(&q, 60000);
    drain(&q, c, 1, 60000); /* Exactly 50 ms remains deliverable. */
    assert(batch.count == 1 && frames[0].sequence == 1);
    drain(&q, c, 8, 110001);
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        batch.discontinuities == BOARD_IMU_DISCONTINUITY_STALE && batch.dropped_frames == 1);
    publish(&q, 120000); publish(&q, 160000);
    drain(&q, c, 8, 170001); /* Fresh latest cannot hide stale prefix. */
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.dropped_frames == 3);
    publish(&q, 180000);
    drain(&q, c, 8, 179999);
    assert(batch.discontinuities == BOARD_IMU_DISCONTINUITY_CLOCK && batch.dropped_frames == 4);
    publish(&q, 180000); /* Duplicate accepted timestamp: explicit order loss. */
    drain(&q, c, 8, 180000);
    assert(batch.discontinuities == BOARD_IMU_DISCONTINUITY_ORDER && batch.dropped_frames == 5);
}

static void test_fences_and_attach_cannot_relabel_inflight(void)
{
    board_imu_queue_t q = {0};
    board_imu_sample_t value = sample(10000);
    const uint64_t before_attach = board_imu_queue_ticket(&q);
    const board_imu_consumer_t c = attach(&q);
    board_imu_queue_publish(&q, before_attach, &value);
    drain(&q, c, 8, 10000);
    assert(batch.discontinuities == BOARD_IMU_DISCONTINUITY_ATTACH && batch.dropped_frames == 1);
    publish(&q, 20000);
    const uint64_t ticket = board_imu_queue_ticket(&q);
    uint64_t new_epoch;
    assert(board_imu_queue_fence(&q, c, &new_epoch));
    drain(&q, c, 8, 20000); /* Acknowledge before the old acquisition completes. */
    value = sample(30000); board_imu_queue_publish(&q, ticket, &value);
    drain(&q, c, 8, 30000);
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.dropped_frames == 3 &&
        batch.delivery_epoch == new_epoch && batch.latest_sequence == 3);
    const uint64_t detached_ticket = board_imu_queue_ticket(&q);
    assert(board_imu_queue_detach(&q, c));
    const board_imu_consumer_t replacement = attach(&q);
    assert(replacement != c);
    value = sample(40000); board_imu_queue_publish(&q, detached_ticket, &value);
    drain(&q, replacement, 8, 40000);
    assert(!batch.count && batch.dropped_frames == 4);
    publish(&q, 50000); drain(&q, replacement, 8, 50000);
    assert(batch.count == 1 && frames[0].sequence == 5 && frames[0].delivery_epoch > new_epoch);
}

static void test_invalid_ownership_and_unavailable(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    board_imu_consumer_t extra = 99;
    assert(!board_imu_queue_attach(&q, &extra) && extra == 99);
    memset(&batch, 0x7a, sizeof(batch));
    const board_imu_batch_t saved = batch;
    assert(!board_imu_queue_drain(&q, 0, frames, 8, 0, true, 0, &batch));
    assert(!board_imu_queue_drain(&q, c + 1, frames, 8, 0, true, 0, &batch));
    assert(!board_imu_queue_drain(&q, c, frames, 0, 0, true, 0, &batch));
    assert(!board_imu_queue_drain(&q, c, frames, 9, 0, true, 0, &batch));
    assert(!board_imu_queue_drain(&q, c, NULL, 8, 0, true, 0, &batch));
    assert(memcmp(&batch, &saved, sizeof(batch)) == 0);
    uint64_t epoch = 123;
    assert(!board_imu_queue_fence(&q, c + 1, &epoch) && epoch == 123);
    assert(!board_imu_queue_detach(&q, c + 1));
    publish(&q, 1000);
    assert(board_imu_queue_drain(&q, c, frames, 8, 1000, false, -17, &batch));
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.source_error == -17 &&
        batch.discontinuities == BOARD_IMU_DISCONTINUITY_SOURCE && batch.dropped_frames == 1);
    assert(board_imu_queue_drain(&q, c, frames, 8, 1000, false, -17, &batch));
    assert(batch.state == BOARD_IMU_DELIVERY_UNAVAILABLE && batch.source_error == -17 && !batch.count);
    assert(board_imu_queue_detach(&q, c));
    assert(!board_imu_queue_drain(&q, c, frames, 8, 1000, true, 0, &batch));
}

static void test_inflight_loss_cause_survives_acknowledgement(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    publish(&q, 1000);
    const uint64_t ticket = board_imu_queue_ticket(&q);
    assert(board_imu_queue_drain(&q, c, frames, 8, 51001, false, -17, &batch));
    const uint32_t causes = BOARD_IMU_DISCONTINUITY_STALE | BOARD_IMU_DISCONTINUITY_SOURCE;
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.discontinuities == causes);
    const board_imu_sample_t next = sample(51002);
    board_imu_queue_publish(&q, ticket, &next);
    drain(&q, c, 8, 51002);
    assert(batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && batch.discontinuities == causes &&
        !(batch.discontinuities & BOARD_IMU_DISCONTINUITY_FENCE) && batch.dropped_frames == 2);
    publish(&q, 52000); drain(&q, c, 8, 52000);
    assert(batch.count == 1 && frames[0].sequence == 3);
}

static void test_detach_counts_canceled_inflight_publication(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    const uint64_t ticket = board_imu_queue_ticket(&q);
    assert(board_imu_queue_detach(&q, c));
    const board_imu_sample_t canceled = sample(1000);
    board_imu_queue_publish(&q, ticket, &canceled);
    publish(&q, 2000); /* Fully unsubscribed acquisition is not delivery loss. */
    const board_imu_consumer_t next = attach(&q);
    assert(batch.dropped_frames == 1 && batch.latest_sequence == 2);
    publish(&q, 3000); drain(&q, next, 8, 3000);
    assert(batch.count == 1 && frames[0].sequence == 3 && batch.dropped_frames == 1);
}

static void test_identifier_and_diagnostic_saturation(void)
{
    board_imu_queue_t q = {0};
    const board_imu_consumer_t c = attach(&q);
    q.dropped_frames = UINT64_MAX;
    publish(&q, 1000);
    uint64_t epoch;
    assert(board_imu_queue_fence(&q, c, &epoch));
    drain(&q, c, 8, 1000);
    assert(batch.dropped_frames == UINT64_MAX && batch.dropped_frames_saturated && !q.failed);
    publish(&q, 2000); drain(&q, c, 8, 2000);
    assert(batch.state == BOARD_IMU_DELIVERY_DATA && batch.count == 1);
    q.latest_sequence = UINT64_MAX - 1;
    publish(&q, 3000); drain(&q, c, 8, 3000);
    assert(frames[0].sequence == UINT64_MAX);
    publish(&q, 4000); drain(&q, c, 8, 4000);
    assert(batch.state == BOARD_IMU_DELIVERY_FAILED && batch.latest_sequence == UINT64_MAX &&
        (batch.discontinuities & BOARD_IMU_DISCONTINUITY_ID_EXHAUSTED));
    drain(&q, c, 8, 5000);
    assert(batch.state == BOARD_IMU_DELIVERY_FAILED); /* No acknowledgement hides failure. */
    epoch = 99;
    assert(!board_imu_queue_fence(&q, c, &epoch) && epoch == 99);
    assert(board_imu_queue_detach(&q, c));
    board_imu_consumer_t next = 99;
    assert(!board_imu_queue_attach(&q, &next) && next == 99);
    q = (board_imu_queue_t){.epoch = UINT64_MAX};
    assert(!board_imu_queue_attach(&q, &next) && next == 99 && q.failed);
    q = (board_imu_queue_t){.owner_counter = UINT64_MAX};
    assert(!board_imu_queue_attach(&q, &next) && q.failed);
    q = (board_imu_queue_t){0};
    const board_imu_consumer_t final = attach(&q);
    q.epoch = UINT64_MAX; epoch = 99;
    assert(!board_imu_queue_fence(&q, final, &epoch) && epoch == 99);
    drain(&q, final, 8, 0);
    assert(batch.state == BOARD_IMU_DELIVERY_FAILED && batch.delivery_epoch == UINT64_MAX);
}

int main(void)
{
    test_fifo_partial_wrap_and_sequence();
    test_exactly_once_under_mismatched_rates();
    test_overflow_and_sticky_reasons();
    test_stale_prefix_and_clock_boundaries();
    test_fences_and_attach_cannot_relabel_inflight();
    test_invalid_ownership_and_unavailable();
    test_inflight_loss_cause_survives_acknowledgement();
    test_detach_counts_canceled_inflight_publication();
    test_identifier_and_diagnostic_saturation();
    printf("IMU queue: FIFO, loss, ownership, in-flight fences and exhaustion passed; sample=%zu frame=%zu batch=%zu queue=%zu bytes\n",
        sizeof(board_imu_sample_t), sizeof(board_imu_frame_t), sizeof(board_imu_batch_t), sizeof(board_imu_queue_t));
    return 0;
}
