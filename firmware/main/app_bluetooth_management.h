// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ble_hid.h"

typedef enum { APP_BT_MANAGE_NONE, APP_BT_MANAGE_ADD, APP_BT_MANAGE_FORGET_ALL,
    APP_BT_MANAGE_FORGET_ONE, APP_BT_MANAGE_SELECT, APP_BT_MANAGE_RECONNECT } app_bluetooth_management_operation_t;
typedef enum { APP_BT_MANAGE_IDLE, APP_BT_MANAGE_RELEASE, APP_BT_MANAGE_DISCONNECT,
    APP_BT_MANAGE_DELETE, APP_BT_MANAGE_POLICY, APP_BT_MANAGE_ADVERTISING,
    APP_BT_MANAGE_COMPLETE, APP_BT_MANAGE_FAILED, APP_BT_MANAGE_CANCELLED } app_bluetooth_management_phase_t;
typedef struct {
    /* release_pending gates disconnect only while a Bluetooth host remains
     * connected; the next-host neutral barrier survives link-down. */
    bool locked, screen_off, release_pending, transport_switch_pending;
} app_bluetooth_management_context_t;
typedef struct {
    app_bluetooth_management_operation_t operation;
    app_bluetooth_management_phase_t phase;
    bool active, uncertain, irreversible;
    esp_err_t error;
    uint64_t revision, deletion_request_id, policy_request_id;
    char message[97];
} app_bluetooth_management_snapshot_t;

/* Sole coordinator only. Local confirmation/physical-origin checks belong to
 * the caller. These functions never read sensors, allocate or send HID. */
void app_bluetooth_management_init(void);
/* Bind the caller-approved admitted host generation, or zero when none.
 * Raw physical generations are used only for the eventual SDK commands. */
esp_err_t app_bluetooth_management_begin(app_bluetooth_management_operation_t,
    uint64_t host_generation, uint64_t bond_revision, uint64_t preference_revision,
    const ble_hid_target_t *exact_target, uint64_t now_ms);
void app_bluetooth_management_service(const app_bluetooth_management_context_t *, uint64_t now_ms);
/* Cancels future steps, never an SDK/NVS write already submitted. Receipts and
 * uncertainty survive cancellation; no automatic pairing or rollback follows. */
void app_bluetooth_management_cancel(void);
void app_bluetooth_management_snapshot(app_bluetooth_management_snapshot_t *);
