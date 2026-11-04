// SPDX-License-Identifier: Apache-2.0
#include "app_bluetooth_management.h"
#include <stdio.h>
#include <string.h>
#if ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define MANAGEMENT_TIMEOUT_MS UINT64_C(30000)
typedef struct {
    app_bluetooth_management_snapshot_t view;
    uint64_t started_ms, deadline_ms, host_generation, preference_revision, admission_reservation;
    bool disconnect_requested, selecting_current;
    ble_hid_target_t target;
    ble_hid_target_snapshot_t original_policy;
    ble_hid_bond_list_t original;
} management_t;
/* Coordinator-only copied workspaces, separate from the BLE worker's mutable
 * candidates. No borrowed pointer, per-poll allocation or task is introduced. */
static EXT_RAM_BSS_ATTR management_t management;
static EXT_RAM_BSS_ATTR ble_hid_bond_list_t observed;
static EXT_RAM_BSS_ATTR ble_hid_target_preferences_request_t policy_request;

static void message(app_bluetooth_management_phase_t phase, const char *text)
{
    management.view.phase = phase;
    if (management.view.revision != UINT64_MAX) ++management.view.revision;
    (void)snprintf(management.view.message, sizeof(management.view.message), "%s", text);
}
static void finish(esp_err_t error, bool uncertain, const char *text)
{
    if (error != ESP_OK && management.view.operation == APP_BT_MANAGE_ADD) ble_hid_pairing_close();
    if (error == ESP_OK) ble_hid_management_resume(management.admission_reservation, NULL);
    else ble_hid_management_release(management.admission_reservation);
    management.view.active = false;
    management.view.error = error;
    management.view.uncertain |= uncertain;
    message(error == ESP_OK ? APP_BT_MANAGE_COMPLETE : APP_BT_MANAGE_FAILED, text);
}
static bool same_target(const ble_hid_target_t *a, const ble_hid_target_t *b)
{
    return a->address_type == b->address_type && a->bond_id_known == b->bond_id_known &&
        !memcmp(a->address, b->address, 6) &&
        (!a->bond_id_known || !memcmp(a->bond_id, b->bond_id, BLE_HID_BOND_ID_BYTES));
}
static ble_hid_target_t key(const ble_hid_bond_t *bond)
{
    ble_hid_target_t result = {.address_type = bond->address_type, .bond_id_known = bond->bond_id_known};
    memcpy(result.address, bond->address, sizeof(result.address));
    memcpy(result.bond_id, bond->bond_id, sizeof(result.bond_id));
    return result;
}
static bool target_matches(const ble_hid_target_t *target, const ble_hid_bond_t *bond)
{
    return target->bond_id_known && bond->bond_id_known && bond->address_type_known &&
        target->address_type == bond->address_type && !memcmp(target->address, bond->address, 6) &&
        !memcmp(target->bond_id, bond->bond_id, BLE_HID_BOND_ID_BYTES);
}
static bool target_is_current(void)
{
    if (!observed.current_match_known) return false;
    for (unsigned i = 0; i < observed.count; ++i)
        if (observed.bonds[i].is_current && target_matches(&management.target, &observed.bonds[i])) return true;
    return false;
}
static bool same_bond(const ble_hid_bond_t *a, const ble_hid_bond_t *b)
{
    return a->address_type_known == b->address_type_known && a->address_type == b->address_type &&
        a->bond_id_known == b->bond_id_known && !memcmp(a->address, b->address, 6) &&
        (!a->bond_id_known || !memcmp(a->bond_id, b->bond_id, BLE_HID_BOND_ID_BYTES));
}
static bool manifest_matches(bool removed)
{
    if (!observed.known || observed.updating || observed.count > BLE_HID_MAX_BONDS) return false;
    unsigned expected = management.original.count;
    if (removed) { if (!expected) return false; --expected; }
    if (observed.count != expected) return false;
    bool used[BLE_HID_MAX_BONDS] = {false};
    for (unsigned i = 0; i < management.original.count; ++i) {
        const ble_hid_bond_t *before = &management.original.bonds[i];
        if (removed && target_matches(&management.target, before)) continue;
        bool found = false;
        for (unsigned j = 0; j < observed.count; ++j)
            if (!used[j] && same_bond(before, &observed.bonds[j])) { used[j] = true; found = true; break; }
        if (!found) return false;
    }
    return true;
}
static bool policy_stable(const ble_hid_target_snapshot_t *policy)
{
    return policy->loaded && policy->request_status != BLE_HID_TARGET_PENDING &&
        policy->request_status != BLE_HID_TARGET_APPLYING && policy->request_status != BLE_HID_TARGET_UNCERTAIN;
}
void app_bluetooth_management_init(void) { memset(&management, 0, sizeof(management)); }
esp_err_t app_bluetooth_management_begin(app_bluetooth_management_operation_t operation,
    uint64_t host_generation, uint64_t bond_revision, uint64_t preference_revision,
    const ble_hid_target_t *target, uint64_t now)
{
    if (operation < APP_BT_MANAGE_ADD || operation > APP_BT_MANAGE_RECONNECT ||
        now > UINT64_MAX - MANAGEMENT_TIMEOUT_MS) return ESP_ERR_INVALID_ARG;
    if (management.view.active) return ESP_ERR_INVALID_STATE;
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    ble_hid_target_snapshot_t policy = ble_hid_target_snapshot();
    ble_hid_copy_bond_list(&observed);
    if (!bluetooth.initialized || bluetooth.failed ||
        (bluetooth.connection_admitted ? bluetooth.generation : 0) != host_generation ||
        !observed.known || observed.updating || observed.count > BLE_HID_MAX_BONDS ||
        observed.revision != bond_revision || !policy_stable(&policy) || policy.revision != preference_revision)
        return ESP_ERR_INVALID_STATE;
    if (operation == APP_BT_MANAGE_ADD && (!observed.capacity || observed.count >= observed.capacity)) return ESP_ERR_NO_MEM;
    if (operation == APP_BT_MANAGE_FORGET_ONE || operation == APP_BT_MANAGE_SELECT || operation == APP_BT_MANAGE_RECONNECT) {
        if (!target || !target->bond_id_known) return ESP_ERR_NOT_SUPPORTED;
        unsigned matches = 0;
        for (unsigned i = 0; i < observed.count; ++i) if (target_matches(target, &observed.bonds[i]) &&
                (operation != APP_BT_MANAGE_SELECT || observed.bonds[i].selection_supported)) ++matches;
        if (matches != 1) return ESP_ERR_NOT_FOUND;
    }
    uint64_t reservation = 0;
    bool reserved = operation == APP_BT_MANAGE_SELECT ?
        ble_hid_management_select_reserve(host_generation, bond_revision, preference_revision, target, &reservation) :
        ble_hid_management_reserve(host_generation, bond_revision, preference_revision, &reservation);
    if (!reserved) return ESP_ERR_INVALID_STATE;
    uint64_t revision = management.view.revision;
    if (revision == UINT64_MAX) { ble_hid_management_release(reservation); return ESP_ERR_INVALID_STATE; }
    memset(&management, 0, sizeof(management));
    management.view.revision = revision;
    management.view.operation = operation; management.view.active = true;
    management.admission_reservation = reservation;
    management.host_generation = host_generation;
    management.preference_revision = preference_revision;
    management.started_ms = now; management.deadline_ms = now + MANAGEMENT_TIMEOUT_MS;
    management.original = observed; management.original_policy = policy;
    if (target) management.target = *target;
    ble_hid_pairing_close();
    message(APP_BT_MANAGE_RELEASE, "Releasing input before changing Bluetooth.");
    return ESP_OK;
}
void app_bluetooth_management_cancel(void)
{
    if (!management.view.active) return;
    ble_hid_pairing_close();
    ble_hid_management_release(management.admission_reservation);
    if (management.view.deletion_request_id) (void)ble_hid_forget_bond_cancel(management.view.deletion_request_id);
    management.view.active = false;
    management.view.uncertain |= management.view.irreversible;
    message(APP_BT_MANAGE_CANCELLED, management.view.irreversible ?
        "Stopped. An accepted change may finish; inspect saved hosts before retrying." : "Bluetooth change cancelled.");
}
static esp_err_t submit_policy(const ble_hid_target_snapshot_t *current, bool keep_current)
{
    memset(&policy_request, 0, sizeof(policy_request));
    policy_request.expected_bond_revision = observed.revision;
    policy_request.expected_revision = current->revision;
    policy_request.count = observed.count;
    policy_request.selected = management.view.operation == APP_BT_MANAGE_SELECT ||
        (management.view.operation == APP_BT_MANAGE_FORGET_ONE &&
        management.original_policy.selected && !same_target(&management.original_policy.target, &management.target));
    if (policy_request.selected) policy_request.target = management.view.operation == APP_BT_MANAGE_SELECT ?
        management.target : management.original_policy.target;
    for (unsigned i = 0; i < observed.count; ++i) policy_request.order[i] = key(&observed.bonds[i]);
    esp_err_t error = keep_current ? ble_hid_management_select_current(management.admission_reservation,
        management.host_generation, &policy_request, &management.view.policy_request_id) :
        ble_hid_target_preferences_request(&policy_request, &management.view.policy_request_id);
    memset(&policy_request, 0, sizeof(policy_request));
    return error;
}
void app_bluetooth_management_service(const app_bluetooth_management_context_t *context, uint64_t now)
{
    if (!context || !management.view.active) return;
    if (context->locked || context->screen_off || context->transport_switch_pending) { app_bluetooth_management_cancel(); return; }
    if (now < management.started_ms || now >= management.deadline_ms) {
        finish(ESP_ERR_TIMEOUT, management.view.irreversible, "Bluetooth change timed out. Inspect current state before retrying."); return;
    }
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    ble_hid_target_snapshot_t policy = ble_hid_target_snapshot();
    ble_hid_copy_bond_list(&observed);
    if (!bluetooth.initialized || bluetooth.failed || policy.request_status == BLE_HID_TARGET_UNCERTAIN) {
        finish(ESP_ERR_INVALID_STATE, management.view.irreversible, "Bluetooth is unavailable or its saved policy is uncertain."); return;
    }
    if (management.view.phase == APP_BT_MANAGE_RELEASE || management.view.phase == APP_BT_MANAGE_DISCONNECT) {
        if (!management.disconnect_requested && management.host_generation !=
            (bluetooth.connection_admitted ? bluetooth.generation : 0)) {
            finish(ESP_ERR_INVALID_STATE, false, "Bluetooth host changed. Review the new connection."); return;
        }
        if (bluetooth.connected) {
            if (management.view.operation == APP_BT_MANAGE_SELECT && bluetooth.connection_admitted && target_is_current()) {
                if (context->release_pending || bluetooth.congested) return;
                if (!manifest_matches(false) || !policy_stable(&policy) || policy.revision != management.preference_revision) {
                    finish(ESP_ERR_INVALID_STATE, false, "Saved hosts or target policy changed. Review before retrying."); return;
                }
                if (policy.durable && policy.selected && same_target(&policy.target, &management.target)) {
                    finish(ESP_OK, false, "Selected host preference is already saved."); return;
                }
                esp_err_t error = submit_policy(&policy, true);
                if (error != ESP_OK) {
                    finish(error, false, "Current host selection could not be saved."); return;
                }
                management.selecting_current = true; management.view.irreversible = true;
                message(APP_BT_MANAGE_POLICY, "Saving the current host as the selected target.");
                return;
            }
            if (!management.disconnect_requested && (!bluetooth.connection_admitted ||
                (!context->release_pending && !bluetooth.congested))) {
                const uint64_t generation = bluetooth.generation;
                if (!(management.view.operation == APP_BT_MANAGE_RECONNECT && bluetooth.connection_admitted ?
                    ble_hid_disconnect_user_generation(generation) : ble_hid_disconnect_generation(generation))) {
                    bluetooth = ble_hid_snapshot();
                    /* A refused stale request did not disconnect anything.
                     * Reconcile a departed provisional ACL on the next poll;
                     * never retry against a replacement in this iteration. */
                    if (!management.host_generation && bluetooth.initialized && !bluetooth.failed &&
                        !bluetooth.connection_admitted && !bluetooth.connected && bluetooth.generation != generation) return;
                    finish(ESP_ERR_INVALID_STATE, false, "The current host could not be disconnected."); return;
                }
                management.disconnect_requested = true;
                message(APP_BT_MANAGE_DISCONNECT, "Waiting for the current Bluetooth host to disconnect.");
            }
            return;
        }
        /* Link-down ends the old host's release obligation. The HID engine
         * deliberately retains a neutral-report barrier for the next host;
         * waiting for that disconnected barrier would prevent pairing it. */
        if (bluetooth.congested || !ble_hid_management_ready(management.admission_reservation)) return;
        if (!manifest_matches(false) || !policy_stable(&policy) || policy.revision != management.preference_revision) {
            finish(ESP_ERR_INVALID_STATE, false, "Saved hosts or target policy changed. Review before retrying."); return;
        }
        if (management.view.operation == APP_BT_MANAGE_RECONNECT && !policy.selected) {
            ble_hid_management_reconnect(management.admission_reservation, &management.target);
            message(APP_BT_MANAGE_ADVERTISING, "Reconnect allowed for 30 seconds. Exact pre-connection selection unavailable; connect from that host.");
            return;
        }
        if (management.view.operation == APP_BT_MANAGE_ADD || management.view.operation == APP_BT_MANAGE_SELECT ||
            management.view.operation == APP_BT_MANAGE_RECONNECT) {
            esp_err_t error = submit_policy(&policy, false);
            if (error != ESP_OK) { finish(error, false, "Target policy could not be cleared; pairing was not opened."); return; }
            management.view.irreversible = true;
            message(APP_BT_MANAGE_POLICY, management.view.operation == APP_BT_MANAGE_SELECT ?
                "Saving the selected host; the host must initiate connection." : "Preparing this device for new pairing.");
        } else {
            esp_err_t error = ESP_ERR_INVALID_STATE;
            if (management.view.operation == APP_BT_MANAGE_FORGET_ALL) {
                if (ble_hid_forget_bonds_without_pairing(bluetooth.generation)) error = ESP_OK;
            } else error = ble_hid_forget_bond_request(bluetooth.generation, observed.revision,
                &management.target, &management.view.deletion_request_id);
            if (error != ESP_OK) { finish(error, false, "Forget request rejected. Saved hosts were not confirmed removed."); return; }
            management.view.irreversible = true;
            message(APP_BT_MANAGE_DELETE, "Waiting for verified removal of the requested saved host data.");
        }
        return;
    }
    if (bluetooth.connected && !(management.selecting_current && management.view.phase == APP_BT_MANAGE_POLICY)) {
        if (management.view.phase == APP_BT_MANAGE_ADVERTISING && (management.view.operation == APP_BT_MANAGE_ADD || management.view.operation == APP_BT_MANAGE_SELECT || management.view.operation == APP_BT_MANAGE_RECONNECT)) {
            if (bluetooth.ready) finish(ESP_OK, false, "A Bluetooth host connected and input readiness was verified.");
            else if (strcmp(management.view.message, "A Bluetooth host connected; waiting for secure input readiness."))
                message(APP_BT_MANAGE_ADVERTISING, "A Bluetooth host connected; waiting for secure input readiness.");
        } else finish(ESP_ERR_INVALID_STATE, management.view.irreversible, "A Bluetooth host connected during the change. Inspect current state.");
        return;
    }
    if (management.view.phase == APP_BT_MANAGE_DELETE) {
        if (management.view.operation == APP_BT_MANAGE_FORGET_ALL) {
            if (bluetooth.bond_forget_state == BLE_HID_FORGET_FAILED) {
                finish(bluetooth.bond_forget_error != ESP_OK ? bluetooth.bond_forget_error : ESP_ERR_INVALID_STATE,
                    true, "Forget all failed or partly applied. Inspect saved hosts."); return;
            }
            if (bluetooth.bond_forget_state != BLE_HID_FORGET_DONE) return;
            if (!observed.known || observed.updating || observed.count) {
                finish(ESP_ERR_INVALID_STATE, true, "An empty saved-host list was not verified."); return;
            }
        } else {
            ble_hid_bond_forget_snapshot_t deletion = ble_hid_bond_forget_snapshot();
            if (deletion.request_id != management.view.deletion_request_id ||
                deletion.status == BLE_HID_BOND_FORGET_FAILED || deletion.status == BLE_HID_BOND_FORGET_UNCERTAIN ||
                deletion.status == BLE_HID_BOND_FORGET_CANCELLED) {
                finish(ESP_ERR_INVALID_STATE, true, "Exact host removal failed, changed or is uncertain."); return;
            }
            if (deletion.status != BLE_HID_BOND_FORGET_SUCCEEDED) return;
            if (!manifest_matches(true)) { finish(ESP_ERR_INVALID_STATE, true, "Removal did not preserve the other saved hosts."); return; }
        }
        if (!policy_stable(&policy) || policy.revision != management.preference_revision) {
            finish(ESP_ERR_INVALID_STATE, true, "Removal finished but target policy needs reconciliation."); return;
        }
        esp_err_t error = submit_policy(&policy, false);
        if (error != ESP_OK) { finish(error, true, "Removal finished; saving the remaining target policy failed."); return; }
        message(APP_BT_MANAGE_POLICY, "Verifying the remaining saved-host target policy.");
        return;
    }
    if (management.view.phase == APP_BT_MANAGE_POLICY) {
        if (policy.request_id != management.view.policy_request_id ||
            policy.request_status == BLE_HID_TARGET_FAILED || policy.request_status == BLE_HID_TARGET_UNCERTAIN) {
            finish(policy.request_error != ESP_OK ? policy.request_error : ESP_ERR_INVALID_STATE, true,
                "Target policy was not verified. Pairing remains closed."); return;
        }
        if (policy.request_status != BLE_HID_TARGET_APPLIED) return;
        bool selecting = management.view.operation == APP_BT_MANAGE_SELECT;
        bool expected_selected = selecting || (management.view.operation == APP_BT_MANAGE_FORGET_ONE &&
            management.original_policy.selected && !same_target(&management.original_policy.target, &management.target));
        if (!policy.durable || policy.revision <= management.preference_revision || policy.selected != expected_selected ||
            (expected_selected && !same_target(&policy.target, selecting ? &management.target : &management.original_policy.target))) {
            finish(ESP_ERR_INVALID_STATE, true, "Saved target policy did not match the approved change."); return;
        }
        if (selecting || management.view.operation == APP_BT_MANAGE_RECONNECT) {
            if (!manifest_matches(false)) { finish(ESP_ERR_INVALID_STATE, true, "Saved hosts changed during selection."); return; }
            if (management.selecting_current && bluetooth.connected) {
                if (bluetooth.generation != management.host_generation || !target_is_current()) {
                    finish(ESP_ERR_INVALID_STATE, true, "Current host changed while its selection was saved."); return;
                }
                finish(ESP_OK, false, "Selected host preference saved."); return;
            }
            if (selecting) ble_hid_management_resume(management.admission_reservation, &management.target);
            else ble_hid_management_reconnect(management.admission_reservation, &management.target);
            message(APP_BT_MANAGE_ADVERTISING, selecting ? "Selected host saved. Waiting for advertising; connection is not ready." :
                "Reconnect allowed for 30 seconds. Exact pre-connection selection unavailable; connect from that host.");
            return;
        }
        if (management.view.operation != APP_BT_MANAGE_ADD) {
            if ((management.view.operation == APP_BT_MANAGE_FORGET_ALL && (!observed.known || observed.updating || observed.count)) ||
                (management.view.operation == APP_BT_MANAGE_FORGET_ONE && !manifest_matches(true))) {
                finish(ESP_ERR_INVALID_STATE, true, "Saved hosts changed before final verification."); return;
            }
            finish(ESP_OK, false, management.view.operation == APP_BT_MANAGE_FORGET_ALL ?
                "All saved input hosts removed. Use Add host to pair." :
                "This saved host was removed. Other saved hosts remain."); return;
        }
        if (!manifest_matches(false)) { finish(ESP_ERR_INVALID_STATE, false, "Saved hosts changed before pairing."); return; }
        if (!ble_hid_management_pairing_open(management.admission_reservation, BLE_HID_PAIRING_DEFAULT_MS)) {
            if (!ble_hid_management_ready(management.admission_reservation)) return; /* Drain an overtaking rejected callback. */
            finish(ESP_ERR_INVALID_STATE, false, "Pairing could not open. Inspect Bluetooth readiness and saved hosts."); return;
        }
        message(APP_BT_MANAGE_ADVERTISING, "Waiting for Bluetooth to become visible.");
        return;
    }
    if (management.view.phase == APP_BT_MANAGE_ADVERTISING) {
        if (management.view.operation == APP_BT_MANAGE_RECONNECT) {
            if (bluetooth.advertising) finish(ESP_OK, false, "Reconnect allowed for 30 seconds; exact selection unavailable. Waiting for verified authentication.");
            return;
        }
        if (management.view.operation == APP_BT_MANAGE_SELECT) {
            if (bluetooth.advertising) finish(ESP_OK, false, "Selected host saved. Connect from that host; input is not ready yet.");
            return;
        }
        if (!bluetooth.pairing_open) { finish(ESP_ERR_INVALID_STATE, false, "The pairing window closed before advertising was verified."); return; }
        if (bluetooth.advertising) finish(ESP_OK, false, "Pairing is open and advertising. Choose the device on your host.");
    }
}
void app_bluetooth_management_snapshot(app_bluetooth_management_snapshot_t *out)
{
    if (out) *out = management.view;
}
