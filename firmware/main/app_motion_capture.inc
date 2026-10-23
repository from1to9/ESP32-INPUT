// SPDX-License-Identifier: Apache-2.0
/* Diagnostic-only observer. The ordered stream is NOT fed into the existing
 * angular model here. This prerequisite collects evidence for its replacement.
 * Observed context is not acquisition-time permission to emit HID. */
#if defined(CONFIG_APP_IMU_CAPTURE) && CONFIG_APP_IMU_CAPTURE
#include "motion_capture.h"

#define MOTION_CAPTURE_RECORDS 6000U
static EXT_RAM_BSS_ATTR motion_capture_frame_t s_capture_frames[MOTION_CAPTURE_RECORDS];
static EXT_RAM_BSS_ATTR board_imu_frame_t s_capture_batch[BOARD_IMU_STREAM_CAPACITY];
static EXT_RAM_BSS_ATTR char s_capture_line[320];
static motion_capture_t s_capture;
static board_imu_consumer_t s_capture_consumer;
static bool s_capture_initialized, s_capture_previous_requested;
static size_t s_capture_dump_index;
static unsigned s_capture_dump_stage;
static board_imu_delivery_state_t s_capture_last_state;

static uint32_t capture_context_flags(bool frame_present)
{
    bool touch, physical;
    portENTER_CRITICAL(&s_control_lock);
    touch = s_touch_down;
    physical = s_physical_down;
    portEXIT_CRITICAL(&s_control_lock);
    return (frame_present ? 1U : 0U) | (s_engine.state.gyro_enabled ? 1U << 1 : 0U) |
        (s_engine.state.locked ? 1U << 2 : 0U) | (touch ? 1U << 3 : 0U) |
        (physical ? 1U << 4 : 0U) | (s_overlay != APP_OVERLAY_NONE ? 1U << 5 : 0U) |
        (board_haptics_active() ? 1U << 6 : 0U) | (s_interaction.output_ready ? 1U << 7 : 0U) |
        (s_interaction.view.calibrated ? 1U << 8 : 0U) | ((uint32_t)s_engine.state.mode << 9) |
        ((uint32_t)s_engine.state.owner << 11) | (s_engine.state.release_pending ? 1U << 13 : 0U) |
        (s_engine.congested ? 1U << 14 : 0U);
}

static void capture_observation(const board_imu_batch_t *batch, const board_imu_frame_t *frame,
                                uint64_t observed_us)
{
    if (s_capture.phase != MOTION_CAPTURE_RECORDING) return;
    motion_capture_frame_t record = {
        .sequence = frame ? frame->sequence : batch->latest_sequence,
        .epoch = frame ? frame->delivery_epoch : batch->delivery_epoch,
        .sample_us = frame ? frame->sample.timestamp_us : 0,
        .observed_us = observed_us, .input_epoch = s_engine.state.epoch,
        .flags = capture_context_flags(frame != NULL) |
            (batch->dropped_frames_saturated ? 1U << 15 : 0U),
        .discontinuity = batch->discontinuities, .dropped_frames = batch->dropped_frames,
        .source_error = (uint32_t)batch->source_error, .delivery_state = (uint32_t)batch->state,
    };
    if (frame) {
        const float axes[6] = {frame->sample.x_dps, frame->sample.y_dps, frame->sample.z_dps,
                              frame->sample.x_g, frame->sample.y_g, frame->sample.z_g};
        _Static_assert(sizeof axes == sizeof record.axes, "Capture requires 32-bit sensor floats");
        memcpy(record.axes, axes, sizeof axes);
    }
    (void)motion_capture_append(&s_capture, &record);
}

static void capture_dump_step(void)
{
    if (s_capture.phase != MOTION_CAPTURE_READY) return;
    bool formatted = false;
    if (s_capture_dump_stage == 0) {
        char elf_hash[65];
        const esp_app_desc_t *description = esp_app_get_description();
        for (size_t i = 0; i < 32; ++i)
            (void)snprintf(elf_hash + i * 2, sizeof elf_hash - i * 2, "%02x", description->app_elf_sha256[i]);
        (void)snprintf(s_capture_line, sizeof s_capture_line, "IMUCAP IMAGE %s %s %s",
            APP_BUILD_GIT_KNOWN ? APP_BUILD_GIT_COMMIT : "unknown", APP_BUILD_GIT_STATE, elf_hash);
        formatted = true;
        ++s_capture_dump_stage;
    } else if (s_capture_dump_stage == 1) {
        formatted = motion_capture_header(&s_capture, s_capture_line, sizeof s_capture_line);
        ++s_capture_dump_stage;
    } else if (s_capture_dump_index < s_capture.count) {
        formatted = motion_capture_row(&s_capture, s_capture_dump_index++, s_capture_line, sizeof s_capture_line);
    } else {
        formatted = motion_capture_footer(&s_capture, s_capture_line, sizeof s_capture_line);
        (void)motion_capture_release(&s_capture);
    }
    /* At most one bounded line per coordinator loop, only after recording and
     * while Motion is off. CDC may drop data; the host must verify every row
     * and END. A completed output attempt is never delivery acknowledgement. */
    if (formatted) (void)printf("%s\n", s_capture_line);
}

static void service_motion_capture(void)
{
    if (!s_capture_initialized) {
        s_capture_initialized = true;
        (void)motion_capture_init(&s_capture, s_capture_frames, MOTION_CAPTURE_RECORDS);
        const esp_err_t error = board_imu_stream_attach(&s_capture_consumer);
        if (error != ESP_OK) ESP_LOGE(TAG, "Raw IMU capture unavailable: %s", esp_err_to_name(error));
        s_capture_last_state = BOARD_IMU_DELIVERY_EMPTY;
    }
    if (!s_capture_consumer) return;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const bool requested = s_engine.state.mode == INPUT_MOUSE && s_engine.state.gyro_enabled;
    if (requested && !s_capture_previous_requested && s_capture.phase == MOTION_CAPTURE_IDLE) {
        uint64_t epoch;
        const esp_err_t error = board_imu_stream_fence(s_capture_consumer, &epoch);
        if (error == ESP_OK && motion_capture_start(&s_capture, now_us)) {
            s_capture_dump_stage = 0;
            s_capture_dump_index = 0;
        } else ESP_LOGE(TAG, "Raw IMU capture could not start: %s",
                       esp_err_to_name(error == ESP_OK ? ESP_ERR_INVALID_STATE : error));
    }
    s_capture_previous_requested = requested;
    board_imu_batch_t batch = {0};
    const esp_err_t error = board_imu_stream_drain(s_capture_consumer, s_capture_batch,
                                                 BOARD_IMU_STREAM_CAPACITY, &batch);
    if (error != ESP_OK) {
        batch.state = BOARD_IMU_DELIVERY_FAILED;
        batch.source_error = error;
    }
    const uint64_t observed_us = (uint64_t)esp_timer_get_time();
    if (batch.state == BOARD_IMU_DELIVERY_DATA) {
        for (size_t i = 0; i < batch.count && i < BOARD_IMU_STREAM_CAPACITY; ++i)
            capture_observation(&batch, &s_capture_batch[i], observed_us);
    } else if (batch.discontinuities || (batch.state != BOARD_IMU_DELIVERY_EMPTY &&
                                       batch.state != s_capture_last_state)) {
        capture_observation(&batch, NULL, observed_us);
    }
    s_capture_last_state = batch.state;
    /* Preserve the final drain/fault observation before ending the record,
     * including a hardware failure that turned Motion off in process_motion. */
    if (!requested) motion_capture_finish(&s_capture, observed_us);
    if (!requested) capture_dump_step();
}
#else
static void service_motion_capture(void) {}
#endif
