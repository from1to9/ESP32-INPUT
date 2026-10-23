// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "device_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HID_PAIRING_DEFAULT_MS 120000U
#define BLE_HID_PAIRING_MAX_MS 300000U
#define BLE_HID_MAX_BONDS 32U
#define BLE_HID_NAME_MAX_BYTES DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES
#define BLE_HID_DEFAULT_NAME DEVICE_SETTINGS_BLUETOOTH_DEFAULT_NAME
#define BLE_HID_NAME_TIMEOUT_MS 5000U
#define BLE_HID_BOND_ID_BYTES 32U

typedef enum {
    BLE_HID_NAME_IDLE,
    BLE_HID_NAME_PENDING,
    BLE_HID_NAME_APPLYING,
    BLE_HID_NAME_APPLIED,
    BLE_HID_NAME_FAILED,
    BLE_HID_NAME_UNCERTAIN
} ble_hid_name_status_t;

typedef struct {
    uint64_t request_id;
    ble_hid_name_status_t status;
    char requested[BLE_HID_NAME_MAX_BYTES + 1U];
    char active[BLE_HID_NAME_MAX_BYTES + 1U];
    bool active_known;
    esp_err_t error;
    bool sdk_status_known;
    uint32_t sdk_status;
    bool rollback_attempted;
    bool rollback_confirmed;
    esp_err_t rollback_error;
} ble_hid_name_snapshot_t;

/* Pure shared validation: 1..18 UTF-8 bytes, no malformed/overlong encodings,
 * surrogates, NUL/C0/C1 controls or leading/trailing ASCII space. The complete
 * name fits the existing 31-byte primary advertisement with HID UUID/flags/
 * appearance. length excludes NUL; the input need not be NUL-terminated. */
bool ble_hid_name_valid(const char *utf8, size_t length);

/* Task-only, bounded copy and submission; no Bluetooth/NVS calls or allocation.
 * One request at a time. May be submitted before init (then used by init), or
 * after initialized. Rejects partial init, fault, forgetting or an uncertain
 * previous operation. request_id may be NULL. ESP_OK is acceptance only.
 * An already verified identical name is an immediate APPLIED no-op.
 * Rejected requests do not replace the current request or change persistence.
 * Parent owns durable settings and should persist only matching APPLIED.
 *
 * Worker verifies the SDK GAP name and both advertising payload completions,
 * refreshing advertising without disconnecting a current HID host. Disconnected
 * apply also waits for advertising restart. Each apply/rollback has a 5 s
 * deadline. Reported failures restore and verify the previous bounded name;
 * FAILED plus rollback_confirmed proves restoration when it was needed.
 * Missing callbacks or failed rollback report UNCERTAIN, block further name
 * requests/advertising restart until reboot, and never claim a rollback.
 * Bonds, local identity, security, HID generations and subscriptions are not
 * changed. Host name caches may remain stale until the host refreshes them. */
esp_err_t ble_hid_name_request(const char *utf8, size_t length, uint64_t *request_id);
/* Thread-safe cached copy. active_known refers to verified SDK name/payload
 * state; before verification active is unavailable. No host receipt claim. */
ble_hid_name_snapshot_t ble_hid_name_snapshot(void);

typedef struct {
    bool recorded;
    /* Non-null static stage name; "none" before any failure. No identities,
     * report content, keys, allocation or caller-owned string storage. */
    const char *stage;
    esp_err_t error;
    bool sdk_status_known;
    uint32_t sdk_status;
    /* Monotonic esp_timer uptime in milliseconds, independent of tick wrap. */
    uint64_t uptime_ms;
} ble_hid_failure_t;

#define BLE_HID_ADVERTISING_TRACE_CAPACITY 8U
typedef enum {
    BLE_HID_ADV_START_REQUEST = 1, BLE_HID_ADV_START_RETURN, BLE_HID_ADV_START_COMPLETE,
    BLE_HID_ADV_STOP_REQUEST, BLE_HID_ADV_STOP_RETURN, BLE_HID_ADV_STOP_COMPLETE,
    BLE_HID_ADV_CONNECT_ACCEPTED, BLE_HID_ADV_CONNECT_REJECTED, BLE_HID_ADV_DISCONNECT,
    BLE_HID_ADV_DATA_COMPLETE, BLE_HID_ADV_SCAN_COMPLETE
} ble_hid_advertising_event_t;
enum {
    BLE_HID_ADV_INITIALIZED = 1U << 0, BLE_HID_ADV_FAILED = 1U << 1,
    BLE_HID_ADV_CONNECTED = 1U << 2, BLE_HID_ADV_ACTIVE = 1U << 3,
    BLE_HID_ADV_START_PENDING = 1U << 4, BLE_HID_ADV_STOP_PENDING = 1U << 5,
    BLE_HID_ADV_REJECT_PENDING = 1U << 6, BLE_HID_ADV_NOTIFY_PENDING = 1U << 7,
    BLE_HID_ADV_SUBMITTING = 1U << 8, BLE_HID_ADV_BONDS_LOADED = 1U << 9,
    BLE_HID_ADV_PAIRING_OPEN = 1U << 10, BLE_HID_ADV_NAME_BLOCKED = 1U << 11,
    BLE_HID_ADV_TARGET_BLOCKED = 1U << 12, BLE_HID_ADV_TARGET_SELECTED = 1U << 13,
    BLE_HID_ADV_FORGETTING = 1U << 14, BLE_HID_ADV_STOPPED_BY_CONNECT = 1U << 15
};
typedef struct {
    bool known;
    uint64_t sampled_uptime_ms;
    size_t free_internal, largest_internal, free_dma, largest_dma;
} ble_hid_advertising_memory_t;
typedef struct {
    uint64_t sequence, command_sequence, uptime_ms, generation;
    ble_hid_advertising_event_t event;
    uint32_t flags, status;
    bool status_known;
    ble_hid_advertising_memory_t memory;
} ble_hid_advertising_observation_t;
typedef struct {
    bool frozen, overflowed, sequence_exhausted;
    uint64_t total_events;
    size_t count;
    ble_hid_advertising_observation_t events[BLE_HID_ADVERTISING_TRACE_CAPACITY];
} ble_hid_advertising_diagnostics_t;
/* Task/callback-only observations, no identities or report payloads. Requests
 * sample host heap before the API; callbacks carry their own later uptime.
 * Neither observation establishes which allocator/controller produced a status.
 * command_sequence identifies the latest reservation, not an SDK callback token.
 * The bounded chronological tail freezes on the first policy-classified
 * advertising fault; benign connect-stopped completions remain observations.
 * Overflow/missing status/missing memory are explicit. Copy performs no SDK I/O. */
void ble_hid_copy_advertising_diagnostics(ble_hid_advertising_diagnostics_t *out);
const char *ble_hid_advertising_event_name(ble_hid_advertising_event_t event);

typedef enum {
    BLE_HID_FORGET_IDLE,
    BLE_HID_FORGET_RUNNING,
    BLE_HID_FORGET_DONE,
    BLE_HID_FORGET_FAILED
} ble_hid_forget_state_t;

typedef struct {
    uint8_t address[6];
    /* Raw SDK type 0..3: public, random, RPA/public identity, RPA/random identity.
     * Ignore address_type when address_type_known is false. */
    uint8_t address_type;
    bool address_type_known;
    bool is_current;
    /* Exact saved-key admission is supported only for a stable public/static
     * key. Private/pseudo keys remain listed and preserved, but selection is
     * unavailable instead of guessing their on-air identity. */
    bool selection_supported;
    bool is_selected;
    bool bond_id_known;
    uint8_t bond_id[BLE_HID_BOND_ID_BYTES]; /* Fingerprint, never exported keys. */
} ble_hid_bond_t;

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    bool bond_id_known;
    uint8_t bond_id[BLE_HID_BOND_ID_BYTES];
} ble_hid_target_t;

typedef enum { BLE_HID_TARGET_IDLE, BLE_HID_TARGET_PENDING, BLE_HID_TARGET_APPLYING,
    BLE_HID_TARGET_APPLIED, BLE_HID_TARGET_FAILED, BLE_HID_TARGET_UNCERTAIN } ble_hid_target_status_t;

typedef struct {
    bool selected;
    bool available;             /* Selected key is fresh and selectable, or no preference. */
    esp_err_t error;            /* Unknown list, removed key and unsupported privacy differ. */
    ble_hid_target_t target;
    uint64_t revision;
    bool loaded;                /* Actual NVS read completed; absence is a known default. */
    bool durable;               /* A versioned record was read/committed and verified. */
    uint64_t request_id;
    ble_hid_target_status_t request_status;
    esp_err_t request_error;
    bool host_initiated_only;    /* Always true: peripheral cannot dial a host. */
    bool nearby_detection_supported; /* Always false; no host scan inference. */
} ble_hid_target_snapshot_t;

typedef struct {
    uint64_t expected_bond_revision;
    uint64_t expected_revision;
    bool selected;
    ble_hid_target_t target;
    uint8_t count;
    ble_hid_target_t order[BLE_HID_MAX_BONDS];
} ble_hid_target_preferences_request_t;

typedef enum { BLE_HID_BOND_FORGET_IDLE, BLE_HID_BOND_FORGET_PENDING,
    BLE_HID_BOND_FORGET_REMOVING, BLE_HID_BOND_FORGET_VERIFYING,
    BLE_HID_BOND_FORGET_SUCCEEDED, BLE_HID_BOND_FORGET_FAILED,
    BLE_HID_BOND_FORGET_CANCELLED, BLE_HID_BOND_FORGET_UNCERTAIN } ble_hid_bond_forget_status_t;
typedef struct {
    uint64_t request_id, generation, bond_revision;
    ble_hid_target_t target;
    ble_hid_bond_forget_status_t status;
    bool sdk_inflight, cancellable;
    esp_err_t error;
} ble_hid_bond_forget_snapshot_t;

typedef struct {
    bool known;
    bool updating;
    esp_err_t error;
    uint64_t revision;
    uint64_t preferences_revision; /* Same-lock active preference observation. */
    uint8_t count;
    /* Actual configured SDK limit, 1..MAX_BONDS. Admission stops at capacity
     * so adding a host cannot trigger the SDK's oldest-bond eviction policy. */
    uint8_t capacity;
    /* True only for a verified active bond with matching SDK address/type.
     * Otherwise every row has is_current=false; use link.connected separately
     * to distinguish no link from an unmapped/private or unauthenticated peer. */
    bool current_match_known;
    ble_hid_bond_t bonds[BLE_HID_MAX_BONDS];
} ble_hid_bond_list_t;

typedef struct {
    bool initialized;
    bool failed;
    bool connected;
    /* Both input CCCDs enabled on this encrypted, active connection, either by
     * actual writes or restoration of this verified bond's saved subscriptions.
     * A successful encryption event alone never enables subscriptions.
     * Congestion does not revoke readiness or cancel queued actions. */
    bool ready;
    /* Includes stack congestion and the one outstanding notification. Skip
     * hid_engine_step while true; a false send means rejection, not queueing. */
    bool congested;
    bool encrypted;
    bool suspended;
    uint8_t keyboard_leds;
    /* Current SDK-reported link address; it may be a private/pseudo address.
     * Known only while connected, zero otherwise. This is neither a host name
     * nor an OS/device-type claim, and must not be logged by default. */
    bool peer_known;
    uint8_t peer_address[6];
    bool advertising;
    bool pairing_open;
    uint32_t pairing_remaining_ms;
    bool bond_count_known;
    uint8_t bond_count;
    /* Actual configured SDK limit, also available before the first bond read.
     * A known count at this limit disables Add device without evicting a peer. */
    uint8_t bond_capacity;
    ble_hid_forget_state_t bond_forget_state;
    esp_err_t bond_forget_error;
    /* Actual asynchronous RSSI in dBm, not a percentage or inferred link
     * quality. Invalid on disconnect, read failure, sentinel 127 or age >5 s. */
    bool rssi_valid;
    bool rssi_pending;
    int8_t rssi_dbm;
    uint32_t rssi_age_ms;
    esp_err_t rssi_error;
    /* Changes on connection replacement/disconnect and readiness revocation,
     * including suspend/CCCD disable even if the coordinator misses the edge.
     * Never retain queued input across a generation change. */
    uint64_t generation;
} ble_hid_snapshot_t;

/* Call once from the application task. Owns the controller, Bluedroid and LE
 * GAP/GATTS callbacks; initialize before any other Bluetooth consumer. Classic
 * builds enable one BTDM controller; BLE-only builds enable BLE. ESP_OK means
 * asynchronous setup was accepted, not that advertising or pairing succeeded.
 * Poll failed for later setup failures. NVS errors are returned without erasing
 * bonds or other stored data. Requires LE Secure Connections Just Works bonding
 * for persistent subscriptions; no legacy-pairing or MITM/passkey UI claim.
 * The pinned public bond export provides SC's PENC but omits local-only legacy
 * LTKs. Existing legacy bonds are rejected without deleting them. No retry
 * after partial initialization. Bonded CCCD writes commit before ATT success;
 * storage failures latch failed without erasing data. Initialization/CCCD
 * updates use one worker and at most 32 saved bonds. Build with bt, nvs_flash,
 * mbedtls, esp_timer, hid_engine and device_settings. */
esp_err_t ble_hid_init(void);

/* Thread-safe value copy. Call from tasks, not interrupt context. */
ble_hid_snapshot_t ble_hid_snapshot(void);

/* Thread-safe, allocation-free first-failure copy. Retained until reboot;
 * disconnect, recovery, repeated initialization and later errors never clear
 * or overwrite it. Historical evidence does not imply current failed/ready.
 * Normal API rejection, explicit disconnect and optional RSSI do not consume
 * it. sdk_status is meaningful only when sdk_status_known is true. */
ble_hid_failure_t ble_hid_failure(void);
/* Convenience static label for logging; use failure().stage for the label
 * belonging to the same copied record. Returns "none" before a failure. */
const char *ble_hid_failure_stage_name(void);

/* Thread-safe cached copy; never reads NVS or invokes Bluetooth APIs. The
 * worker refreshes after initialization, authentication and bond management.
 * Authentication invalidates the durable view immediately; its refresh survives
 * disconnect and ignores reads overtaken by another authentication completion.
 * known=false is unavailable/stale evidence, not an empty list; retained rows
 * are the last completed read and must be labeled accordingly. updating means
 * bond management is active. revision tracks completed reads/status, while
 * current-match flags follow the current link and can change without a read.
 * SDK bond indices may be private/pseudo;
 * their display does not infer a name/OS or promise peripheral-initiated
 * connection to that host. No automatic logging of identities. */
/* Same-lock observation directly into caller-owned storage, without a large
 * return-value temporary. No SDK calls; NULL is ignored. */
void ble_hid_copy_bond_list(ble_hid_bond_list_t *out);
/* Compatibility wrapper; new stack-sensitive callers should use copy-out. */
ble_hid_bond_list_t ble_hid_bond_list(void);

/* Task-only reconnect-admission preference, with the exact known bond-list
 * revision. NULL clears selection. Requires disconnected, no pending HID
 * notification/rejection/forget/name operation and a healthy known list.
 * Does not disconnect a host, initiate a connection, erase a bond, authorize
 * pairing or claim proximity. ESP_OK is copied submission only; poll the
 * matching request ID/status for verified durable application. While selected, only that
 * exact SDK saved key/type is admitted; unsupported privacy/pseudo keys return
 * ESP_ERR_NOT_SUPPORTED. The host still initiates reconnection. The coordinator
 * must release input and disconnect first when switching a live host. */
esp_err_t ble_hid_target_select(uint64_t bond_revision, const ble_hid_target_t *target);
ble_hid_target_snapshot_t ble_hid_target_snapshot(void);

/* Complete permutation of the current verified saved keys, no duplicates or
 * omissions. Reorders display/reconnect preferences only; never rotates hosts,
 * evicts a bond or changes the one-link limit. Stable ordering survives cache
 * refresh; new keys append, removed keys disappear. The control worker writes
 * versioned NVS outside callbacks and verifies readback before APPLIED. No
 * SDK/NVS call, allocation or connection change occurs in the submission call.
 * Pending/uncertain persistence blocks admission; no automatic erase/retry. */
esp_err_t ble_hid_target_order(uint64_t bond_revision, const ble_hid_target_t *ordered, size_t count);
/* Atomic selection+order update. Both expected revisions must match. A key
 * without a fingerprint is bound to that fresh row before admission; a supplied
 * fingerprint must match. Duplicate/omitted keys and unsupported identities
 * reject before NVS. An unchanged removed selection may be retained blocked.
 * Rejected calls leave request/status/persistence unchanged. */
esp_err_t ble_hid_target_preferences_request(const ble_hid_target_preferences_request_t *, uint64_t *request_id);

/* Nonblocking pairing admission controls. Open accepts 1..MAX_MS, only while
 * disconnected, below the configured bond capacity with a known list, and no
 * forget operation/fault is active. Existing bonds are never implicitly erased
 * to make room for Add device. A successful first
 * boot bond-list query with zero bonds opens DEFAULT_MS once, with no automatic
 * reopening on expiry. Saved peers may reconnect outside the window. An
 * unknown peer must enter during it and gets at most 30 s to finish pairing;
 * the worker confirms fresh SDK capacity before granting security or initiating
 * pairing. A physical link awaiting that check has no pairing authorization.
 * closing the window stops new admission, not an already admitted handshake.
 * Use disconnect_generation to cancel that handshake. No host name is inferred. */
bool ble_hid_pairing_open(uint32_t duration_ms);
void ble_hid_pairing_close(void);
/* Shared Classic pairing owner only. Claim after closing the LE window; fails
 * while an LE handshake is admitted/pending. Prevents simultaneous writes into
 * the SDK's shared eviction domain. Retain through actual Classic auth result;
 * uncertain/missing auth completion retains the claim until stack reset. */
bool ble_hid_pairing_claim_classic(void);
void ble_hid_pairing_release_classic(void);

/* The coordinator owns explicit user confirmation, engine cancellation and
 * release-before-disconnect. This command rejects a connected link or pending
 * notification, so it cannot bypass that release/disconnect boundary. The
 * expected snapshot generation is rechecked atomically. Returns submission
 * acceptance; poll bond_forget_state/error for actual asynchronous completion.
 * The worker removes at most 32 bonds one at a time, clears only this adapter's
 * saved subscriptions and verifies zero bonds. Success opens DEFAULT_MS.
 * A failure retains truthful partial/unknown state and requires recovery;
 * no entire NVS partition is erased and no stale completion reopens admission. */
bool ble_hid_forget_bonds(uint64_t generation);
/* Same exact worker and completion proof, with no automatic pairing window at
 * any phase. Factory reset/remote forget-all must use this variant; opening
 * new trust remains a separate explicit local action. */
bool ble_hid_forget_bonds_without_pairing(uint64_t generation);

/* Exact single saved bond, never an alias of forget-all. Requires current HID
 * generation, fresh exact list revision and a known matching fingerprint.
 * Disconnected/released only, no pending reports, pairing, rename, preference
 * write or other bond operation. Admission closes while the worker stops
 * advertising, rereads the actual SDK identity, submits one address removal,
 * and verifies that only the named identity disappeared. No automatic retry,
 * NVS partition erase, pairing window or replacement-host selection. */
esp_err_t ble_hid_forget_bond_request(uint64_t generation, uint64_t bond_revision,
                                     const ble_hid_target_t *target, uint64_t *request_id);
/* Local pending cancellation only. ESP_ERR_NOT_SUPPORTED means already
 * submitted/too late; no SDK cancellation or rollback is invented. */
esp_err_t ble_hid_forget_bond_cancel(uint64_t request_id);
ble_hid_bond_forget_snapshot_t ble_hid_bond_forget_snapshot(void);

/* Task-only, single coordinator sender. Report ID selects the characteristic;
 * do not prefix it to the payload. Only keyboard ID 1/8 bytes and mouse ID 2/4
 * bytes are accepted. The gate is checked again for each attempt; the pinned
 * stack copies data before return. No adapter queue, waits for acknowledgements,
 * automatic replay or retries. true means SDK submission acceptance only; it
 * proves neither delivery nor an intended host UI action. The SDK call itself
 * may wait for its internal queue, so never call from a UI/Bluetooth callback.
 * An already submitted notification cannot be recalled after interruption.
 * Reconnect advertising waits for its SDK completion before conn_id reuse.
 * A missing completion after 1000 ms closes the link and latches failed until
 * reboot; reconnect advertising stays closed because safe reuse is unproven.
 * This relies on notification CONF_EVT behavior in pinned SDK 383dda6c1. */
bool ble_hid_send(uint8_t report_id, const uint8_t *data, size_t length);

/* Preferred coordinator API: also rejects an action prepared for an obsolete
 * snapshot. A generation is local session identity, not host acknowledgement. */
bool ble_hid_send_generation(uint64_t generation, uint8_t report_id,
                             const uint8_t *data, size_t length);

/* Nonblocking request for coordinator release/recovery exhaustion. Returns
 * false for an obsolete generation or absent link. Immediately revokes the
 * input generation; the control worker closes the connection. */
bool ble_hid_disconnect_generation(uint64_t generation);

#ifdef __cplusplus
}
#endif
