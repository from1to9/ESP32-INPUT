// SPDX-License-Identifier: Apache-2.0
#include "../../firmware/main/board_imu_queue.h"

static board_imu_queue_t capture_queue;
static bool capture_attach_failure;

esp_err_t board_imu_stream_attach(board_imu_consumer_t *consumer)
{
    return !capture_attach_failure && board_imu_queue_attach(&capture_queue, consumer) ? ESP_OK : ESP_FAIL;
}
esp_err_t board_imu_stream_fence(board_imu_consumer_t consumer, uint64_t *epoch)
{
    return board_imu_queue_fence(&capture_queue, consumer, epoch) ? ESP_OK : ESP_FAIL;
}
esp_err_t board_imu_stream_drain(board_imu_consumer_t consumer, board_imu_frame_t *frames,
                               size_t capacity, board_imu_batch_t *batch)
{
    return board_imu_queue_drain(&capture_queue, consumer, frames, capacity,
        (uint64_t)esp_timer_get_time(), test_capabilities.imu, test_imu_result, batch) ? ESP_OK : ESP_FAIL;
}
esp_err_t board_imu_stream_detach(board_imu_consumer_t consumer)
{
    return board_imu_queue_detach(&capture_queue, consumer) ? ESP_OK : ESP_FAIL;
}

static void capture_setup(void)
{
    setup(INPUT_MOUSE);
    capture_queue = (board_imu_queue_t){0};
    capture_attach_failure = false;
    s_capture = (motion_capture_t){0};
    s_capture_consumer = 0;
    s_capture_initialized = s_capture_previous_requested = false;
    s_capture_dump_index = s_capture_dump_stage = 0;
    test_capabilities.imu = true;
    test_imu_result = ESP_OK;
    s_engine.state.gyro_enabled = false;
    service_motion_capture();
    s_engine.state.gyro_enabled = true;
    service_motion_capture();
    assert(s_capture.phase == MOTION_CAPTURE_RECORDING && s_capture.count == 1);
    assert(s_capture.frames[0].discontinuity == BOARD_IMU_DISCONTINUITY_FENCE);
}

static void capture_publish(void)
{
    s_now_ms += 10;
    board_imu_sample_t sample = {.timestamp_us = (uint64_t)esp_timer_get_time(), .z_g = 1,
                                .x_dps = 0.25f, .y_g = -0.1f};
    board_imu_queue_publish(&capture_queue, board_imu_queue_ticket(&capture_queue), &sample);
    service_motion_capture();
}

static void raw_capture_observes_without_mutating_production_input(void)
{
    capture_setup();
    const hid_engine_t before_engine = s_engine;
    const interaction_t before_model = s_interaction;
    const size_t before_reports = trace_count;
    const uint64_t before_latest = s_last_imu_us;
    capture_publish();
    capture_publish();
    assert(s_capture.count == 3 && (s_capture.frames[1].flags & 1));
    assert(s_capture.frames[2].sequence == s_capture.frames[1].sequence + 1);
    assert(s_capture.frames[2].sample_us > s_capture.frames[1].sample_us);
    assert(memcmp(&before_engine, &s_engine, sizeof s_engine) == 0);
    assert(memcmp(&before_model, &s_interaction, sizeof s_interaction) == 0);
    assert(trace_count == before_reports && s_last_imu_us == before_latest);
}

static void raw_capture_full_and_dump_pause_do_not_stop_motion(void)
{
    capture_setup();
    s_capture.capacity = 2; /* Exercise the same production full-state path quickly. */
    capture_publish();
    assert(s_capture.phase == MOTION_CAPTURE_READY && s_capture.stop == MOTION_CAPTURE_STOP_CAPACITY);
    assert(s_engine.state.gyro_enabled && s_capture_dump_stage == 0);
    const motion_capture_frame_t saved = s_capture.frames[1];
    for (unsigned i = 0; i < 20; ++i) capture_publish();
    assert(s_capture.count == 2 && memcmp(&saved, &s_capture.frames[1], sizeof saved) == 0);
    s_engine.state.gyro_enabled = false;
    service_motion_capture();
    assert(s_capture_dump_stage == 1);
    s_engine.state.gyro_enabled = true;
    service_motion_capture();
    assert(s_capture.session == 1 && s_capture_dump_stage == 1);
    s_engine.state.gyro_enabled = false;
    for (unsigned i = 0; i < 4; ++i) service_motion_capture();
    assert(s_capture.phase == MOTION_CAPTURE_IDLE);
    s_engine.state.gyro_enabled = true;
    service_motion_capture();
    assert(s_capture.session == 2 && s_capture.phase == MOTION_CAPTURE_RECORDING);
}

static void raw_capture_delivery_failure_is_not_a_hardware_or_input_failure(void)
{
    capture_setup();
    const hid_engine_t engine = s_engine;
    const interaction_t model = s_interaction;
    board_imu_queue_break(&capture_queue, BOARD_IMU_DISCONTINUITY_SOURCE);
    test_imu_result = ESP_ERR_TIMEOUT;
    service_motion_capture();
    assert(s_capture.count == 2 && !(s_capture.frames[1].flags & 1));
    assert(s_capture.frames[1].source_error == (uint32_t)ESP_ERR_TIMEOUT);
    assert(memcmp(&engine, &s_engine, sizeof engine) == 0);
    assert(memcmp(&model, &s_interaction, sizeof model) == 0 && test_capabilities.imu);
    capture_queue.failed = true;
    service_motion_capture();
    assert(memcmp(&engine, &s_engine, sizeof engine) == 0 && test_capabilities.imu);
}
