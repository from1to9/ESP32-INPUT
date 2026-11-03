// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_engine.h"
#include "audio_media.h"

typedef struct {
    ae_start_request_t request;
    ae_job_snapshot_t view;
    ae_ring_t ring;
    uint64_t startup_deadline_ms, finalize_deadline_ms;
    uint64_t original_consumer_generation;
    uint64_t last_data_ms, next_sync_ms;
    uint64_t playback_offset;
    uint8_t stage;
    bool reserved, reservation_pending, discard, finish_requested, route_bound, route_binding_started;
    ae_record_metadata_t metadata;
} ae_job_t;

struct ae_engine {
    ae_dependencies_t dependencies;
    ae_environment_t environment;
    ae_snapshot_t view;
    ae_codec_capabilities_t codec_capabilities;
    ae_store_capabilities_t store_capabilities;
    ae_job_t capture[AE_CONSUMERS_MAX];
    ae_job_t playback;
    ae_frame_t capture_storage[AE_CONSUMERS_MAX][AE_CAPTURE_RING_FRAMES];
    ae_frame_t playback_storage[AE_RING_FRAMES];
    ae_frame_t work_frame, playback_io_frame;
    uint8_t wav_header[AE_WAV_HEADER_BYTES];
    uint64_t last_ms, next_consumer_generation;
    uint64_t capture_sequence, capture_sample_offset;
    uint64_t stop_generation, stop_started_ms;
    ae_route_token_t stop_route_token;
    uint64_t last_unix_seconds;
    uint64_t last_capture_ms, closing_started_ms[2];
    size_t capture_fill;
    uint8_t prepare_stage, source_roles, active_roles, io_pending, closing_roles, control_pending, control_value;
    uint8_t mute_known_roles, unmute_pending, unmute_cancelled;
    bool playback_turn;
    bool codec_prepared, codec_running;
    bool source_prepare_started; /* Includes pending I/O; AE_BUSY explicitly acquired nothing. */
    bool codec_stop_pending, route_stop_pending;
    bool microphone_may_be_active;
    bool clock_fault;
    bool initialized;
};

bool ae_job_active(const ae_job_t *);
bool ae_cancel_latched(const ae_engine_t *);
bool ae_clock(ae_engine_t *, uint64_t now_ms);
bool ae_privacy_ready(const ae_engine_t *, uint64_t now_ms);
ae_result_t ae_authorize(ae_engine_t *, ae_job_t *, ae_authority_operation_t, uint64_t now_ms);
void ae_end_job(ae_engine_t *, ae_job_t *, ae_stop_reason_t, ae_result_t, uint64_t now_ms);
void ae_request_hardware_stop(ae_engine_t *, uint64_t now_ms);
bool ae_jobs_active(const ae_engine_t *);
ae_result_t ae_record_admit(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_record_reserve(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_record_write(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_record_finalize(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_record_reconcile(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_record_source(ae_engine_t *, ae_job_t *, uint64_t now_ms);
ae_result_t ae_store_time(ae_engine_t *, uint64_t *unix_seconds);
void ae_refresh_view(ae_engine_t *);
