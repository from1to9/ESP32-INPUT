// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_control_server.h"
#include "control_grants.h"
#include "app_firmware_update.h"
#include "app_clock.h"

#define APP_CONTROL_TRUST_REVIEW_ABI 2U

#define APP_CONTROL_TRUST_PARTITION "control"
#define APP_CONTROL_TRUST_NAMESPACE "ctrl_v1"
#define APP_CONTROL_TRUST_PACKAGE_BYTES 8192U
#define APP_CONTROL_TRUST_CERT_BYTES 2048U
#define APP_CONTROL_TRUST_KEY_BYTES 1024U
#define APP_CONTROL_TRUST_WINDOW_MS 120000U

/* Identity package JSON v1 is created by bridge/commission.py, never committed:
 * v, host, port, certificate_pem, private_key_pem, principals[<=4] containing
 * id/credential_sha256/scopes/native_only/enabled. Raw client secrets stay in
 * the owner's Keychain/export, never the device package. Protected local import
 * only; no remote command or provisioning-HTTP route can open this window.
 * NVS partition control (64 KiB) is separate from existing Wi-Fi/bond NVS.
 * Two keys identity_a/identity_b store CTB1 + BE generation/length + SHA256 +
 * canonical package. No automatic erase or old-NVS migration. Development
 * at-rest protection is reported truthfully, not inferred from a private label. */
typedef struct {
    bool initialized, known, present, uncertain, import_window;
    bool at_rest_protected;
    uint64_t revision;
    esp_err_t error;
    uint32_t window_remaining_ms;
    bool review_pending;
    char candidate_digest[65];
    char active_digest[65]; /* Exact verified installed package; empty if unknown/absent. */
} app_control_trust_snapshot_t;
typedef struct {
    uint8_t id[CP_ID_BYTES];
    cp_scope_mask_t scopes;
    bool enabled, native_only;
} app_control_principal_review_t;
typedef struct {
    char digest[65], host[129];
    uint16_t port;
    uint8_t clock_server_count;
    char clock_servers[APP_CLOCK_SERVER_CAPACITY][129];
    uint8_t principal_count, capture_grant_count;
    cp_scope_mask_t principal_scopes[CP_MAX_PRINCIPALS];
    app_control_principal_review_t principals[CP_MAX_PRINCIPALS];
    cp_capture_grant_t grants[CP_MAX_CAPTURE_GRANTS];
    bool update_source_present, replace_unknown_store;
    char update_catalog_url[512], update_signing_key_id[65];
    char certificate_pem_sha256[65], update_public_key_sha256[65];
    char update_tls_ca_sha256[65], update_product[65], update_board[65], update_layout[65];
    uint32_t update_minimum_release_index;
    bool update_allow_private_source;
    bool update_policy_present;
    app_update_policy_t update_policy;
} app_control_trust_review_t;

/* Task-only storage operations; caller schedules outside UI/HID callbacks. */
esp_err_t app_control_trust_init(void);
esp_err_t app_control_trust_open_local_window(uint8_t token[CP_SECRET_BYTES]);
void app_control_trust_close_local_window(void);
/* A local UI must present this exact summary and explicitly approve its
 * digest. USB/network data can stage a proposal, never approve local Review. */
esp_err_t app_control_trust_stage(const uint8_t token[CP_SECRET_BYTES], const char *, size_t);
bool app_control_trust_review(app_control_trust_review_t *);
esp_err_t app_control_trust_approve_local(const char digest[65]);
esp_err_t app_control_trust_import(const uint8_t token[CP_SECRET_BYTES], const char *package_json,
                                   size_t length);
esp_err_t app_control_trust_verify(void);
void app_control_trust_snapshot(app_control_trust_snapshot_t *);
/* Loads/validates the protected store and starts the actual station TLS server
 * with coordinator callbacks. Unavailable/uncertain storage fails closed. */
esp_err_t app_control_trust_start_server(const cp_callbacks_t *coordinator);
size_t app_control_trust_grants(const uint8_t principal[CP_ID_BYTES],
                                cp_capture_grant_t out[CP_MAX_CAPTURE_GRANTS]);
bool app_control_trust_grant(const uint8_t id[CP_ID_BYTES], cp_capture_grant_t *);
size_t app_control_trust_clock_servers(char out[2][129]);
bool app_control_trust_update_config(app_update_provisioned_config_t *);
bool app_control_trust_update_policy(const uint8_t principal[16], app_update_policy_t *);
