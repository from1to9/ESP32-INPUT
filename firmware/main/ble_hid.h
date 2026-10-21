// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HID_PAIRING_DEFAULT_MS 120000U
#define BLE_HID_PAIRING_MAX_MS 300000U
#define BLE_HID_MAX_BONDS 32U

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
} ble_hid_bond_t;

typedef struct {
    bool known;
    bool updating;
    esp_err_t error;
    uint64_t revision;
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

/* Call once from the application task. Owns the controller, Bluedroid and GAP/
 * GATTS callbacks; initialize before any other Bluetooth consumer. ESP_OK means
 * asynchronous setup was accepted, not that advertising or pairing succeeded.
 * Poll failed for later setup failures. NVS errors are returned without erasing
 * bonds or other stored data. Requires LE Secure Connections Just Works bonding
 * for persistent subscriptions; no legacy-pairing or MITM/passkey UI claim.
 * The pinned public bond export provides SC's PENC but omits local-only legacy
 * LTKs. Existing legacy bonds are rejected without deleting them. No retry
 * after partial initialization. Bonded CCCD writes commit before ATT success;
 * storage failures latch failed without erasing data. Initialization/CCCD
 * updates use one worker and at most 32 saved bonds. Build with bt, nvs_flash,
 * mbedtls and hid_engine. */
esp_err_t ble_hid_init(void);

/* Thread-safe value copy. Call from tasks, not interrupt context. */
ble_hid_snapshot_t ble_hid_snapshot(void);

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
ble_hid_bond_list_t ble_hid_bond_list(void);

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
