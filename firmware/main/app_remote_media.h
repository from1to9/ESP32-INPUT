// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_audio.h"
#include "control_registry.h"
#include "control_grants.h"
#include "spoken_letters.h"

#define APP_REMOTE_MEDIA_JOBS 8U
#define APP_REMOTE_MEDIA_READ_BYTES 2048U

typedef enum {
    APP_REMOTE_MEDIA_START, APP_REMOTE_MEDIA_STOP_JOB, APP_REMOTE_MEDIA_ROUTE, APP_REMOTE_MEDIA_KIND,
    APP_REMOTE_MEDIA_ENABLE, APP_REMOTE_MEDIA_MUTE, APP_REMOTE_MEDIA_LINK,
    APP_REMOTE_MEDIA_RECORD_GET, APP_REMOTE_MEDIA_RECORD_LIST,
    APP_REMOTE_MEDIA_RECORD_READ, APP_REMOTE_MEDIA_RECORD_DELETE,
    APP_REMOTE_MEDIA_SPEECH_CONFIRM, APP_REMOTE_MEDIA_SPEECH_REJECT
} app_remote_media_action_t;

/* Immutable decoded coordinator request, copied before submit returns. The
 * identity is the authenticated protocol identity, never a local principal.
 * For START, start contains independently generated request_id and exact
 * current route token/format, caller IDs and reviewed grant revisions. The
 * runtime must preserve it verbatim through its common app_audio owner.
 * Other actions use object/job identities and their exact consumer/revision.
 * Session IDs and opaque object IDs never confer permission by themselves. */
typedef struct app_remote_media_request {
    uint64_t token, command_key, deadline_ms;
    cp_operation_id_t operation;
    cp_identity_t identity;
    uint64_t management_generation, audio_generation, resource_revision;
    uint64_t input_epoch, ui_revision, transport_generation, lease_expires_ms;
    uint8_t lease_id[CP_ID_BYTES];
    char target_id[CP_TARGET_CAPACITY]; /* Candidate confirm only: shared input guards. */
    app_remote_media_action_t action;
    ae_start_request_t start;
    ae_route_t route;
    ae_role_t role;
    ae_format_t format;
    bool value;
    char peer_id[CP_TARGET_CAPACITY];
    ae_id_t object_id, job_id;
    uint64_t consumer_generation, object_revision, offset;
    uint64_t candidate_token, candidate_revision;
    ae_stop_reason_t stop_reason;
    uint32_t cursor;
    uint16_t capacity;
} app_remote_media_request_t;

typedef struct {
    cp_device_state_t device;
    bool audio_known, speech_known, speech_context;
    bool privacy_latched, maintenance;
    ae_snapshot_t audio;
    sl_view_t speech;
    ae_codec_capabilities_t codec;
    ae_store_capabilities_t store;
    ae_route_capabilities_t routes[AE_ROUTE_COUNT];
} app_remote_media_state_t;

typedef struct {
    void *user;
    /* Cached coordinator state only; no network, codec, storage or LVGL I/O. */
    bool (*observe)(void *, app_remote_media_state_t *);
    bool (*available)(void *, cp_operation_id_t, char reason[CP_REASON_CAPACITY]);
    cp_random_fn random;
    bool (*session_alive)(void *, const cp_identity_t *);
    bool (*grant)(void *, const uint8_t id[CP_ID_BYTES], cp_capture_grant_t *);
    cp_error_t (*capture_authority)(void *, const cp_capture_authority_request_t *, cp_capture_authority_t *);
    /* AE_OK is copied command admission only. The runtime remains the sole
     * app_audio result consumer. Poll must match this remote token and owner,
     * and must not consume a local UI command's result. Copy returned query
     * payload before release_result; unknown/pending is never success.
     * Mute/disable/stop completes only after the relevant physical role ACK.
     * The adapter follows long-running jobs via observe after start admission. */
    ae_result_t (*submit)(void *, const app_remote_media_request_t *);
    bool (*poll)(void *, uint64_t token, app_audio_result_t *);
    void (*release_result)(void *, uint64_t token);
    ae_result_t (*copy_records)(void *, uint64_t token, ae_record_metadata_t *, size_t capacity);
    ae_result_t (*copy_record_data)(void *, uint64_t token, size_t offset, void *, size_t bytes);
    /* Revoke only this immutable request's grant/job, never global audio.
     * Recheck source/background grants and session liveness inside runtime
     * authority at activation and every frame. Continue-local is recording
     * only, with a distinct live background delegation. For record reads and
     * deletes the runtime must retain this identity/scopes/object while the
     * async operation is pending, then revalidate before exposing payload. */
    void (*cancel)(void *, uint64_t token, const cp_identity_t *, ae_stop_reason_t);
    bool (*complete)(void *, const cp_result_t *);
    /* Read-only owner-scoped recording metadata cache. The parent may populate
     * it through its bounded async query worker; absent evidence returns BUSY
     * or UNAVAILABLE. Never return another principal's file metadata. */
    cp_read_fn read_recordings;
    /* Confirmation alone needs the existing coordinator input lease/UI guard
     * and preview append path. Neither recognition nor rejection sends HID. */
    bool (*candidate_context)(void *, const cp_command_t *);

} app_remote_media_port_t;

typedef struct app_remote_media app_remote_media_t;
size_t app_remote_media_bytes(void);
/* Caller storage must satisfy both this alignment and the byte requirement. */
size_t app_remote_media_alignment(void);
cp_error_t app_remote_media_init(void *memory, size_t bytes,
    const app_remote_media_port_t *, app_remote_media_t **);
/* Static operation ownership is independent of current hardware readiness. */
bool app_remote_media_owns(cp_operation_id_t);
bool app_remote_media_available(app_remote_media_t *, cp_operation_id_t, char reason[CP_REASON_CAPACITY]);
cp_error_t app_remote_media_execute(app_remote_media_t *, const cp_command_t *, uint64_t now_ms, cp_result_t *);
void app_remote_media_service(app_remote_media_t *, uint64_t now_ms);
void app_remote_media_revoke(app_remote_media_t *, cp_safety_reason_t, const cp_identity_t *, uint64_t now_ms);
/* Coordinator-owned redacted JSON cache builder. Network tasks must use the
 * parent's protected copied cache. Content download requires the separate
 * authenticated binary/Range HTTP transport; this JSON API never relabels
 * encoded samples or an accepted command as a completed download. */
cp_error_t app_remote_media_read(app_remote_media_t *, const cp_identity_t *, const char *resource,
    char *json, size_t capacity, size_t *length);
