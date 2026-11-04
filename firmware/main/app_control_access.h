// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"
#include "esp_err.h"
#include "app_firmware_update.h"

#define APP_CONTROL_ACCESS_CLIENTS CP_MAX_PRINCIPALS
#define APP_CONTROL_ACCESS_LABEL 33U
enum {
    APP_CONTROL_ACCESS_INPUT = 1U,
    APP_CONTROL_ACCESS_MANAGE = 2U,
    APP_CONTROL_ACCESS_UPDATE = 4U
};
typedef enum {
    APP_CONTROL_ACCESS_SIX_HOURS,
    APP_CONTROL_ACCESS_DAY,
    APP_CONTROL_ACCESS_PERMANENT
} app_control_access_duration_t;
typedef struct {
    uint8_t id[CP_ID_BYTES];
    char label[APP_CONTROL_ACCESS_LABEL], fingerprint[65];
    uint8_t permissions;
    app_control_access_duration_t duration;
    uint64_t revision, expires_unix_seconds;
    /* legacy marks untouched imported scopes; edited native clients use the groups. */
    bool legacy, enabled, revoked, expired, clock_unknown, connected, controlling;
} app_control_access_client_t;
typedef enum {
    APP_CONTROL_ACCESS_APPROVE,
    APP_CONTROL_ACCESS_EDIT,
    APP_CONTROL_ACCESS_REVOKE,
    APP_CONTROL_ACCESS_REJECT
} app_control_access_action_t;
typedef struct {
    app_control_access_action_t action;
    uint64_t expected_revision, pairing_request;
    uint8_t client_id[CP_ID_BYTES], permissions;
    app_control_access_duration_t duration;
} app_control_access_request_t;
typedef struct {
    bool known, uncertain, pending;
    esp_err_t error;
    uint64_t revision, pairing_request;
    uint32_t pairing_remaining_ms;
    app_control_access_client_t proposal;
    uint8_t count;
    app_control_access_client_t clients[APP_CONTROL_ACCESS_CLIENTS];
    uint64_t receipt_request;
    cp_result_state_t receipt_state;
    cp_error_t receipt_error;
    char address[256];
} app_control_access_snapshot_t;

/* A proposal starts with INPUT/6h; it does not constrain local scope choices.
 * EDIT can reapprove legacy/expired/unknown/revoked entries. REVOKE is valid
 * for every saved entry. APPROVE/EDIT require 1..7 permission bits; REVOKE
 * alone clears all permissions. Finite duration needs known trusted time.
 * receipt_request matches the returned local request ID; only APPLIED proves
 * verified persistence. Failure/uncertain never replaces it with success.
 * Nonblocking local-only ingress. QUEUED/ACCEPTED is never durable success.
 * Runtime checks trusted local unlocked state; service owns all NVS writes. */
esp_err_t app_control_runtime_access_request(const app_control_access_request_t *, uint64_t *request);
void app_control_access_snapshot(app_control_access_snapshot_t *);

/* Management/server owner integration; no UI or hardware dependencies. */
esp_err_t app_control_access_init(void);
void app_control_access_service(bool local_unlocked);
cp_error_t app_control_access_submit(const app_control_access_request_t *, uint64_t *request);
bool app_control_access_principal(const uint8_t id[CP_ID_BYTES], cp_principal_t *);
bool app_control_access_input(const cp_identity_t *);
bool app_control_access_any_input(void);
bool app_control_access_managed(const uint8_t id[CP_ID_BYTES]);
bool app_control_access_update_policy(const uint8_t id[CP_ID_BYTES], app_update_policy_t *);
cp_error_t app_control_access_pair(const char *client, const char *digest, const char *label,
                                   uint64_t *request);
cp_error_t app_control_access_pair_status(const char *client, const char *credential,
                                          char *out, size_t capacity, size_t *length);
