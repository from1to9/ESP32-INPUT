// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_io.h"

#define AE_START_TIMEOUT_MS 2000U
#define AE_STOP_TIMEOUT_MS 100U
#define AE_FINALIZE_TIMEOUT_MS 30000U
#define AE_INDICATOR_MAX_AGE_MS 250U
#define AE_PLAYBACK_UNDERRUN_MS 500U
#define AE_CAPTURE_STALL_MS 500U
#define AE_SYNC_INTERVAL_MS 1000U

typedef struct ae_engine ae_engine_t;
typedef enum { AE_OFF, AE_PREPARING, AE_READY_MUTED, AE_RUNNING, AE_STOPPING, AE_FAULT } ae_state_t;
typedef enum { AE_CAPTURE_LIVE, AE_CAPTURE_RECORDING, AE_CAPTURE_RECOGNITION,
    AE_PLAYBACK_STREAM, AE_PLAYBACK_RECORDING } ae_job_kind_t;
typedef enum { AE_JOB_EMPTY, AE_JOB_STARTING, AE_JOB_RUNNING, AE_JOB_STOPPING,
    AE_JOB_FINALIZING, AE_JOB_COMPLETED, AE_JOB_PARTIAL, AE_JOB_ERROR } ae_job_state_t;
typedef enum { AE_DISCONNECT_STOP, AE_DISCONNECT_CONTINUE_LOCAL } ae_disconnect_policy_t;
typedef enum { AE_STOP_REQUESTED, AE_STOP_CANCEL_KEEP, AE_STOP_CANCEL_DISCARD,
    AE_STOP_DURATION, AE_STOP_BYTES, AE_STOP_OWNER_LOST, AE_STOP_CONSENT,
    AE_STOP_PRIVACY, AE_STOP_INDICATOR, AE_STOP_OVERFLOW, AE_STOP_IO,
    AE_STOP_START_TIMEOUT, AE_STOP_UNDERRUN, AE_STOP_TIME, AE_STOP_MAINTENANCE } ae_stop_reason_t;

/* Trusted coordinator admission, not a wire authorization token. Every grant
 * is rechecked through authority below at activation and while running.
 * Starts are immutable; replaying a request ID cannot extend its bounds.
 * The enclosing authenticated command registry owns cross-transport replay
 * history after the engine's bounded current/terminal slots are superseded. */
typedef struct {
    ae_id_t request_id, principal_id, owner_session_id;
    ae_id_t consent_id, background_grant_id, recording_id;
    uint64_t consent_revision, background_grant_revision;
    uint64_t expected_audio_generation, expected_revision;
    ae_job_kind_t kind;
    ae_route_t route;
    ae_route_token_t route_token;
    ae_format_t format;
    uint32_t duration_seconds;
    uint64_t byte_limit;
    uint32_t retention_seconds;
    ae_export_policy_t export_policy;
    ae_storage_t storage;
    ae_disconnect_policy_t disconnect_policy;
    bool activate_microphone;
} ae_start_request_t;

typedef struct {
    bool unlocked;
    bool privacy_latched;
    bool maintenance;
    bool screen_visible;
    bool indicator_visible;
    uint64_t indicator_generation;
    uint64_t indicator_seen_ms; /* Acknowledged readable presentation, not a queued request. */
} ae_environment_t;

typedef enum { AE_AUTH_START, AE_AUTH_RUNNING, AE_AUTH_STOP,
    AE_AUTH_RECORD_READ, AE_AUTH_RECORD_DELETE } ae_authority_operation_t;

typedef struct {
    bool allowed;
    bool owner_session_alive;
    bool background_allowed;
    uint64_t source_valid_until_ms;
    uint64_t background_valid_until_ms;
    ae_result_t reason;
} ae_authority_t;

typedef struct {
    ae_id_t job_id, audio_session_id, recording_id, request_id;
    uint64_t consumer_generation;
    ae_job_kind_t kind;
    ae_job_state_t state;
    ae_stop_reason_t stop_reason;
    ae_result_t error;
    uint64_t first_frame_ms, deadline_ms;
    uint64_t received_bytes, delivered_bytes, sample_frames;
    uint64_t discarded_frames;
    bool first_frame_seen;
    bool device_owned; /* Explicit successful file-only background transfer. */
    bool cleanup_pending;
} ae_job_snapshot_t;

typedef struct {
    ae_state_t state;
    ae_route_t route;
    ae_route_token_t route_token;
    ae_format_t format;
    uint64_t audio_generation, revision;
    bool microphone_enabled, speaker_enabled;
    bool microphone_muted, speaker_muted; /* Immediate logical gates. */
    /* Applied is the last acknowledged physical mute state. Confirmed means
     * that state matches the logical gate with no pending mute/stop request.
     * Unknown startup/failure evidence must never be inferred from !fault. */
    bool microphone_mute_applied, speaker_mute_applied;
    bool microphone_mute_confirmed, speaker_mute_confirmed;
    bool microphone_mute_pending, speaker_mute_pending;
    bool hardware_off_confirmed;
    bool capture_indicator_required; /* Includes an unconfirmed hardware stop. */
    bool capture_gate_open, playback_gate_open;
    bool quarantined;
    uint8_t volume_percent, microphone_gain_db;
    uint64_t overruns, underruns, stale_frames;
    ae_result_t error;
    ae_job_snapshot_t capture[AE_CONSUMERS_MAX];
    ae_job_snapshot_t playback;
} ae_snapshot_t;

typedef struct {
    ae_codec_port_t codec;
    ae_route_port_t route;
    ae_store_port_t store;
    void *policy_context;
    /* Pure bounded policy lookup. No network, UI, codec or filesystem work.
     * For record read/delete, request names the caller and target object.
     * Running authority must verify immutable source/destination/byte/duration/
     * retention/export bounds and current grant revisions, never widen them. */
    ae_result_t (*authority)(void *, ae_authority_operation_t,
        const ae_start_request_t *, uint64_t now_ms, ae_authority_t *);
    ae_result_t (*random_id)(void *, ae_id_t *);
    ae_result_t (*trusted_unix_time)(void *, uint64_t *seconds);
    uint64_t (*monotonic_ms)(void *); /* Rechecked after bounded I/O before publishing samples. */
    /* Thread-safe cancellation latch, checked around every external callback.
     * true closes egress before any subsequently completed read can fan out. */
    bool (*safety_stop_requested)(void *);
    /* Borrowed complete frame. AE_OK consumes it; AE_AGAIN retains it only in
     * the manager's bounded ring. The sink must copy before returning and must
     * enforce generation on any downstream asynchronous operation. */
    ae_result_t (*capture_sink)(void *, ae_job_kind_t, const ae_id_t *job_id, const ae_frame_t *);
    void (*capture_cancel)(void *, ae_job_kind_t, const ae_id_t *job_id, uint64_t consumer_generation);
    /* Enabled only after bounded fan-out/overload validation and a system
     * resource reservation. Independent microphone consumers remain opt-in. */
    bool fanout_enabled;
} ae_dependencies_t;

/* Caller supplies memory aligned to ae_engine_alignment(), ordinarily PSRAM on the device.
 * No internal heap fallback. No I/O occurs until init explicitly proves off.
 * All calls have one serialized owner, including frame submission/snapshots;
 * app_audio supplies the task/cross-thread queue boundary. Init rejects invalid
 * arguments before I/O and leaves the output untouched. Once an engine is
 * returned, retain it even for non-OK codec stop results: its snapshot and
 * recovery lifecycle remain valid. */
size_t ae_engine_bytes(void);
size_t ae_engine_alignment(void);
ae_result_t ae_init(void *memory, size_t bytes, const ae_dependencies_t *, uint64_t now_ms,
    ae_engine_t **engine);
void ae_environment(ae_engine_t *, const ae_environment_t *, uint64_t now_ms);
ae_result_t ae_route_select(ae_engine_t *, ae_route_t, uint64_t generation, uint64_t revision, uint64_t now_ms);
ae_result_t ae_enable(ae_engine_t *, ae_role_t, bool enabled, const ae_format_t *, uint64_t now_ms);
ae_result_t ae_mute(ae_engine_t *, ae_role_t, bool muted, uint64_t now_ms);
ae_result_t ae_volume(ae_engine_t *, uint8_t percent, uint64_t now_ms);
ae_result_t ae_gain(ae_engine_t *, uint8_t db, uint64_t now_ms);
ae_result_t ae_start(ae_engine_t *, const ae_start_request_t *, uint64_t now_ms, ae_job_snapshot_t *);
ae_result_t ae_stop(ae_engine_t *, const ae_id_t *job_id, uint64_t consumer_generation,
    ae_stop_reason_t, uint64_t now_ms);
void ae_safety_stop(ae_engine_t *, ae_stop_reason_t, uint64_t now_ms);
/* Poll performs bounded work; no callback waits beyond the 20 ms port budget.
 * The owner calls it at least every 10 ms while jobs/cleanup are pending. */
ae_result_t ae_poll(ae_engine_t *, uint64_t now_ms);
/* Serialized owner only: available stream ring slots, zero without an active
 * stream. This is storage capacity, not authorization; submission revalidates
 * the frame and current job. No engine mutation may intervene before submit. */
size_t ae_playback_capacity(const ae_engine_t *);
ae_result_t ae_submit_playback(ae_engine_t *, const ae_frame_t *, uint64_t now_ms);
ae_result_t ae_recover(ae_engine_t *, uint64_t now_ms);
void ae_snapshot(const ae_engine_t *, ae_snapshot_t *);

/* Local playback uses the same immutable object/ACL checks without requiring
 * network export permission. These content reads are authenticated downloads
 * and additionally require owner_download; partial objects remain metadata-only.
 * list returns at most AE_LIST_MAX caller-owned objects. */
ae_result_t ae_record_get(ae_engine_t *, const ae_id_t *principal, const ae_id_t *object,
    ae_record_metadata_t *, uint64_t now_ms);
ae_result_t ae_record_list(ae_engine_t *, const ae_id_t *principal, uint32_t cursor,
    ae_record_metadata_t *items, size_t capacity, size_t *count, uint32_t *next, uint64_t now_ms);
ae_result_t ae_record_read(ae_engine_t *, const ae_id_t *principal, const ae_id_t *object,
    uint64_t offset, void *output, size_t capacity, size_t *actual, uint64_t now_ms);
ae_result_t ae_record_delete(ae_engine_t *, const ae_id_t *principal, const ae_id_t *object,
    uint64_t revision, bool *access_revoked, bool *cleanup_complete, uint64_t now_ms);
