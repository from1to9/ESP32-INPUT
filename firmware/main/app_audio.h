// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_engine.h"

/* Async worker contract v1. Init allocates fixed PSRAM state and starts one
 * manager worker; unavailable memory/ports fail closed. No caller performs
 * codec/storage I/O. One ordinary command may be pending, with a retained
 * result until acknowledged; safety stop uses a separate atomic latch.
 * AE_OK from submit means copied/admitted only, not physical success. */
typedef enum { APP_AUDIO_CMD_ROUTE, APP_AUDIO_CMD_ENABLE, APP_AUDIO_CMD_MUTE, APP_AUDIO_CMD_VOLUME,
    APP_AUDIO_CMD_GAIN, APP_AUDIO_CMD_START, APP_AUDIO_CMD_STOP, APP_AUDIO_CMD_RECOVER,
    APP_AUDIO_CMD_RECORD_GET, APP_AUDIO_CMD_RECORD_LIST, APP_AUDIO_CMD_RECORD_READ,
    APP_AUDIO_CMD_RECORD_DELETE, APP_AUDIO_CMD_STORE_PROVISION } app_audio_command_kind_t;

typedef struct {
    uint64_t request_token;
    app_audio_command_kind_t kind;
    uint64_t expected_generation, expected_revision;
    ae_route_t route;
    ae_role_t role;
    ae_format_t format;
    bool value;
    uint8_t level;
    ae_start_request_t start;
    ae_id_t job_id;
    uint64_t consumer_generation;
    ae_stop_reason_t stop_reason;
    ae_id_t principal_id, object_id;
    uint64_t object_revision, offset, quota_bytes;
    uint32_t cursor;
    uint16_t capacity, object_limit; /* Read <= 8192 bytes; list <= AE_LIST_MAX. */
} app_audio_command_t;

typedef struct {
    uint64_t request_token;
    ae_result_t result;
    ae_job_snapshot_t job;
    ae_record_metadata_t metadata;
    uint32_t next_cursor;
    size_t bytes, count;
    bool access_revoked, cleanup_complete;
} app_audio_result_t;

ae_result_t app_audio_init(const ae_dependencies_t *);
ae_result_t app_audio_submit(const app_audio_command_t *);
bool app_audio_take_result(app_audio_result_t *);
/* Payload remains in PSRAM until the next accepted command. Exact token is
 * required; a replaced payload returns AE_STALE, never another caller's data.
 * Copy before submitting the next command; no borrowed pointers escape. */
ae_result_t app_audio_copy_record_data(uint64_t request_token, size_t offset, void *, size_t bytes);
ae_result_t app_audio_copy_record_list(uint64_t request_token, ae_record_metadata_t *, size_t capacity);
void app_audio_set_environment(const ae_environment_t *);
void app_audio_request_safety_stop(ae_stop_reason_t);
/* Copy-only callback entry for decoded BT/UAC/network PCM. Exact generation,
 * format/sequence and capacity are validated before it can reach the codec. */
ae_result_t app_audio_submit_playback(const ae_frame_t *);
/* Copied network ingress: AE_AGAIN means full with no sequence advancement.
 * The sender may retry the exact frame; it does not fault independent audio. */
ae_result_t app_audio_try_submit_playback(const ae_frame_t *);
/* Ports with route.bind_playback must stamp the manager-issued nonzero
 * consumer_generation at production. The ingress preserves and checks it;
 * old callbacks are never retargeted. Legacy ports without this callback may
 * supply zero, which is bound under the ingress lock to the current job.
 * Exact audio generation, actual link token, format and sequence always apply.
 * No job/pre-start/closed ingress rejects without buffering. */
ae_result_t app_audio_submit_transport_playback(ae_route_t, const ae_frame_t *);
/* Cached copied-frame credit for the sole product-worker producer. Zero for
 * closed/stale bindings; the producer must not pop a transport frame then.
 * Credit is advisory: push still validates generation/consumer/link identity. */
size_t app_audio_transport_playback_capacity(ae_route_t, uint64_t audio_generation);
bool app_audio_snapshot(ae_snapshot_t *);
/* Explicit shutdown: close gates immediately, then wait for confirmed hardware
 * stop/recording cleanup. AE_AGAIN never frees possibly owned driver buffers. */
ae_result_t app_audio_shutdown(void);
