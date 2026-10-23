// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_control_server.h"
#include "app_control_trust.h"
#include "app_wifi_profiles.h"
#include "app_firmware_update.h"
#include "app_clock.h"
#include "control_registry.h"

#define APP_CONTROL_RUNTIME_ABI 1U

/* Parent owns this nonblocking host port. All other operations are unavailable
 * unless their own manager actually supports them. Parent handles:
 * - control.acquire/renew/release, input.*, mouse.*, keyboard.*, selection.*,
 *   preview.*, touch.*, device.lock, imu.calibrate, semantic ui.*;
 * - config.stage/apply/cancel/reset through the shared settings/manager owner;
 * - bluetooth.*, haptics.*, display.*, operation.confirm/cancel;
 * - audio.*, speech.*, recording.*, recordings.*, job.get/cancel for those jobs;
 * - system.restart, preferences.reset, device.factory_reset.request.
 * Runtime owns Wi-Fi profiles/scans/join, firmware.*, local trust/session
 * administration, copied sensor telemetry and network clock/status. Neither side bypasses the other's
 * production gates; local override revokes input, not independent media. */
typedef struct {
    uint32_t abi_version;
    void *user;
    cp_available_fn available;
    cp_dispatch_fn submit;
    cp_safety_fn safety;
    cp_read_fn read;
    /* Nonblocking policy/maintenance handshake for firmware download/activate.
     * It may latch a parent maintenance request and return authorized=true,
     * maintenance_ready=false while actual input release is pending. Missing
     * callback keeps flash update capability unavailable. Re-evaluated during
     * the job; scope/policy matching cannot be replaced by a caller Boolean. */
    cp_error_t (*update_authority)(void *, const cp_command_t *, app_update_authority_t *);
} app_control_host_t;
typedef struct {
    bool initialized, commissioning_open, usb_import_active;
    app_control_server_snapshot_t server;
    app_control_trust_snapshot_t trust;
    app_clock_snapshot_t clock;
    esp_err_t error;
    uint32_t revision;
} app_control_runtime_snapshot_t;

/* Init copies the host port and starts one bounded management worker. The
 * worker loads control NVS and starts station-only HTTPS once trust and LAN
 * are actually ready. No automatic AP, trust defaults, flash migration or reset.
 * ESP_OK is worker admission only. No USB installation/reinstallation here. */
esp_err_t app_control_runtime_init(const app_control_host_t *);
esp_err_t app_control_runtime_publish_state(const cp_device_state_t *);
esp_err_t app_control_runtime_complete(uint64_t command_key, const cp_result_t *);
void app_control_runtime_physical_override(bool locked);
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *);
bool app_control_runtime_wall_time(uint64_t *unix_seconds);
/* Pure bounded cached policy check. Parent combines it with actual coordinator
 * maintenance. It never reads coordinator-owned mutable state. */
cp_error_t app_control_runtime_update_authority(const cp_command_t *, app_update_authority_t *);
bool app_control_runtime_session_alive(const uint8_t session[CP_ID_BYTES],
                                       const uint8_t principal[CP_ID_BYTES]);
cp_error_t app_control_runtime_capture_authority(const cp_capture_authority_request_t *,
                                                 cp_capture_authority_t *);
bool app_control_runtime_commissioning_review(app_control_trust_review_t *);
esp_err_t app_control_runtime_approve_commissioning(const char digest[65]);
/* Local file/USB proposal only. This never approves the proposal. */
esp_err_t app_control_runtime_stage_commissioning(const char *json, size_t length);
/* Call only from a trusted unlocked local enrollment action. It does not expose
 * a remote presence flag. Window token travels only over the explicitly local
 * commissioning exchange, never ordinary logs/state/URLs. */
esp_err_t app_control_runtime_open_commissioning(void);
void app_control_runtime_close_commissioning(void);
/* Parent calls once AFTER existing usb_console_start succeeded. Uses its
 * nonblocking, non-echoing CDC VFS stdin; no second USB owner, parser/reset
 * handler in diagnostic stdout, endpoint or descriptor change. Import is read
 * only during an explicitly opened local window. Nothing touches USB in host
 * validation unless a boundary substitute is installed by the test harness. */
esp_err_t app_control_runtime_attach_usb_stdio(void);
