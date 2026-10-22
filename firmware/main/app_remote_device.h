// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "control_registry.h"
#include "device_settings.h"

#define APP_REMOTE_DEVICE_TRANSACTIONS 4U
#define APP_REMOTE_DEVICE_JOBS 8U
#define APP_REMOTE_DEVICE_STAGE_MS 30000U
#define APP_REMOTE_DEVICE_JOB_MS 5000U

typedef enum {
    APP_REMOTE_SETTING_THEME = 1U << 0,
    APP_REMOTE_SETTING_BRIGHTNESS = 1U << 1,
    APP_REMOTE_SETTING_IDLE = 1U << 2,
    APP_REMOTE_SETTING_HAPTICS = 1U << 3,
    APP_REMOTE_SETTING_SHORTCUT = 1U << 4,
    APP_REMOTE_SETTING_TRANSPORT = 1U << 5,
    APP_REMOTE_SETTING_TAP = 1U << 6,
    APP_REMOTE_SETTING_BLUETOOTH_NAME = 1U << 7,
    APP_REMOTE_SETTING_POINTER_GAIN = 1U << 8,
    APP_REMOTE_SETTING_DEADZONE = 1U << 9,
    APP_REMOTE_SETTING_FILTER = 1U << 10,
    APP_REMOTE_SETTING_MOTION_EXTRAS = 1U << 11,
    APP_REMOTE_SETTING_GAIN_X = 1U << 12,
    APP_REMOTE_SETTING_GAIN_Y = 1U << 13,
    APP_REMOTE_SETTING_INVERT_X = 1U << 14,
    APP_REMOTE_SETTING_INVERT_Y = 1U << 15,
    APP_REMOTE_SETTING_FREEZE = 1U << 16,
} app_remote_device_setting_t;
#define APP_REMOTE_SETTING_MOTION (APP_REMOTE_SETTING_POINTER_GAIN | APP_REMOTE_SETTING_DEADZONE | APP_REMOTE_SETTING_FILTER | \
    APP_REMOTE_SETTING_MOTION_EXTRAS | APP_REMOTE_SETTING_GAIN_X | APP_REMOTE_SETTING_GAIN_Y | APP_REMOTE_SETTING_INVERT_X | \
    APP_REMOTE_SETTING_INVERT_Y | APP_REMOTE_SETTING_FREEZE)

typedef struct {
    uint64_t management_generation;
    uint64_t resource_revision; /* Actual owner revision for the requested operation. */
    uint64_t settings_revision;
    uint64_t ui_revision;
    uint64_t bluetooth_generation, bond_revision;
    device_settings_t settings;
    bool locked, screen_awake, local_busy, transition_busy;
} app_remote_device_state_t;

/* Fully decoded action; no arbitrary JSON, password, private draft, UI pointer
 * or SDK handle crosses this port. Parent uses the same update_settings and
 * semantic/manager paths as native actions. token is a bounded job identity,
 * never evidence that a worker, radio, flash write or restart completed. */
typedef struct {
    cp_operation_id_t operation;
    uint64_t token, command_key, deadline_ms;
    cp_identity_t identity;
    uint64_t management_generation, resource_revision, settings_revision;
    uint64_t bluetooth_generation, bond_revision;
    uint32_t settings_fields;
    device_settings_t settings;
    bool durable;
    bool approval_required;
    bool duration_provided, strength_provided;
    uint32_t timeout_ms, window_seconds, duration_ms;
    uint8_t strength_percent;
    char peer_id[CP_TARGET_CAPACITY];
    char pattern[33];
    char overlay[17], page[17], group[33];
    char approval_id[CP_ID_HEX_CAPACITY];
    char retained_manifest_digest[65];
    uint8_t action_digest[32]; /* Stable action/identity/revision digest, no struct padding. */
} app_remote_device_request_t;

typedef enum {
    APP_REMOTE_DEVICE_PENDING,
    APP_REMOTE_DEVICE_EFFECTIVE,
    APP_REMOTE_DEVICE_COMPLETED,
    APP_REMOTE_DEVICE_CANCELLED,
    APP_REMOTE_DEVICE_FAILED,
    APP_REMOTE_DEVICE_UNCERTAIN,
    APP_REMOTE_DEVICE_CLEANUP_PENDING,
    APP_REMOTE_DEVICE_RESTART_PENDING,
} app_remote_device_phase_t;
typedef struct {
    app_remote_device_phase_t phase;
    cp_error_t error;
    uint64_t resource_revision;
    bool boundary_confirmed; /* Real owner acknowledgement, never submit success. */
    bool durable_confirmed;  /* Required before durable config can complete. */
    bool irreversible;
    char approval_id[CP_ID_HEX_CAPACITY];
} app_remote_device_outcome_t;

/* This is a COPY of the parent's existing immutable confirmation model. The
 * adapter cannot create physical presence or approve a hidden native dialog.
 * Parent creates IDs/digests when presenting an actual reviewable operation.
 * Restart/reset commands referencing absent approvals remain stale/unavailable.
 */
typedef struct {
    char id[CP_ID_HEX_CAPACITY];
    uint8_t action_digest[32];
    cp_operation_id_t operation;
    cp_identity_t owner;
    cp_scope_mask_t required_scopes;
    uint64_t management_generation, resource_revision, expires_ms;
    bool confirmed, consumed, cancelled;
    bool physical_required, physical_satisfied;
} app_remote_device_approval_t;

typedef struct {
    void *user;
    bool (*observe)(void *, cp_operation_id_t, app_remote_device_state_t *);
    bool (*available)(void *, cp_operation_id_t, uint32_t settings_fields, char reason[CP_REASON_CAPACITY]);
    /* Pure inert capability/dependency validation; no mutation or worker start. */
    cp_error_t (*validate_settings)(void *, const device_settings_t *, uint32_t fields, bool durable);
    cp_random_fn random;
    /* submit executes at most once per token; poll observes only that token.
     * Missing/unknown evidence must stay pending, failed or uncertain. The
     * parent must not mark queued restart/reset, brightness, calibration or
     * NVS changes completed before their actual common owning boundary. */
    cp_error_t (*submit)(void *, const app_remote_device_request_t *, app_remote_device_outcome_t *);
    bool (*poll)(void *, uint64_t token, app_remote_device_outcome_t *);
    cp_error_t (*cancel)(void *, uint64_t token);
    bool (*complete)(void *, const cp_result_t *); /* False retains the result locally. */
    bool (*approval)(void *, const char *id, app_remote_device_approval_t *);
    /* Inert reviewed proposal only. It must bind the supplied digest/identity/
     * revisions, generate a fresh opaque ID and never start the action. */
    cp_error_t (*approval_prepare)(void *, const app_remote_device_request_t *, app_remote_device_approval_t *);
    cp_error_t (*approval_decide)(void *, const app_remote_device_approval_t *, bool confirm);
    cp_error_t (*approval_claim)(void *, const app_remote_device_approval_t *);
    /* Optional coordinator-only cached board/system JSON builder. Parent
     * refreshes/copies network read caches; network tasks never call this port. */
    cp_read_fn read_cached;
} app_remote_device_port_t;

typedef struct app_remote_device app_remote_device_t;
/* Includes bounded per-instance parsing/execution scratch; execute allocates no
 * heap storage. The coordinator must serialize all calls on an instance. This
 * is not a thread-safe API. Same-instance execute reentry from a port callback
 * returns CP_BUSY without changing the active request; other instances remain
 * independent. Callback inputs are borrowed only for the duration of that
 * callback. Retained work must copy them, as this adapter does for jobs/stages.
 * Every owning execute return wipes its transient document and workspace. */
size_t app_remote_device_bytes(void);
/* Caller storage must satisfy both this alignment and the byte requirement. */
size_t app_remote_device_alignment(void);
bool app_remote_device_owns(cp_operation_id_t);
cp_error_t app_remote_device_init(void *memory, size_t bytes,
    const app_remote_device_port_t *, app_remote_device_t **);
bool app_remote_device_available(app_remote_device_t *, cp_operation_id_t,
    char reason[CP_REASON_CAPACITY]);
cp_error_t app_remote_device_execute(app_remote_device_t *, const cp_command_t *, uint64_t now_ms, cp_result_t *);
/* Only Input owner's already-authorized delegate may call this. Ordinary
 * execute rejects ui.* so it cannot bypass the shared lease/UI/epoch gates. */
cp_error_t app_remote_device_execute_ui_authorized(app_remote_device_t *, const cp_command_t *,
    uint64_t now_ms, cp_result_t *);
void app_remote_device_service(app_remote_device_t *, uint64_t now_ms);
void app_remote_device_revoke(app_remote_device_t *, cp_safety_reason_t,
    const cp_identity_t *, uint64_t now_ms);
/* Coordinator-only JSON snapshots. Parent may copy these into its protected
 * network read cache; no live coordinator state is accessed by network tasks.
 * Config output contains only supported non-secret desired/current fields. */
cp_error_t app_remote_device_read(app_remote_device_t *, const cp_identity_t *, const char *resource,
    char *json, size_t capacity, size_t *length);
