// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "control_protocol.h"

#define APP_CONTROL_SERVER_ABI 1U

typedef struct {
    uint32_t abi_version;
    const char *station_ifkey;  /* e.g. WIFI_STA_DEF; never provisioning AP. */
    const char *allowed_host;   /* Exact Host, including non-default port. */
    const char *browser_origin; /* Exact HTTPS origin; no wildcard/null origin. */
    const uint8_t *certificate_pem;
    size_t certificate_length;
    const uint8_t *private_key_pem;
    size_t private_key_length;
    uint16_t port; /* HTTPS only; zero selects 443. */
    cp_callbacks_t coordinator;
    const cp_principal_t *principals; /* Verified protected local-store snapshot. */
    size_t principal_count;
} app_control_server_config_t;

typedef enum {
    APP_CONTROL_STOPPED,
    APP_CONTROL_STARTING,
    APP_CONTROL_READY,
    APP_CONTROL_STOPPING,
    APP_CONTROL_ERROR
} app_control_server_state_t;
typedef struct {
    app_control_server_state_t state;
    bool tls;
    bool enrollment_available;
    bool websocket_available;
    uint32_t revision;
    esp_err_t error;
    char address[256]; /* Populated only by an actually running listener. */
} app_control_server_snapshot_t;

/* Task-only startup; copies config strings/trust before returning. Real TLS
 * material and protected principals are mandatory. No example/default secret,
 * HTTP downgrade, provisioning-server reuse, auto-enrollment or device write.
 * Coordinator callbacks are nonblocking; read must scope/redact its output. */
esp_err_t app_control_server_start(const app_control_server_config_t *);
esp_err_t app_control_server_stop(void);
void app_control_server_snapshot(app_control_server_snapshot_t *);
/* Nonblocking coordinator-to-server mailboxes. Physical takeover must already
 * gate production input before calling this notification; no network roundtrip. */
esp_err_t app_control_server_publish_state(const cp_device_state_t *);
esp_err_t app_control_server_complete(uint64_t command_key, const cp_result_t *);
void app_control_server_physical_override(bool locked);
/* Cached, bounded lookup; safe from media/coordinator tasks. HTTP connection
 * close does not destroy a logical session. Explicit logout/revoke does. */
bool app_control_server_session_alive(const uint8_t session_id[CP_ID_BYTES],
                                      const uint8_t principal_id[CP_ID_BYTES]);
/* Exact identity includes the current enrolled scopes, not only two IDs. */
bool app_control_server_identity_alive(const cp_identity_t *);
/* Authenticated activity within 5 seconds and a currently valid credential. */
bool app_control_server_client_connected(const uint8_t principal_id[CP_ID_BYTES]);
/* Network media callbacks are pure bounded owner-cache checks / copied ingress.
 * The audio owner validates every immutable job/generation/format/sequence and
 * current grant. Missing hooks keep binary live media unavailable. */
typedef struct {
    void *user;
    bool (*capture_allowed)(void *, const cp_identity_t *, const uint8_t job_id[16],
                            uint64_t consumer_generation, uint64_t channel_generation);
    void (*capture_failed)(void *, const cp_identity_t *, const uint8_t job_id[16],
                           uint64_t consumer_generation, uint64_t channel_generation, cp_error_t);
    cp_error_t (*playback)(void *, const cp_identity_t *, uint64_t channel_generation,
                           const uint8_t *, size_t);
    void (*channel_lost)(void *, const cp_identity_t *, uint64_t channel_generation);
} app_control_media_port_t;
void app_control_server_set_media_port(const app_control_media_port_t *);
/* Nonzero immutable channel generation; zero means no current channel. Pin at
 * live-job admission and carry on ingress/egress; never silently rebind a job. */
uint64_t app_control_server_channel_generation(const cp_identity_t *);
/* Complete ACM1 frame copied before return, max 2048 bytes. ESP_OK is bounded
 * queue acceptance, not socket/host receipt. No queue survives owner loss. */
esp_err_t app_control_server_capture_send(const cp_identity_t *, const uint8_t job_id[16],
                                          uint64_t consumer_generation, uint64_t channel_generation,
                                          const uint8_t *, size_t);
void app_control_server_capture_cancel(const cp_identity_t *, const uint8_t job_id[16],
                                       uint64_t consumer_generation, uint64_t channel_generation);
