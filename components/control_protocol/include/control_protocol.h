// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CP_API_VERSION 1U
#define CP_MAX_FRAME_BYTES 2048U
#define CP_MAX_ARGS_BYTES 1280U
#define CP_MAX_RESPONSE_BYTES 4096U
#define CP_MAX_PRINCIPALS 4U
#define CP_MAX_SESSIONS 2U
#define CP_MAX_RESULTS 64U
#define CP_MAX_ACTIVE_REQUESTS 16U
#define CP_ID_BYTES 16U
#define CP_SECRET_BYTES 32U
#define CP_ID_HEX_CAPACITY 33U
#define CP_SECRET_HEX_CAPACITY 65U
#define CP_TARGET_CAPACITY 65U
#define CP_REASON_CAPACITY 65U

typedef uint16_t cp_operation_id_t;
typedef uint64_t cp_scope_mask_t;
typedef enum {
    CP_OK,
    CP_INVALID,
    CP_TOO_LARGE,
    CP_UNAUTHENTICATED,
    CP_FORBIDDEN,
    CP_UNAVAILABLE,
    CP_NOT_IMPLEMENTED,
    CP_LOCKED,
    CP_STALE,
    CP_EXPIRED,
    CP_BUSY,
    CP_SEQUENCE_GAP,
    CP_REQUEST_CONFLICT,
    CP_RESULT_UNKNOWN,
    CP_OWNER_BUSY,
    CP_LOCAL_BUSY,
    CP_RELEASE_PENDING,
    CP_STORAGE_UNCERTAIN,
    CP_INTERNAL,
    CP_TOO_LATE
} cp_error_t;
typedef enum {
    CP_RESULT_REJECTED,
    CP_RESULT_ACCEPTED,
    CP_RESULT_RUNNING,
    CP_RESULT_APPLIED,
    CP_RESULT_SENT,
    CP_RESULT_COMPLETED,
    CP_RESULT_CANCELLED,
    CP_RESULT_FAILED,
    CP_RESULT_CLEANUP_PENDING
} cp_result_state_t;
typedef struct {
    cp_result_state_t state;
    cp_error_t error;
    uint64_t command_key;
    uint64_t job_id;
    uint64_t resource_revision;
    char object_id[CP_ID_HEX_CAPACITY]; /* Optional transaction/artifact ID. */
    bool progress_known;
    uint32_t started_prefix;
    uint32_t completed_pairs;
    int32_t uncertain_index; /* -1 means none; never a host-outcome assertion. */
    char reason[CP_REASON_CAPACITY];
} cp_result_t;

typedef struct {
    uint8_t principal_id[CP_ID_BYTES];
    uint8_t session_id[CP_ID_BYTES];
    cp_scope_mask_t scopes;
    bool browser;
} cp_identity_t;

/* Immutable bounded dispatch payload. Arguments have already passed the shared
 * typed registry. The coordinator still rechecks mutable production gates at
 * execution. Copy before returning ACCEPTED; no callback retains this pointer.
 * Sensitive args (Wi-Fi secrets, etc.) must never enter UI/event/log snapshots. */
typedef struct {
    uint32_t version;
    cp_operation_id_t operation;
    uint64_t command_key;
    cp_identity_t identity;
    uint64_t deadline_ms;
    uint64_t management_generation;
    uint64_t resource_revision;
    uint64_t input_epoch;
    uint64_t transport_generation;
    uint64_t ui_revision;
    uint64_t lease_expires_ms; /* Trusted proposed acquire/renew deadline. */
    uint8_t lease_id[CP_ID_BYTES];
    char target_id[CP_TARGET_CAPACITY];
    char args_json[CP_MAX_ARGS_BYTES + 1U];
} cp_command_t;

/* Coordinator observation, never supplied by a client. A local physical
 * takeover/lock must first gate the actual production coordinator immediately;
 * publishing takeover here revokes only the input lease and stale input work.
 * Authenticated management sessions and independent media jobs remain governed
 * by their own authority/liveness. Lock/privacy stop are separate boundaries. */
typedef struct {
    uint64_t management_generation;
    uint64_t input_epoch;
    uint64_t transport_generation;
    uint64_t ui_revision;
    uint64_t state_sequence;
    uint64_t allowance_generation;
    char target_id[CP_TARGET_CAPACITY];
    char mode[12];
    bool locked;
    bool input_ready;
    bool release_pending;
    bool local_busy;
    bool remote_allowed;
    bool remote_owner;
    uint8_t owner_session[CP_ID_BYTES];
} cp_device_state_t;

typedef enum {
    CP_SAFETY_LEASE_EXPIRED,
    CP_SAFETY_SESSION_LOST,
    CP_SAFETY_REVOKED,
    CP_SAFETY_LOCAL_OVERRIDE,
    CP_SAFETY_LOCKED,
    CP_SAFETY_STATE_CHANGED,
    CP_SAFETY_REQUEST_CONFLICT,
    CP_SAFETY_CLIENT_STOP
} cp_safety_reason_t;

typedef struct {
    cp_operation_id_t id;
    const char *name;
    cp_scope_mask_t scopes;
    bool input;
    bool ui_context;
    bool safety;
    bool read_only;
    bool sensitive;
    bool initial_v1_supported;
} cp_operation_info_t;

/* These callbacks never block on hardware, flash, network, or LVGL. ACCEPTED
 * means the owning coordinator reserved a bounded work item, not completion.
 * Missing capability/provider returns explicit unavailable/not implemented.
 * A completion must later name command_key and cannot resurrect cancelled work. */
typedef bool (*cp_available_fn)(void *, cp_operation_id_t, char reason[CP_REASON_CAPACITY]);
typedef cp_error_t (*cp_dispatch_fn)(void *, const cp_command_t *, cp_result_t *);
typedef void (*cp_safety_fn)(void *, cp_safety_reason_t, const cp_identity_t *);
typedef cp_error_t (*cp_read_fn)(void *, const cp_identity_t *, const char *resource, char *json,
                                 size_t capacity, size_t *length);
typedef bool (*cp_random_fn)(void *, uint8_t *, size_t);

typedef struct {
    void *user;
    cp_random_fn random;
    cp_available_fn available;
    cp_dispatch_fn dispatch;
    cp_safety_fn safety;
    cp_read_fn read;
} cp_callbacks_t;

const cp_operation_info_t *cp_operation_find(const char *name);
const cp_operation_info_t *cp_operation_at(size_t index);
size_t cp_operation_count(void);
const char *cp_error_name(cp_error_t error);
const char *cp_result_state_name(cp_result_state_t state);
bool cp_command_string(const cp_command_t *, const char *key, char *out, size_t capacity);
bool cp_command_integer(const cp_command_t *, const char *key, int64_t *out);
bool cp_command_boolean(const cp_command_t *, const char *key, bool *out);
/* Retrieve a validated nested object/array as bounded canonical JSON. */
bool cp_command_json(const cp_command_t *, const char *key, char *out, size_t capacity);

typedef struct cp_context cp_context_t;
size_t cp_context_size(void);
cp_error_t cp_initialize(void *storage, size_t capacity, const cp_callbacks_t *, cp_context_t **);
void cp_set_device_state(cp_context_t *, const cp_device_state_t *, uint64_t now_ms);
void cp_tick(cp_context_t *, uint64_t now_ms);
void cp_shutdown(cp_context_t *, uint64_t now_ms);
/* Serialized protocol-owner observation only. Other tasks consume a copy from
 * the server cache, never the protocol context. Media liveness is independent
 * of the input lease and expires 5 seconds after authenticated activity. */
typedef struct {
    cp_identity_t identity;
    uint64_t last_authenticated_ms;
    bool authenticated;
} cp_session_view_t;
size_t cp_sessions_snapshot(const cp_context_t *, cp_session_view_t out[CP_MAX_SESSIONS]);
void cp_physical_override(cp_context_t *, bool locked, uint64_t now_ms);
cp_error_t cp_complete(cp_context_t *, uint64_t command_key, const cp_result_t *, uint64_t now_ms);

/* Trust material is injected only by a protected local enrollment/storage
 * provider. No HTTP request can call this API or assert physical presence. */
typedef struct {
    uint8_t id[CP_ID_BYTES];
    uint8_t credential_sha256[CP_SECRET_BYTES];
    cp_scope_mask_t scopes;
    bool native_only;
    bool enabled;
} cp_principal_t;
cp_error_t cp_load_principal(cp_context_t *, const cp_principal_t *);
cp_error_t cp_revoke_principal(cp_context_t *, const uint8_t id[CP_ID_BYTES], uint64_t now_ms);
void cp_set_transport_capabilities(cp_context_t *, bool websocket, bool binary_download);

typedef struct {
    bool tls;
    const char *host;
    const char *origin;        /* NULL only for a separately enrolled native principal. */
    const char *authorization; /* Bearer session token, never URL/query secret. */
    const char *cookie;
    const char *csrf;
} cp_http_security_t;
/* Trusted TLS transport entry points, serialized by the same protocol owner as
 * cp_http. Established channel identity is an internal copy from successful
 * authentication, never a client-provided identity or presence assertion. */
cp_error_t cp_transport_authenticate(cp_context_t *, const cp_http_security_t *, bool mutation,
                                     uint64_t now_ms, cp_identity_t *);
cp_error_t cp_transport_heartbeat(cp_context_t *, const cp_identity_t *, uint64_t now_ms);
/* Browser upgrades consume an exact-session 10-second ticket issued through
 * CSRF-protected HTTP. Native upgrades use enrolled Bearer authentication. */
cp_error_t cp_transport_upgrade(cp_context_t *, const cp_http_security_t *, const char *ticket,
                                uint64_t now_ms, cp_identity_t *);
/* Dedicated WSS loss revokes only that session's input lease/queued input with
 * STATE_CHANGED. Independent HTTP management/file capture and authentication
 * retain their own liveness/grants. Ordinary HTTP close never calls this. */
cp_error_t cp_transport_close(cp_context_t *, const cp_identity_t *, uint64_t now_ms);
/* Server/bridge consume this same bounded HTTP engine. The caller guarantees
 * a real verified station-only TLS listener; tls=false always fails closed.
 * The response never contains private drafts or an unredacted argument echo. */
typedef struct {
    unsigned status;
    size_t length;
    char body[CP_MAX_RESPONSE_BYTES + 1U];
    char set_cookie[256];
} cp_http_response_t;
cp_error_t cp_transport_frame(cp_context_t *, const cp_identity_t *, const char *frame,
                              size_t length, uint64_t now_ms, cp_http_response_t *);
cp_error_t cp_http_configure(cp_context_t *, const char *host, const char *browser_origin);
cp_error_t cp_http(cp_context_t *, const char *method, const char *resource,
                   const cp_http_security_t *, const char *body, size_t body_length,
                   uint64_t now_ms, cp_http_response_t *);
void cp_sha256(const void *, size_t, uint8_t output[CP_SECRET_BYTES]);
