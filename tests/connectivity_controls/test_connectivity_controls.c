// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_connectivity_controls.c"
#include "review_fixture.h"

static app_wifi_snapshot_t wifi;
static app_wifi_profiles_snapshot_t profiles;
static ble_hid_snapshot_t bluetooth;
static ble_hid_bond_list_t bonds;
static ble_hid_target_snapshot_t preference;
static app_control_runtime_snapshot_t api;
static app_control_trust_review_t review;
static app_connectivity_controls_context_t context;
static app_view_t view;
static unsigned joins, reorders, forgets, target_requests, opens, closes, approvals;
static esp_err_t submit_error;
static char submitted_id[33], submitted_order[8][33], approved_digest[65];
static uint64_t submitted_revision;
static ble_hid_target_preferences_request_t submitted_target;
static ble_hid_bond_forget_snapshot_t single_forget;
static unsigned pairing_opens, pairing_closes, disconnects, all_forgets, single_forgets;
bool ble_hid_management_reserve(uint64_t generation, uint64_t bond_revision,
    uint64_t preference_revision, uint64_t *reservation)
{
    assert(generation == (bluetooth.connection_admitted ? bluetooth.generation : 0));
    assert(bond_revision == bonds.revision && preference_revision == preference.revision);
    *reservation = 1; return true;
}
void ble_hid_management_release(uint64_t reservation) { (void)reservation; }
void ble_hid_management_resume(uint64_t reservation, const ble_hid_target_t *target) { (void)reservation; (void)target; }
void ble_hid_management_reconnect(uint64_t reservation, const ble_hid_target_t *target) { (void)reservation; (void)target; }
bool ble_hid_management_ready(uint64_t reservation) { return reservation && !bluetooth.connected && !bluetooth.advertising; }
bool ble_hid_management_pairing_open(uint64_t reservation, uint32_t milliseconds)
{ assert(reservation); return ble_hid_pairing_open(milliseconds); }
bool ble_hid_pairing_open(uint32_t milliseconds)
{ assert(milliseconds == BLE_HID_PAIRING_DEFAULT_MS); ++pairing_opens; return false; }
void ble_hid_pairing_close(void) { ++pairing_closes; bluetooth.pairing_open = false; }
bool ble_hid_disconnect_user_generation(uint64_t generation) { return ble_hid_disconnect_generation(generation); }
bool ble_hid_disconnect_generation(uint64_t generation)
{ assert(generation == bluetooth.generation); ++disconnects; return false; }
bool ble_hid_forget_bonds_without_pairing(uint64_t generation)
{ assert(generation == bluetooth.generation); ++all_forgets; return false; }
esp_err_t ble_hid_forget_bond_request(uint64_t generation, uint64_t revision, const ble_hid_target_t *target, uint64_t *id)
{ (void)generation; (void)revision; (void)target; (void)id; ++single_forgets; return ESP_ERR_INVALID_STATE; }
esp_err_t ble_hid_forget_bond_cancel(uint64_t id) { (void)id; return ESP_ERR_NOT_SUPPORTED; }
ble_hid_bond_forget_snapshot_t ble_hid_bond_forget_snapshot(void) { return single_forget; }
void app_wifi_get_snapshot(app_wifi_snapshot_t *out) { *out = wifi; }
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out) { *out = profiles; }
ble_hid_snapshot_t ble_hid_snapshot(void) { return bluetooth; }
void ble_hid_copy_bond_list(ble_hid_bond_list_t *out) { assert(out); *out = bonds; }
ble_hid_target_snapshot_t ble_hid_target_snapshot(void) { return preference; }
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *out) { *out = api; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out) { *out = api.trust; }
bool app_control_runtime_commissioning_review(app_control_trust_review_t *out) { *out = review; return api.trust.review_pending; }
esp_err_t app_wifi_profile_join(const char *id, uint64_t revision)
{ ++joins; memcpy(submitted_id, id, 33); submitted_revision = revision; return submit_error; }
esp_err_t app_wifi_profile_forget(const char *id, uint64_t revision)
{ ++forgets; memcpy(submitted_id, id, 33); submitted_revision = revision; return submit_error; }
esp_err_t app_wifi_profiles_reorder(const char ids[8][33], uint8_t count, uint64_t revision)
{ assert(count == profiles.count); ++reorders; memcpy(submitted_order, ids, sizeof(submitted_order)); submitted_revision = revision; return submit_error; }
esp_err_t ble_hid_target_preferences_request(const ble_hid_target_preferences_request_t *request, uint64_t *id)
{ ++target_requests; submitted_target = *request; if (!submit_error) *id = preference.request_id + 1U; return submit_error; }
esp_err_t app_control_runtime_open_commissioning(void) { ++opens; return submit_error; }
void app_control_runtime_close_commissioning(void) { ++closes; }
esp_err_t app_control_runtime_approve_commissioning(const char digest[65])
{ ++approvals; memcpy(approved_digest, digest, 65); return submit_error; }

static void poll_at(uint64_t now)
{ app_connectivity_controls_effect_t effect; app_connectivity_controls_context(&context); app_connectivity_controls_poll(now, &effect); app_connectivity_controls_fill(&view); }
static void setup(void)
{
    wifi = (app_wifi_snapshot_t){.initialized = true, .revision = 7, .state = APP_WIFI_IDLE};
    profiles = (app_wifi_profiles_snapshot_t){.known = true, .revision = 11, .count = 8, .capacity = 8};
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(profiles.profiles[i].id, 33, "%032x", i + 1U);
        snprintf(profiles.profiles[i].ssid, 33, "Network %u", i + 1U);
        profiles.profiles[i].enabled = true; profiles.profiles[i].priority = (uint8_t)i;
    }
    bluetooth = (ble_hid_snapshot_t){.initialized = true, .generation = 31};
    bonds = (ble_hid_bond_list_t){.known = true, .revision = 13, .count = 2, .capacity = 15};
    for (unsigned i = 0; i < 2; ++i) {
        bonds.bonds[i] = (ble_hid_bond_t){.address = {(uint8_t)(0xc2U + i)}, .address_type = 1,
            .address_type_known = true, .bond_id_known = true, .selection_supported = true};
        memset(bonds.bonds[i].bond_id, (int)(i + 1U), BLE_HID_BOND_ID_BYTES);
    }
    preference = (ble_hid_target_snapshot_t){.loaded = true, .durable = true, .revision = 17};
    api = (app_control_runtime_snapshot_t){.initialized = true, .trust = {.initialized = true, .known = true, .revision = 19}};
    context = (app_connectivity_controls_context_t){.epoch = 23, .ui_revision = 29, .mode = INPUT_MOUSE, .overlay = APP_OVERLAY_WIFI_PROFILES};
    assert(app_connectivity_controls_init()); assert(!app_connectivity_controls_init()); poll_at(1000);
}
static app_intent_t wifi_intent(app_saved_wifi_operation_t operation, unsigned row)
{
    app_intent_t intent = {.action = APP_WIFI_PROFILE_CONTROL, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .wifi_profile = {.operation = operation, .revision = profiles.revision}};
    if (row < profiles.count) memcpy(intent.wifi_profile.id, profiles.profiles[row].id, 33);
    return intent;
}
static app_intent_t api_intent(app_api_operation_t operation)
{
    app_intent_t intent = {.action = APP_API_COMMISSIONING, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .api = {.operation = operation, .revision = view.api.revision, .page = view.api.review_page}};
    memcpy(intent.api.review_digest, view.api.review_digest, 65); return intent;
}
static app_connectivity_controls_effect_t handle(app_intent_t intent)
{
    app_connectivity_controls_effect_t effect; assert(app_connectivity_controls_handle(&intent, &effect)); assert(effect.handled);
    const unsigned char *bytes = (const unsigned char *)&bluetooth_request;
    for (size_t i = 0; i < sizeof(bluetooth_request); ++i) assert(bytes[i] == 0);
    return effect;
}
static void apply_overlay(app_connectivity_controls_effect_t effect)
{
    assert(effect.open_overlay); context.overlay = effect.overlay; context.confirmation = effect.confirmation;
    ++context.epoch; ++context.ui_revision; app_connectivity_controls_context(&context); app_connectivity_controls_fill(&view);
}
static void routing(void)
{
    setup(); app_connectivity_controls_effect_t effect;
    app_intent_t other = {.action = APP_WIFI_SCAN}; assert(!app_connectivity_controls_handle(&other, &effect));
    other.action = APP_PAIRING_REQUEST; assert(!app_connectivity_controls_handle(&other, &effect));
    other.action = APP_MEDIA_CONTROL; assert(!app_connectivity_controls_handle(&other, &effect));
    other.action = APP_API_INPUT_ALLOW_SET; assert(!app_connectivity_controls_handle(&other, &effect));
    other.action = APP_API_COMMISSIONING; other.api.operation = (app_api_operation_t)99;
    assert(!app_connectivity_controls_handle(&other, &effect));
    app_intent_t stale = wifi_intent(APP_SAVED_WIFI_JOIN, 0); --stale.epoch; handle(stale); assert(!joins);
    stale = wifi_intent(APP_SAVED_WIFI_JOIN, 0); --stale.wifi_profile.revision; handle(stale); assert(!joins);
    context.locked = true; poll_at(1010); handle(wifi_intent(APP_SAVED_WIFI_JOIN, 0)); assert(!joins && !view.saved_wifi.rows[0].can_join);
    context.locked = false; context.overlay = APP_OVERLAY_SETTINGS; poll_at(1020); handle(wifi_intent(APP_SAVED_WIFI_JOIN, 0)); assert(!joins);
}
static void wifi_order(void)
{
    setup(); assert(view.saved_wifi.known && view.saved_wifi.count == 8);
    handle(wifi_intent(APP_SAVED_WIFI_PREFER, 7)); assert(reorders == 1 && submitted_revision == 11 && controls.pending == P_WIFI_PREFER);
    assert(!memcmp(submitted_order[0], profiles.profiles[7].id, 33));
    for (unsigned i = 1; i < 8; ++i) assert(!memcmp(submitted_order[i], profiles.profiles[i - 1U].id, 33));
    poll_at(1010); assert(controls.pending == P_WIFI_PREFER && reorders == 1);
    app_wifi_profile_view_t last = profiles.profiles[7]; memmove(profiles.profiles + 1, profiles.profiles, 7 * sizeof(last)); profiles.profiles[0] = last;
    ++profiles.revision; ++wifi.revision; poll_at(1020); assert(controls.pending == P_NONE && strstr(controls.message, "verified"));
    profiles.known = false; profiles.uncertain = true; poll_at(1030); assert(!view.saved_wifi.known && view.saved_wifi.uncertain && !view.saved_wifi.count);
    handle(wifi_intent(APP_SAVED_WIFI_PREFER, 1)); assert(reorders == 1);
}
static void wifi_join(void)
{
    setup(); handle(wifi_intent(APP_SAVED_WIFI_JOIN, 1)); assert(joins == 1 && controls.pending == P_WIFI_JOIN);
    strcpy(wifi.ssid, profiles.profiles[1].ssid); wifi.state = APP_WIFI_OBTAINING_IP; ++wifi.revision; poll_at(1010);
    assert(controls.pending == P_WIFI_JOIN);
    wifi.state = APP_WIFI_CONNECTED; poll_at(1020); assert(controls.pending == P_WIFI_JOIN);
    wifi.lan_ready = true; poll_at(1030); assert(controls.pending == P_NONE && strstr(controls.message, "Connected"));
    submit_error = ESP_FAIL; handle(wifi_intent(APP_SAVED_WIFI_JOIN, 2)); assert(joins == 2 && controls.pending == P_NONE);
    poll_at(1040); assert(joins == 2);
}
static void wifi_forget(void)
{
    setup(); app_connectivity_controls_effect_t effect = handle(wifi_intent(APP_SAVED_WIFI_FORGET_REQUEST, 3));
    assert(!forgets && effect.confirmation == APP_CONFIRM_WIFI_FORGET); apply_overlay(effect);
    ++profiles.revision; poll_at(1010);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1};
    effect = handle(confirm); assert(!forgets); apply_overlay(effect);
    effect = handle(wifi_intent(APP_SAVED_WIFI_FORGET_REQUEST, 4)); apply_overlay(effect);
    confirm = (app_intent_t){.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1};
    effect = handle(confirm); assert(forgets == 1 && !memcmp(submitted_id, profiles.profiles[4].id, 33)); apply_overlay(effect);
    memmove(profiles.profiles + 4, profiles.profiles + 5, 3 * sizeof(profiles.profiles[0])); --profiles.count; ++profiles.revision; ++wifi.revision; poll_at(1020);
    assert(controls.pending == P_NONE && strstr(controls.message, "forgotten"));
    assert(!app_connectivity_controls_handle(&confirm, &effect)); /* Legacy confirmation is not stolen. */
}
static app_intent_t bluetooth_intent(app_bluetooth_target_operation_t operation, unsigned row)
{
    return (app_intent_t){.action = APP_BLUETOOTH_TARGET_CONTROL, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .bluetooth_target = {.operation = operation, .bond_revision = bonds.revision, .preferences_revision = preference.revision, .target = ui_key(&bonds.bonds[row])}};
}
static void bluetooth_targets(void)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 1)); apply_overlay(effect);
    assert(view.bluetooth_target.known && view.bluetooth_target.can_select && !view.bluetooth_hosts[0].name[0]);
    app_connectivity_controls_effect_t selected = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 1));
    assert(selected.release_input && controls.pending == P_BLE_MANAGEMENT && !target_requests);
    poll_at(1020);
    assert(target_requests == 1 && submitted_target.selected && submitted_target.count == 2 && submitted_target.target.address[0] == 0xc3);
    assert(submitted_target.expected_bond_revision == 13 && submitted_target.expected_revision == 17);
    app_bluetooth_management_snapshot_t manager; app_bluetooth_management_snapshot(&manager);
    preference.request_id = manager.policy_request_id; preference.request_status = BLE_HID_TARGET_PENDING;
    poll_at(1030); assert(controls.pending == P_BLE_MANAGEMENT);
    preference.request_status = BLE_HID_TARGET_APPLIED; ++preference.revision;
    preference.selected = true; preference.target = submitted_target.target;
    poll_at(1040); assert(controls.pending == P_BLE_MANAGEMENT);
    bluetooth.advertising = true; poll_at(1050);
    assert(controls.pending == P_NONE && strstr(controls.message, "Connect from that host"));
    handle(bluetooth_intent(APP_BLUETOOTH_TARGET_PREFER, 1)); assert(target_requests == 2 && submitted_target.order[0].address[0] == 0xc3 && submitted_target.order[1].address[0] == 0xc2);
    preference.request_id = controls.ble_request_id; preference.request_status = BLE_HID_TARGET_UNCERTAIN; poll_at(1060);
    assert(controls.pending == P_NONE && view.bluetooth_target.uncertain && !view.bluetooth_target.can_select);
}
static void bluetooth_request_reuse(void)
{
    setup(); bonds.count = BLE_HID_MAX_BONDS; ++bonds.revision;
    for (unsigned i = 0; i < bonds.count; ++i) {
        bonds.bonds[i] = bonds.bonds[0]; bonds.bonds[i].address[5] = (uint8_t)i;
        memset(bonds.bonds[i].bond_id, (int)(i + 1U), BLE_HID_BOND_ID_BYTES);
    }
    context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, BLE_HID_MAX_BONDS - 1)));
    submit_error = ESP_ERR_NO_MEM; handle(bluetooth_intent(APP_BLUETOOTH_TARGET_PREFER, BLE_HID_MAX_BONDS - 1));
    assert(target_requests == 1 && controls.pending == P_NONE && submitted_target.count == BLE_HID_MAX_BONDS);
    assert(submitted_target.expected_bond_revision == bonds.revision && submitted_target.expected_revision == preference.revision);
    for (unsigned i = 0; i < bonds.count; ++i) {
        unsigned index = i ? i - 1U : BLE_HID_MAX_BONDS - 1U;
        assert(!memcmp(submitted_target.order[i].address, bonds.bonds[index].address, 6));
        assert(!memcmp(submitted_target.order[i].bond_id, bonds.bonds[index].bond_id, BLE_HID_BOND_ID_BYTES));
    }
    bonds.count = 1; ++bonds.revision; poll_at(1020);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 0)));
    submit_error = ESP_OK; handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 0)); poll_at(1030);
    assert(target_requests == 2 && submitted_target.count == 1 && submitted_target.selected);
    assert(!memcmp(submitted_target.target.bond_id, bonds.bonds[0].bond_id, BLE_HID_BOND_ID_BYTES));
    const unsigned char *tail = (const unsigned char *)&submitted_target.order[1];
    for (size_t i = 0; i < sizeof(submitted_target.order) - sizeof(submitted_target.order[0]); ++i) assert(tail[i] == 0);
}
static void bluetooth_connected_target_selection(void)
{
    setup(); bluetooth.connection_admitted = bluetooth.connected = bluetooth.ready = bluetooth.encrypted = true;
    context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 1)));
    assert(view.bluetooth_target.can_select);
    context.release_pending = true;
    app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 1));
    assert(effect.release_input && app_connectivity_controls_bluetooth_busy());
    poll_at(1020); assert(!disconnects && !target_requests);
    context.release_pending = false; poll_at(1030);
    assert(disconnects == 1 && !app_connectivity_controls_bluetooth_busy()); /* Rejected SDK close is not success. */
}
static void bluetooth_private_target_reconnect(void)
{
    setup(); bluetooth.connection_admitted = bluetooth.connected = bluetooth.ready = bluetooth.encrypted = true;
    bonds.bonds[1].selection_supported = false;
    context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 1)));
    assert(view.bluetooth_target.can_select && !view.bluetooth_target.selection_supported && view.bluetooth_target.can_forget);
    assert(strstr(view.bluetooth_target.reason, "verified identity"));
    bonds.current_match_known = true; bonds.bonds[1].is_current = true; poll_at(1015);
    assert(strstr(view.bluetooth_target.reason, "input ready"));
    bluetooth.ready = false; poll_at(1017); assert(!strstr(view.bluetooth_target.reason, "input ready"));
    app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 1));
    app_bluetooth_management_snapshot_t manager; app_bluetooth_management_snapshot(&manager);
    assert(effect.release_input && manager.operation == APP_BT_MANAGE_RECONNECT && manager.active && !target_requests);
}
static void bluetooth_missing_fingerprint_rejects_before_release(void)
{
    setup(); bluetooth.connection_admitted = bluetooth.connected = bluetooth.ready = bluetooth.encrypted = true;
    bonds.bonds[1].selection_supported = bonds.bonds[1].bond_id_known = false;
    memset(bonds.bonds[1].bond_id, 0, sizeof(bonds.bonds[1].bond_id));
    context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 1)));
    assert(!view.bluetooth_target.can_select && strstr(view.bluetooth_target.reason, "cannot be verified"));
    app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 1));
    assert(!effect.release_input && !app_connectivity_controls_bluetooth_busy() && !disconnects && !target_requests && bluetooth.ready);
}
static void bluetooth_current_evidence(void)
{
    setup(); bonds.bonds[0].is_current = true; bonds.current_match_known = true; bluetooth.connected = false; poll_at(1010);
    assert(!view.bluetooth_hosts[0].connected);
    bluetooth.connected = true; bonds.current_match_known = false; poll_at(1020); assert(!view.bluetooth_hosts[0].connected);
    bonds.current_match_known = true; bluetooth.connection_admitted = true; poll_at(1030); assert(view.bluetooth_hosts[0].connected && !view.bluetooth_hosts[1].connected);
    bonds.known = false; poll_at(1040); assert(!view.bluetooth_hosts_available && !view.bluetooth_host_count && !view.bluetooth_hosts[0].connected);
    bonds.known = true; bluetooth.connection_admitted = bluetooth.connected = false; poll_at(1050); assert(!view.bluetooth_hosts[0].connected);
}
static void bluetooth_exact_forget_confirmation(void)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 1)));
    assert(view.bluetooth_target.can_forget);
    app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_FORGET_REQUEST, 1));
    assert(!single_forgets && effect.confirmation == APP_CONFIRM_BLUETOOTH_FORGET_ONE); apply_overlay(effect);
    app_intent_t approve = {.action = APP_CONFIRM, .x = 1, .epoch = context.epoch, .ui_revision = context.ui_revision};
    ++bonds.bonds[1].bond_id[0]; /* Even a faulty unchanged revision cannot retarget the captured fingerprint. */
    handle(approve); assert(!single_forgets && !app_connectivity_controls_bluetooth_busy());
}
static void bluetooth_disconnected_release_does_not_block_management(void)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH;
    context.release_pending = true; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 0)));
    assert(view.bluetooth_target.can_select && view.bluetooth_target.can_forget);
    for (input_mode_t mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        context.mode = mode; poll_at(1020 + (uint64_t)mode * 20);
        apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_FORGET_REQUEST, 0)));
        app_intent_t approve = {.action = APP_CONFIRM, .x = 1, .epoch = context.epoch, .ui_revision = context.ui_revision};
        apply_overlay(handle(approve)); assert(app_connectivity_controls_bluetooth_busy());
        poll_at(1030 + (uint64_t)mode * 20);
        assert(single_forgets == (unsigned)mode + 1 && !app_connectivity_controls_bluetooth_busy());
        assert(strstr(controls.message, "rejected") && context.release_pending);
    }
    assert(!pairing_opens && !all_forgets && !target_requests && !disconnects);
}
static void bluetooth_connected_release_still_blocks_disconnect(void)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH;
    bluetooth.connection_admitted = bluetooth.connected = true; context.release_pending = true; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 0)));
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_FORGET_REQUEST, 0)));
    app_intent_t approve = {.action = APP_CONFIRM, .x = 1, .epoch = context.epoch, .ui_revision = context.ui_revision};
    apply_overlay(handle(approve)); poll_at(1020);
    assert(app_connectivity_controls_bluetooth_busy() && !disconnects && !single_forgets);
    context.release_pending = false; poll_at(1030);
    assert(disconnects == 1 && !app_connectivity_controls_bluetooth_busy() && !single_forgets);
    assert(strstr(controls.message, "could not be disconnected"));
}
static void bluetooth_back_cancels_future_pairing(void)
{
    setup(); context.overlay = APP_OVERLAY_BLUETOOTH_PAIRING; poll_at(1010);
    app_connectivity_controls_effect_t effect;
    assert(app_connectivity_controls_bluetooth_start(APP_PAIR_OPEN, 0, bonds.revision, preference.revision, &effect));
    context.release_pending = true; poll_at(1020); assert(target_requests == 1 && !pairing_opens);
    assert(!submitted_target.selected && context.release_pending);
    app_bluetooth_management_snapshot_t receipt; app_bluetooth_management_snapshot(&receipt);
    preference.request_id = receipt.policy_request_id; preference.request_status = BLE_HID_TARGET_PENDING;
    context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1040);
    app_bluetooth_management_snapshot(&receipt);
    assert(receipt.phase == APP_BT_MANAGE_CANCELLED && receipt.uncertain && receipt.policy_request_id && !pairing_opens);
    preference.request_status = BLE_HID_TARGET_APPLIED; preference.durable = true; ++preference.revision;
    poll_at(1050); assert(!pairing_opens && !view.bluetooth_management.pending && view.bluetooth_management.uncertain);
}
static void bluetooth_pairing_status_requires_owner_evidence(void)
{
    setup(); context.overlay = APP_OVERLAY_BLUETOOTH_PAIRING; poll_at(1010);
    assert(!strcmp(view.bluetooth_management.status, "Pairing closed") && !view.bluetooth_management.remaining_known);
    bluetooth.advertising = true; poll_at(1020);
    assert(!strcmp(view.bluetooth_management.status, "Pairing closed")); /* Saved reconnect advertising is insufficient. */
    bluetooth.pairing_open = true; bluetooth.advertising = false; bluetooth.pairing_remaining_ms = 120000; poll_at(1030);
    assert(!strcmp(view.bluetooth_management.status, "Pairing open") && view.bluetooth_management.remaining_seconds == 120);
    bluetooth.advertising = true; poll_at(1040);
    assert(!strcmp(view.bluetooth_management.status, "Pairing open") && view.bluetooth_management.can_stop);
    bluetooth.connected = true; poll_at(1050); assert(!strcmp(view.bluetooth_management.status, "Pairing open") && !view.bluetooth_management.connected);
    bluetooth.connection_admitted = true; poll_at(1051); assert(!strcmp(view.bluetooth_management.status, "Connecting securely"));
    bluetooth.ready = true; poll_at(1060); assert(!strcmp(view.bluetooth_management.status, "Connected"));
    bluetooth.connection_admitted = bluetooth.connected = bluetooth.ready = bluetooth.pairing_open = false; bluetooth.pairing_remaining_ms = 0; poll_at(1070);
    assert(!strcmp(view.bluetooth_management.status, "Pairing closed") && !view.bluetooth_management.remaining_known);
    bluetooth.failed = true; poll_at(1080);
    assert(!strcmp(view.bluetooth_management.status, "Bluetooth failed") && !view.bluetooth_management.can_add);
    assert(!pairing_opens && !target_requests && !all_forgets && !single_forgets);
}
static void api_window(void)
{
    setup(); context.overlay = APP_OVERLAY_API_CONNECTION; poll_at(1010); assert(view.api.known && view.api.can_open && !view.api.ready);
    handle(api_intent(APP_API_WINDOW_OPEN)); assert(opens == 1 && controls.pending == P_API_OPEN);
    poll_at(1020); assert(controls.pending == P_API_OPEN);
    api.trust.import_window = true; api.trust.window_remaining_ms = 90000; poll_at(1030);
    assert(controls.pending == P_NONE && view.api.commissioning_open && view.api.remaining_seconds == 90);
    handle(api_intent(APP_API_WINDOW_CLOSE)); assert(closes == 1);
    api.trust.import_window = false; poll_at(1040); assert(controls.pending == P_NONE);
    api.server.state = APP_CONTROL_READY; api.server.tls = true; strcpy(api.server.address, "https://device.local:8443"); poll_at(1050);
    assert(view.api.ready && !strcmp(view.api.url, api.server.address));
    assert(view.wifi_lan_service == APP_UI_LAN_SERVICE_READY && !strcmp(view.wifi_lan_service_url, api.server.address));
    api.server.tls = false; poll_at(1060); assert(!view.api.ready && !view.api.url[0]);
    assert(view.wifi_lan_service == APP_UI_LAN_SERVICE_ERROR && !view.wifi_lan_service_url[0]);
    api.server.state = APP_CONTROL_STARTING; poll_at(1070); assert(view.wifi_lan_service == APP_UI_LAN_SERVICE_STARTING);
    api.server.state = APP_CONTROL_STOPPED; poll_at(1080); assert(view.wifi_lan_service == APP_UI_LAN_SERVICE_DISABLED);
    api.initialized = false; poll_at(1090); assert(view.wifi_lan_service == APP_UI_LAN_SERVICE_UNAVAILABLE);
}
static void api_review(void)
{
    setup(); context.overlay = APP_OVERLAY_API_REVIEW; api.trust.import_window = api.trust.review_pending = true;
    memset(review.digest, 'a', 64); review.digest[64] = 0; memcpy(api.trust.candidate_digest, review.digest, 65);
    strcpy(review.host, "device.local"); review.principal_count = 1; review.principal_scopes[0] = CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL;
    review.principals[0].id[0] = 1; review.principals[0].enabled = true; review.principals[0].scopes = review.principal_scopes[0];
    memset(review.certificate_pem_sha256, 'b', 64); review.certificate_pem_sha256[64] = 0;
#if APP_CONTROL_TRUST_REVIEW_ABI >= 2
    review.port = 443;
#endif
    poll_at(1010);
    assert(view.api.review_complete && view.api.review_page_count);
    if (view.api.review_page_count > 1) { handle(api_intent(APP_API_REVIEW_APPROVE)); assert(!approvals); }
    while (view.api.review_page + 1U < view.api.review_page_count) {
        app_intent_t next = api_intent(APP_API_REVIEW_PAGE); ++next.api.page; handle(next); ++context.ui_revision; poll_at(controls.now_ms + 10);
    }
    assert(view.api.can_approve); handle(api_intent(APP_API_REVIEW_APPROVE)); assert(approvals == 1 && !memcmp(approved_digest, review.digest, 65));
    ++api.trust.revision; api.trust.present = true; api.trust.import_window = api.trust.review_pending = false;
#if APP_CONTROL_TRUST_REVIEW_ABI >= 2
    memcpy(api.trust.active_digest, review.digest, 65);
#endif
    poll_at(controls.now_ms + 10);
#if APP_CONTROL_TRUST_REVIEW_ABI >= 2
    assert(controls.pending == P_NONE && strstr(controls.message, "Reviewed"));
#else
    assert(controls.pending == P_NONE && strstr(controls.message, "unconfirmed"));
#endif
    review.principal_count = CP_MAX_PRINCIPALS + 1U; api.trust.import_window = api.trust.review_pending = true; poll_at(1200);
    assert(!view.api.review_complete && !view.api.can_approve);
}
static void clock_and_timeout(void)
{
    setup(); handle(wifi_intent(APP_SAVED_WIFI_JOIN, 0)); poll_at(31000); assert(controls.pending == P_NONE && strstr(controls.message, "not confirmed"));
    poll_at(1000); assert(controls.clock_failed); handle(wifi_intent(APP_SAVED_WIFI_JOIN, 1)); assert(joins == 1);
}
static void complete_review_fixture(bool maximal)
{
    context.overlay = APP_OVERLAY_API_REVIEW;
    api.trust.import_window = api.trust.review_pending = true;
    trust_review_fixture(&review, maximal); memcpy(api.trust.candidate_digest, review.digest, 65);
    poll_at(controls.now_ms + 10);
}
static void review_to_end(void)
{
    while (view.api.review_page + 1U < view.api.review_page_count) {
        assert(!view.api.can_approve);
        app_intent_t next = api_intent(APP_API_REVIEW_PAGE); ++next.api.page; handle(next);
        ++context.ui_revision; poll_at(controls.now_ms + 10);
    }
    assert(view.api.can_approve);
}
static void api_review_authority(void)
{
    setup(); complete_review_fixture(false);
    assert(view.api.review_complete && view.api.review_page_count > 8);
    char combined[APP_API_REVIEW_PAGES * 257] = {0}; size_t length = 0;
    for (unsigned p = 0; p < view.api.review_page_count; ++p)
        for (const char *s = view.api.review_pages[p]; *s; ++s) if (*s != '\n') combined[length++] = *s;
    const char *expected[] = {"HTTPS listener port: 8443", "clock-one.example", "clock-two.example",
        "Product: esp32-input; board: mosaico", "Layout: ota-v2", "Minimum release: 42; private source: yes",
        review.update_tls_ca_sha256, review.update_public_key_sha256, review.update_catalog_url,
        "Firmware update policy: configured", "owner-loss continue: yes", review.update_policy.allowed_digest};
    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i) assert(strstr(combined, expected[i]));
    app_intent_t jump = api_intent(APP_API_REVIEW_PAGE); jump.api.page = view.api.review_page_count - 1;
    handle(jump); poll_at(controls.now_ms + 10); assert(view.api.review_page == 0 && !view.api.can_approve);
    review_to_end(); app_intent_t stale = api_intent(APP_API_REVIEW_APPROVE);
    review.update_allow_private_source = false; poll_at(controls.now_ms + 10);
    assert(!view.api.can_approve && view.api.review_page == 0); handle(stale); assert(!approvals);
    review_to_end(); handle(api_intent(APP_API_REVIEW_APPROVE)); assert(approvals == 1);
    printf("Review normal source/grant/policy pages: %u\n", view.api.review_page_count);
}
static void api_review_missing(void)
{
    setup();
    for (unsigned field = 0; field < 8; ++field) {
        complete_review_fixture(false);
        if (field == 0) review.port = 0;
        else if (field == 1) review.clock_server_count = APP_CLOCK_SERVER_CAPACITY + 1;
        else if (field == 2) memset(review.clock_servers[0], 'x', sizeof(review.clock_servers[0]));
        else if (field == 3) review.clock_servers[1][0] = 0;
        else if (field == 4) review.update_tls_ca_sha256[0] = 0;
        else if (field == 5) review.update_product[0] = 0;
        else if (field == 6) memset(review.update_board, 'x', sizeof(review.update_board));
        else review.update_layout[0] = 0;
        poll_at(controls.now_ms + 10); assert(!view.api.review_complete && !view.api.can_approve);
        handle(api_intent(APP_API_REVIEW_APPROVE)); assert(!approvals);
    }
}
static void api_review_capacity(void)
{
    setup(); complete_review_fixture(true);
    assert(view.api.review_complete && view.api.review_page_count > 32 && view.api.review_page_count <= APP_API_REVIEW_PAGES);
    review_to_end(); printf("Review maximal public metadata pages: %u\n", view.api.review_page_count);
    /* Exact 64-bit boundary: the high page bit is necessary, with no shift by
     * 64. An extra claimed bit also cannot complete a smaller review. */
    controls.reviewed_pages = UINT64_MAX >> 1; assert(!all_reviewed(64) && all_reviewed(63));
    controls.reviewed_pages |= UINT64_C(1) << 63; assert(all_reviewed(64) && !all_reviewed(63));
    assert(!all_reviewed(0) && !all_reviewed(65));
    writer_t writer = {.valid = true};
    memset(controls.api.review_pages, 0, sizeof(controls.api.review_pages));
    for (unsigned i = 0; i < APP_API_REVIEW_PAGES * 5; ++i) write_line(&writer, "complete line");
    assert(writer.valid && writer.page == 63 && writer.lines == 5);
    write_line(&writer, "must not be silently dropped"); assert(!writer.valid && writer.page == 64);
}
static void api_review_package(void)
{
    setup(); complete_review_fixture(false); trust_package_review_fixture(&review);
    memcpy(api.trust.candidate_digest, review.digest, 65); poll_at(controls.now_ms + 10);
    assert(view.api.review_complete && view.api.review_page_count > 32);
    review_to_end(); printf("Review 7735-byte maximal-count enrollment pages: %u\n", view.api.review_page_count);
    complete_review_fixture(false); review.capture_grant_count = review.clock_server_count = 0;
    review.update_source_present = review.update_policy_present = false; poll_at(controls.now_ms + 10);
    assert(view.api.review_complete && view.api.review_page_count <= 4);
    review_to_end(); printf("Review minimal enrollment pages: %u\n", view.api.review_page_count);
}
static void bluetooth_provisional_pairing_window(void)
{
    setup(); context.overlay = APP_OVERLAY_BLUETOOTH_PAIRING;
    bonds.count = 0; bluetooth.pairing_open = true; bluetooth.pairing_remaining_ms = 120000;
    for (unsigned i = 0; i < 200; ++i) {
        bluetooth.connected = (i & 1U) != 0; bluetooth.advertising = !bluetooth.connected;
        ++bluetooth.generation; poll_at(1010U + i);
        assert(!strcmp(view.bluetooth_management.status, "Pairing open"));
        assert(!view.bluetooth_management.connected && view.bluetooth_management.can_stop && view.bluetooth_management.can_add);
        assert(view.bluetooth_management.window_open && view.bluetooth_management.remaining_seconds == 120);
    }
}
static void bluetooth_provisional_forget_confirmation(void)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 0)));
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_FORGET_REQUEST, 0)));
    bluetooth.connected = true; ++bluetooth.generation; context.release_pending = true; poll_at(1010);
    app_intent_t approve = {.action = APP_CONFIRM, .x = 1, .epoch = context.epoch, .ui_revision = context.ui_revision};
    apply_overlay(handle(approve)); poll_at(1020);
    assert(disconnects == 1 && !single_forgets && !app_connectivity_controls_bluetooth_busy());
    assert(strstr(controls.message, "could not be disconnected")); /* SDK refusal stays a failure. */
}

static void bluetooth_provisional_start_edge(bool forget)
{
    setup(); context.overlay = APP_OVERLAY_SETTINGS; context.settings_page = APP_SETTINGS_BLUETOOTH;
    bluetooth.connected = true; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS, 0)));
    if (forget) apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_FORGET_REQUEST, 0)));
    /* Only the SDK observation changes. The UI owner still holds the old
     * provisional snapshot and the exact approved catalogue/policy. */
    bluetooth.connected = false; ++bluetooth.generation;
    if (forget) {
        app_intent_t approve = {.action = APP_CONFIRM, .x = 1, .epoch = context.epoch, .ui_revision = context.ui_revision};
        apply_overlay(handle(approve));
    } else {
        app_connectivity_controls_effect_t effect = handle(bluetooth_intent(APP_BLUETOOTH_TARGET_SELECT, 0));
        assert(effect.release_input);
    }
    assert(app_connectivity_controls_bluetooth_busy()); poll_at(1020);
    assert(!disconnects);
    if (forget) assert(single_forgets == 1 && !app_connectivity_controls_bluetooth_busy()); /* Actual mock refusal retained. */
    else assert(target_requests == 1 && submitted_target.selected);
}

static void bluetooth_provisional_select_edge(void) { bluetooth_provisional_start_edge(false); }
static void bluetooth_provisional_forget_edge(void) { bluetooth_provisional_start_edge(true); }

static void bluetooth_exact_shared_disconnect(void)
{
    setup(); context.overlay=APP_OVERLAY_BLUETOOTH_DEVICES;
    bluetooth.connection_admitted=bluetooth.connected=bluetooth.ready=true;
    bonds.current_match_known=true; bonds.bonds[0].is_current=true; poll_at(1010);
    apply_overlay(handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DETAILS,0)));
    app_connectivity_controls_effect_t wrong=handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DISCONNECT_REQUEST,1));
    assert(!wrong.open_overlay && !disconnects);
    app_connectivity_controls_effect_t right=handle(bluetooth_intent(APP_BLUETOOTH_TARGET_DISCONNECT_REQUEST,0));
    assert(right.open_overlay && right.confirmation==APP_CONFIRM_DISCONNECT && right.confirmation_host_generation==bluetooth.generation);
    assert(!disconnects && !single_forgets && !all_forgets);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"bluetooth-unverified-reject", bluetooth_missing_fingerprint_rejects_before_release}, {"bluetooth-private-reconnect", bluetooth_private_target_reconnect}, {"bluetooth-connected-select", bluetooth_connected_target_selection},
        {"routing", routing}, {"wifi-order", wifi_order}, {"wifi-join", wifi_join}, {"wifi-forget", wifi_forget},
        {"bluetooth", bluetooth_targets}, {"bluetooth-current", bluetooth_current_evidence}, {"api-window", api_window}, {"api-review", api_review}, {"clock-timeout", clock_and_timeout},
        {"api-review-authority", api_review_authority}, {"api-review-missing", api_review_missing}, {"api-review-capacity", api_review_capacity},
        {"api-review-package", api_review_package}, {"bluetooth-request-reuse", bluetooth_request_reuse},
        {"bluetooth-forget-confirmation", bluetooth_exact_forget_confirmation},
        {"bluetooth-offline-release", bluetooth_disconnected_release_does_not_block_management},
        {"bluetooth-connected-release", bluetooth_connected_release_still_blocks_disconnect},
        {"bluetooth-back-cancel", bluetooth_back_cancels_future_pairing},
        {"bluetooth-pairing-status", bluetooth_pairing_status_requires_owner_evidence},
        {"bluetooth-provisional-window", bluetooth_provisional_pairing_window},
        {"bluetooth-provisional-confirm", bluetooth_provisional_forget_confirmation},
        {"bluetooth-exact-shared-disconnect", bluetooth_exact_shared_disconnect},
        {"bluetooth-start-select-edge", bluetooth_provisional_select_edge},
        {"bluetooth-start-forget-edge", bluetooth_provisional_forget_edge}};
    if (!strcmp(argv[1], "--list")) { for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) puts(cases[i].name); return 0; }
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) if (!strcmp(argv[1], cases[i].name)) {
        cases[i].run(); printf("Connectivity controls: %s passed\n", cases[i].name); return 0;
    }
    return 2;
}
