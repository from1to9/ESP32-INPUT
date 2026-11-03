// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_UPDATE_ABI 1U
#define APP_UPDATE_ID_CAPACITY 65U
#define APP_UPDATE_REASON_CAPACITY 65U
#define APP_UPDATE_SLOT_BYTES 0x600000U
#define APP_UPDATE_LAYOUT "esp32s31-16m-ota6-v3"
typedef enum {
    APP_UPDATE_UNAVAILABLE,
    APP_UPDATE_IDLE,
    APP_UPDATE_CHECKING,
    APP_UPDATE_AVAILABLE,
    APP_UPDATE_WAITING_MAINTENANCE,
    APP_UPDATE_DOWNLOADING,
    APP_UPDATE_VERIFYING,
    APP_UPDATE_STAGED,
    APP_UPDATE_SELECTING_BOOT,
    APP_UPDATE_RESTART_PENDING,
    APP_UPDATE_PENDING_HEALTH,
    APP_UPDATE_CONFIRMED,
    APP_UPDATE_CANCELLING,
    APP_UPDATE_CANCELLED,
    APP_UPDATE_ERROR,
    APP_UPDATE_BOOT_SELECTION_UNCERTAIN
} app_update_state_t;
typedef struct {
    app_update_state_t state;
    uint64_t job_id, revision, policy_revision;
    uint32_t received_bytes, written_bytes, total_bytes;
    bool available, cleanup_pending, boot_selected, running_expected;
    bool running_state_known, running_confirmed, probation_active, health_confirmation_attempted;
    esp_err_t error;
    esp_err_t running_state_error, health_confirmation_error;
    char release_id[APP_UPDATE_ID_CAPACITY];
    char digest[65];
    uint64_t trust_revision;
    char reason[APP_UPDATE_REASON_CAPACITY];
} app_update_snapshot_t;
typedef struct {
    uint32_t abi_version;
    const char *catalog_url, *tls_ca_pem;
    const char *signing_key_id;
    const uint8_t *signing_public_key; /* P-256 uncompressed point, 65 bytes. */
    const char *product, *board, *layout;
    bool allow_private_source;
    uint32_t minimum_release_index;
    uint64_t trust_revision;
} app_update_config_t;
typedef struct {
    uint8_t id[16], principal_id[16];
    uint64_t revision, expires_unix_seconds;
    uint32_t maximum_job_ms, maximum_bytes;
    bool allow_download, allow_activate, allow_restart, continue_after_owner_loss;
    bool allow_signed_releases;
    char allowed_digest[65];
} app_update_policy_t;
typedef struct {
    char catalog_url[512], tls_ca_pem[2049], signing_key_id[65];
    char product[65], board[65], layout[65];
    uint8_t signing_public_key[65];
    uint32_t minimum_release_index;
    bool allow_private_source;
} app_update_provisioned_config_t;
typedef struct {
    /* Observation only; known flags distinguish missing evidence from valid
     * zero values such as CP_OK and a zero monotonic timestamp. */
    uint64_t observed_ms, owner_cached_ms;
    uint32_t owner_rejected;
    int32_t runtime_status;
    bool runtime_status_known, owner_observed;
} app_update_authority_observation_t;
enum {
    APP_UPDATE_OWNER_STALE = 1U << 0,
    APP_UPDATE_OWNER_CLAIM = 1U << 1,
    APP_UPDATE_OWNER_EXPIRED = 1U << 2,
    APP_UPDATE_OWNER_LOCKED = 1U << 3,
    APP_UPDATE_OWNER_POLICY = 1U << 4,
    APP_UPDATE_OWNER_GENERATION = 1U << 5,
    APP_UPDATE_OWNER_HID = 1U << 6,
    APP_UPDATE_OWNER_TRANSPORT = 1U << 7,
    APP_UPDATE_OWNER_PAIRING = 1U << 8,
    APP_UPDATE_OWNER_MEDIA_UNKNOWN = 1U << 9,
    APP_UPDATE_OWNER_MEDIA = 1U << 10,
    APP_UPDATE_OWNER_FLASH = 1U << 11,
    APP_UPDATE_OWNER_NOT_READY = 1U << 12
};
typedef struct {
    uint64_t policy_revision, authority_expires_ms;
    bool authorized, unlocked, maintenance_ready, restart_authorized;
    char expected_digest[65];
    app_update_authority_observation_t observation;
} app_update_authority_t;
typedef enum {
    APP_UPDATE_FAILURE_NONE, APP_UPDATE_FAILURE_RUNNING_STATE,
    APP_UPDATE_FAILURE_RELEASE_TIME, APP_UPDATE_FAILURE_LAYOUT,
    APP_UPDATE_FAILURE_MAINTENANCE, APP_UPDATE_FAILURE_SOURCE,
    APP_UPDATE_FAILURE_CONTENT_LENGTH, APP_UPDATE_FAILURE_BUFFER,
    APP_UPDATE_FAILURE_BEFORE_BEGIN, APP_UPDATE_FAILURE_OTA_BEGIN,
    APP_UPDATE_FAILURE_BEFORE_READ, APP_UPDATE_FAILURE_HTTP_READ,
    APP_UPDATE_FAILURE_BEFORE_WRITE, APP_UPDATE_FAILURE_HASH_UPDATE,
    APP_UPDATE_FAILURE_OTA_WRITE, APP_UPDATE_FAILURE_STREAM_DIGEST,
    APP_UPDATE_FAILURE_BEFORE_END, APP_UPDATE_FAILURE_OTA_END,
    APP_UPDATE_FAILURE_READBACK, APP_UPDATE_FAILURE_READBACK_DIGEST,
    APP_UPDATE_FAILURE_DESCRIPTION, APP_UPDATE_FAILURE_IMAGE_IDENTITY,
    APP_UPDATE_FAILURE_BEFORE_STAGE, APP_UPDATE_FAILURE_OTHER,
    APP_UPDATE_FAILURE_MAINTENANCE_TIMEOUT
} app_update_failure_phase_t;
enum {
    APP_UPDATE_DENIED_CANCEL = 1U << 0, APP_UPDATE_DENIED_AUTHORIZED = 1U << 1,
    APP_UPDATE_DENIED_UNLOCKED = 1U << 2, APP_UPDATE_DENIED_MAINTENANCE = 1U << 3,
    APP_UPDATE_DENIED_EXPIRED = 1U << 4, APP_UPDATE_DENIED_CLOCK = 1U << 5,
    APP_UPDATE_DENIED_JOB_TIME = 1U << 6, APP_UPDATE_DENIED_POLICY = 1U << 7,
    APP_UPDATE_DENIED_RESTART = 1U << 8, APP_UPDATE_DENIED_DIGEST = 1U << 9
};

/* No initialization/download/activation resumes old executable work. Only
 * actual inactive OTA_0/OTA_1 partitions are writable; factory/NVS/bootloader/
 * partition table/eFuse/model/recording/control regions are never update targets.
 * Trust/policy and actual coordinator maintenance must be rechecked at execution.
 * APIs admit worker work; snapshots distinguish staged/selected/running/valid. */
/* Slot writes require an actual VALID running-image observation. Missing OTA
 * state (including an initial image without a valid OTA metadata entry) is
 * unavailable, never inferred to be confirmed or repaired automatically. Such
 * initial metadata belongs to the separately authorized migration/recovery.
 * Probation permits one health-confirmation attempt, verified by image-state
 * readback, and retains its fixed deadline. Rollback is attempted at most once
 * and only while actual state is still PENDING_VERIFY; unknown write outcomes
 * remain quarantined rather than replaying destructive metadata operations. */
/* Confirming boot health cannot override failed updater configuration or
 * downgrade-floor validation. OTA availability needs independent validation. */
esp_err_t app_update_init(const app_update_config_t *);
/* Replace configuration only with no queued/running/staged job. The trusted
 * local importer owns this call; no ordinary command accepts a source URL. */
esp_err_t app_update_configure(const app_update_config_t *);
esp_err_t app_update_check(void);
esp_err_t app_update_download(const char *release_id, const app_update_authority_t *);
esp_err_t app_update_activate(uint64_t job_id, const app_update_authority_t *);
esp_err_t app_update_discard(uint64_t job_id);
/* A staged/queued cancellation discards only RAM authority, not slot bytes.
 * Active work drains at its actual safe boundary; already terminal work rejects
 * cancellation. Boot-selection/rollback uncertainty is never undone here. */
esp_err_t app_update_cancel(uint64_t job_id);
void app_update_publish_authority(const app_update_authority_t *);
void app_update_snapshot(app_update_snapshot_t *);
/* Appendable JSON value for this exact observed job/revision; null before a
 * terminal failure or for a replaced snapshot. The first download guard/SDK
 * failure survives cleanup and authority refreshes until next admission.
 * Missing source observations are null; denied=0 means no authorization
 * predicate failed at this checkpoint (for example, an SDK error). No IDs. */
bool app_update_failure_json(uint64_t job_id, uint64_t revision, char *, size_t capacity, size_t *length);
/* Application-controlled probation; true means all required local gates were
 * actually observed, not HID-host/Internet presence. No early auto-confirm. */
esp_err_t app_update_confirm_health(bool required_gates_passed);
bool app_update_flash_busy(void);
