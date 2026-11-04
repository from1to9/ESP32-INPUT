// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_audio.h"
#include "app_spoken_letters.h"

/* Product composition above the codec/transport manager. One worker owns
 * transaction sequencing and results; caller-facing methods copy only. Local
 * physical admission is performed by the coordinator before calling submit.
 * Remote admission must provide a current authenticated, scoped grant through
 * the same immutable request boundary; it is never inferred from a network. */
typedef enum {
    APP_MEDIA_START, APP_MEDIA_STOP, APP_MEDIA_VOLUME, APP_MEDIA_GAIN,
    APP_MEDIA_RECOVER, APP_MEDIA_ROUTE, APP_MEDIA_QUERY_GET, APP_MEDIA_QUERY_LIST,
    APP_MEDIA_QUERY_READ, APP_MEDIA_QUERY_DELETE, APP_MEDIA_PREPARE_STORE,
    APP_MEDIA_ENABLE_ROLE, APP_MEDIA_MUTE_ROLE, APP_MEDIA_STOP_JOB, APP_MEDIA_LINK_CONTROL
} app_media_action_t;
typedef struct {
    uint64_t token, epoch, ui_revision;
    uint64_t expected_audio_generation, expected_audio_revision;
    app_media_action_t action;
    ae_job_kind_t kind;
    ae_route_t route;
    /* Required for a local external-route START. Copy the admitted full
     * transport identity; asynchronous route selection never replaces it. */
    ae_route_token_t expected_route_token;
    ae_format_t format;
    uint32_t duration_seconds;
    uint64_t byte_limit;
    uint32_t retention_seconds;
    uint8_t level;
    ae_export_policy_t export_policy;
    ae_storage_t storage;
    ae_id_t recording_id, job_id;
    uint64_t consumer_generation;
    ae_stop_reason_t stop_reason;
    ae_role_t role;
    bool value;
    uint64_t bluetooth_generation, bluetooth_link_generation, bluetooth_bond_revision;
    uint8_t bluetooth_peer[6];
    uint64_t object_revision, offset, quota_bytes;
    uint32_t cursor;
    uint16_t capacity, object_limit;
} app_media_local_request_t;
typedef struct {
    bool initialized, busy, result_ready;
    uint64_t request_token;
    ae_result_t result;
    app_audio_result_t result_detail;
    bool audio_known, speech_known;
    ae_snapshot_t audio;
    sl_view_t speech;
} app_media_snapshot_t;

/* Init never opens the microphone or prepares recording storage. */
typedef struct {
    ae_store_port_t store;
    ae_result_t (*wall_time)(void *, uint64_t *seconds);
    void *clock_context;
} app_media_services_t;
ae_result_t app_media_runtime_init(const app_media_services_t *);
/* Current UI/input identities gate local speech preview only. Independent
 * local audio may remain active while the user changes input modes. */
void app_media_runtime_environment(const ae_environment_t *, uint64_t epoch,
                                   uint64_t ui_revision, bool speech_context);
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *);
void app_media_runtime_cancel_boot_speaker(uint64_t token);
void app_media_runtime_stop(ae_stop_reason_t);
bool app_media_runtime_pairing_current(uint64_t generation, uint64_t revision);
bool app_media_runtime_snapshot(app_media_snapshot_t *);
/* Copy any returned payload before acknowledging the result. Exact product
 * token prevents a previous caller from reading a replacement query's bytes. */
ae_result_t app_media_runtime_copy_records(uint64_t token, ae_record_metadata_t *, size_t capacity);
ae_result_t app_media_runtime_copy_record_data(uint64_t token, size_t offset, void *, size_t bytes);
bool app_media_runtime_take_result(uint64_t *token, ae_result_t *);
ae_result_t app_media_runtime_confirm(uint64_t token, uint64_t epoch,
    uint64_t ui_revision, char *candidate);
ae_result_t app_media_runtime_reject(uint64_t token, uint64_t epoch, uint64_t ui_revision);
/* Cached route capabilities are independent from HID target selection. */
ae_result_t app_media_runtime_route(ae_route_t, ae_route_capabilities_t *);
