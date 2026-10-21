// SPDX-License-Identifier: Apache-2.0
/* Included by main.c after native helpers and before app_remote_input bindings.
 * One coordinator owns these receipts. SDK/flash/radio work stays in existing
 * managers; this fragment never opens LVGL or performs a hardware operation. */
#include "app_remote_device.h"
#include "app_device_restart.h"
#include "app_wifi_profiles.h"
#include "bt_audio.h"
#include <inttypes.h>
#include <stdarg.h>

static bool remote_complete(void *context, const cp_result_t *result);
static app_remote_device_t *s_remote_device;
static uint64_t s_remote_device_maintenance_revision = 1;
static bool s_remote_device_maintenance_active;
static bool remote_device_maintenance(void) { return s_remote_device_maintenance_active; }
static bool remote_device_restart_available;
typedef struct {
    bool used, applied, name_done, transport_done, disconnect_requested, action_started;
    bool cancelling, stop_requested;
    uint64_t name_id, haptic_id, saved_request, applied_revision;
    uint64_t target_id, bond_forget_id, restart_id, wifi_id, wifi_revision, target_revision;
    unsigned reset_step;
    device_settings_t before;
    app_remote_device_request_t request;
    app_remote_device_outcome_t outcome;
} remote_device_binding_job_t;
static remote_device_binding_job_t s_remote_device_jobs[APP_REMOTE_DEVICE_JOBS];
typedef struct {
    bool used;
    uint64_t created_ms, ui_revision;
    uint64_t wifi_revision, target_revision;
    app_remote_device_request_t request;
    app_remote_device_approval_t approval;
} remote_device_review_t;
static remote_device_review_t s_remote_device_reviews[4];
static char s_remote_device_local_review[CP_ID_HEX_CAPACITY];

static bool remote_device_id_equal(const void *a, const void *b, size_t bytes)
{
    const uint8_t *x = a, *y = b; unsigned difference = 0;
    for (size_t i = 0; i < bytes; ++i) difference |= (unsigned)(x[i] ^ y[i]);
    return difference == 0;
}
static void remote_device_hex(const uint8_t *data, size_t bytes, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) { out[i*2] = digits[data[i] >> 4]; out[i*2+1] = digits[data[i] & 15U]; }
    out[bytes*2] = 0;
}
static bool remote_device_session(const cp_identity_t *identity)
{
    return app_control_runtime_session_alive(identity->session_id, identity->principal_id);
}
static bool remote_device_random(void *context, uint8_t *bytes, size_t count)
{
    (void)context;
    if (!bytes || count != CP_ID_BYTES) return false;
    esp_fill_random(bytes, count); return true;
}
#include "app_remote_device_read.inc"
static bool remote_device_observe(void *context, cp_operation_id_t operation, app_remote_device_state_t *state)
{
    (void)context;
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    ble_hid_bond_list_t bonds = ble_hid_bond_list();
    portENTER_CRITICAL(&s_control_lock);
    bool busy = s_touch_down || s_physical_down || s_local_submission_pending;
    bool awake = s_screen_accepts_input;
    portEXIT_CRITICAL(&s_control_lock);
    uint64_t revision = s_state_sequence;
    if ((operation >= CP_OP_CONFIG_STAGE && operation <= CP_OP_CONFIG_RESET) || operation == CP_OP_CONFIG_GET) revision = s_settings_revision;
    else if (operation >= CP_OP_UI_NAVIGATE && operation <= CP_OP_UI_MODE_PANEL_OPEN) revision = s_ui_revision;
    else if (operation == CP_OP_IMU_CALIBRATE || operation == CP_OP_DISPLAY_WAKE || operation == CP_OP_DISPLAY_SLEEP) revision = s_ui_revision;
    else if (operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && operation <= CP_OP_BLUETOOTH_TARGET_REQUEST)
        revision = operation == CP_OP_BLUETOOTH_TARGET_REQUEST || operation == CP_OP_BLUETOOTH_BOND_FORGET ? bonds.revision : bluetooth.generation;
    else if (operation == CP_OP_HAPTICS_TEST || operation == CP_OP_HAPTICS_RECOVER) revision = board_haptics_status().request_id;
    else if (operation == CP_OP_PREFERENCES_RESET) revision = s_settings_revision;
    else if (operation == CP_OP_SYSTEM_RESTART || operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) revision = s_remote_device_maintenance_revision;
    *state = (app_remote_device_state_t){.management_generation = s_management_generation,
        .resource_revision = revision, .settings_revision = s_settings_revision, .ui_revision = s_ui_revision,
        .bluetooth_generation = bluetooth.generation, .bond_revision = bonds.revision,
        .settings = s_settings, .locked = s_engine.state.locked, .screen_awake = awake, .local_busy = busy,
        .transition_busy = s_transport_switch_pending || s_pair_operation != APP_CONFIRM_NONE || s_name_should_persist};
    return true;
}
static bool remote_device_target_store_ready(const ble_hid_target_snapshot_t *target)
{
    bool terminal = target->request_status == BLE_HID_TARGET_IDLE || target->request_status == BLE_HID_TARGET_APPLIED;
    bool known_default = !target->selected && target->revision == 0 && target->request_id == 0 &&
        target->request_status == BLE_HID_TARGET_IDLE;
    /* A selected key may be unavailable after removal while its persisted
     * preferences remain healthy. Successful loading also proves absence. */
    return target->loaded && (target->durable || known_default) && terminal && target->request_error == ESP_OK;
}
static bool remote_device_owner_available(void *context, cp_operation_id_t operation, uint32_t fields,
    char reason[CP_REASON_CAPACITY])
{
    (void)context;
    if (reason) reason[0] = 0;
    bool available = s_state_sequence != UINT64_MAX && s_settings_revision != UINT64_MAX && s_ui_revision != UINT64_MAX;
    board_capabilities_t board = board_capabilities();
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    if (operation >= CP_OP_CONFIG_STAGE && operation <= CP_OP_CONFIG_RESET) {
        if (fields & APP_REMOTE_SETTING_BRIGHTNESS) available = available && board.brightness;
        if (fields & APP_REMOTE_SETTING_HAPTICS) available = available && board.haptics;
        if (fields & APP_REMOTE_SETTING_BLUETOOTH_NAME) available = available && bluetooth.initialized && !bluetooth.failed && !s_name_should_persist;
    } else if (operation >= CP_OP_UI_NAVIGATE && operation <= CP_OP_UI_MODE_PANEL_OPEN) available = available && board.display;
    else if (operation == CP_OP_IMU_CALIBRATE) available = available && board.imu;
    else if (operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && operation <= CP_OP_BLUETOOTH_TARGET_REQUEST) {
        available = available && bluetooth.initialized && !bluetooth.failed;
        if (operation == CP_OP_BLUETOOTH_TARGET_REQUEST || operation == CP_OP_BLUETOOTH_BOND_FORGET) {
            ble_hid_bond_list_t bonds = ble_hid_bond_list();
            available = available && bonds.known && !bonds.updating;
        }
    } else if (operation == CP_OP_HAPTICS_TEST) available = available && board.haptics && board_haptics_enabled();
    else if (operation == CP_OP_HAPTICS_RECOVER) available = available && board_haptics_status().worker_available &&
        board_haptics_status().state != BOARD_HAPTIC_PENDING && board_haptics_status().state != BOARD_HAPTIC_ACTIVE;
    else if (operation == CP_OP_DISPLAY_WAKE || operation == CP_OP_DISPLAY_SLEEP) available = available && board.brightness;
    else if (operation == CP_OP_OPERATION_CONFIRM || operation == CP_OP_OPERATION_CANCEL) { }
    else if (operation == CP_OP_SYSTEM_RESTART) available = available && remote_device_restart_available && s_remote_device_maintenance_revision != UINT64_MAX;
    else if (operation == CP_OP_PREFERENCES_RESET) available = available && settings_store_status().available;
    else if (operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) {
        app_wifi_profiles_snapshot_t wifi; app_wifi_profiles_snapshot(&wifi);
        ble_hid_bond_list_t bonds = ble_hid_bond_list(); ble_hid_target_snapshot_t target = ble_hid_target_snapshot();
        available = available && settings_store_status().available && wifi.known && !wifi.uncertain && !wifi.operation_pending &&
            bonds.known && !bonds.updating && remote_device_target_store_ready(&target) && s_remote_device_maintenance_revision != UINT64_MAX;
    } else available = false;
    if (!available && reason) (void)snprintf(reason, CP_REASON_CAPACITY, "%s", "actual_device_owner_unavailable");
    return available;
}
static cp_error_t remote_device_validate_settings(void *context, const device_settings_t *candidate, uint32_t fields, bool durable)
{
    (void)context;
    if (!candidate || !device_settings_validate(candidate)) return CP_INVALID;
    if (durable && !settings_store_status().available) return CP_UNAVAILABLE;
    char reason[CP_REASON_CAPACITY];
    if (!remote_device_owner_available(NULL, CP_OP_CONFIG_APPLY, fields, reason)) return CP_UNAVAILABLE;
    if (s_transport_switch_pending || s_pair_operation != APP_CONFIRM_NONE || s_name_should_persist) return CP_BUSY;
    if (fields & APP_REMOTE_SETTING_TRANSPORT) {
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot(); usb_hid_snapshot_t usb = usb_hid_snapshot();
        bool ready = candidate->preferred_transport == DEVICE_TRANSPORT_USB ? usb.initialized && !usb.failed : bluetooth.initialized && !bluetooth.failed;
        if (!ready) return CP_UNAVAILABLE;
    }
    return CP_OK;
}
static remote_device_binding_job_t *remote_device_binding(uint64_t token)
{
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i)
        if (s_remote_device_jobs[i].used && s_remote_device_jobs[i].request.token == token) return &s_remote_device_jobs[i];
    return NULL;
}
static void remote_device_failed(remote_device_binding_job_t *job, cp_error_t error, bool uncertain)
{
    job->outcome = (app_remote_device_outcome_t){.phase = uncertain ? APP_REMOTE_DEVICE_UNCERTAIN : APP_REMOTE_DEVICE_FAILED,
        .error = error, .resource_revision = s_state_sequence};
}
static void remote_device_applied(remote_device_binding_job_t *job, uint64_t revision, bool durable)
{
    job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_COMPLETED, .boundary_confirmed = true,
        .durable_confirmed = durable, .resource_revision = revision};
}
static bool remote_device_target(const app_remote_device_request_t *request, ble_hid_target_t *target)
{
    ble_hid_bond_list_t bonds = ble_hid_bond_list();
    if (!bonds.known || bonds.updating || bonds.revision != request->bond_revision || !request->peer_id[0]) return false;
    for (uint8_t i = 0; i < bonds.count; ++i) {
        const ble_hid_bond_t *bond = &bonds.bonds[i]; char id[BLE_HID_BOND_ID_BYTES * 2U + 1U];
        if (!bond->bond_id_known || !bond->address_type_known ||
            (request->operation == CP_OP_BLUETOOTH_TARGET_REQUEST && !bond->selection_supported)) continue;
        remote_device_hex(bond->bond_id, sizeof(bond->bond_id), id);
        if (strcmp(request->peer_id, id)) continue;
        *target = (ble_hid_target_t){.address_type = bond->address_type, .bond_id_known = true};
        memcpy(target->address, bond->address, sizeof(target->address)); memcpy(target->bond_id, bond->bond_id, sizeof(target->bond_id));
        return true;
    }
    return false;
}

static void remote_device_settings_progress(remote_device_binding_job_t *job)
{
    const app_remote_device_request_t *request = &job->request;
    if (!job->applied) {
        if (job->cancelling) {
            if (job->name_id) {
                ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
                if (name.request_id == job->name_id && (name.status == BLE_HID_NAME_PENDING || name.status == BLE_HID_NAME_APPLYING)) return;
            }
            if (s_transport_switch_pending) return;
            if (job->name_id || job->transport_done) remote_device_failed(job, CP_TOO_LATE, true);
            else job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true};
            return;
        }
        if (!remote_device_session(&request->identity)) { remote_device_failed(job, CP_UNAUTHENTICATED, false); return; }
        if (!job->name_done && (request->settings_fields & APP_REMOTE_SETTING_BLUETOOTH_NAME) &&
            strcmp(request->settings.bluetooth_name, job->before.bluetooth_name)) {
            if (!job->name_id) {
                esp_err_t error = ble_hid_name_request(request->settings.bluetooth_name, strlen(request->settings.bluetooth_name), &job->name_id);
                if (error != ESP_OK) remote_device_failed(job, CP_UNAVAILABLE, false);
                return;
            }
            ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
            if (name.request_id != job->name_id) { remote_device_failed(job, CP_STALE, true); return; }
            if (name.status == BLE_HID_NAME_FAILED || name.status == BLE_HID_NAME_UNCERTAIN) {
                remote_device_failed(job, CP_STORAGE_UNCERTAIN, name.status == BLE_HID_NAME_UNCERTAIN); return;
            }
            if (name.status != BLE_HID_NAME_APPLIED || !name.active_known || strcmp(name.active, request->settings.bluetooth_name)) return;
            job->name_done = true;
        }
        app_input_transport_t target = request->settings.preferred_transport == DEVICE_TRANSPORT_USB ? APP_INPUT_USB : APP_INPUT_BLUETOOTH;
        if ((request->settings_fields & APP_REMOTE_SETTING_TRANSPORT) && s_input_transport != target) {
            if (!job->transport_done) {
                ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
                if (!request_input_transport_internal(target, &bluetooth, false)) { remote_device_failed(job, CP_UNAVAILABLE, false); return; }
                job->transport_done = true;
            }
            if (s_transport_switch_pending) return;
            if (s_input_transport != target) { remote_device_failed(job, CP_STALE, false); return; }
        }
        if (s_transport_switch_pending) return;
        device_settings_t interim = job->before;
        if (job->transport_done) interim.preferred_transport = request->settings.preferred_transport;
        if (!device_settings_equal(&s_settings, &job->before) && !device_settings_equal(&s_settings, &interim)) {
            remote_device_failed(job, CP_STALE, job->name_done); return;
        }
        if (request->settings_fields & (APP_REMOTE_SETTING_TAP | APP_REMOTE_SETTING_SHORTCUT | APP_REMOTE_SETTING_MOTION)) cancel_input();
        if (!update_settings_internal(&request->settings, false)) { remote_device_failed(job, CP_UNAVAILABLE, job->name_done); return; }
        if (request->durable && !persist_settings(&request->settings)) { remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return; }
        settings_store_status_t after = settings_store_status();
        if (request->settings_fields & APP_REMOTE_SETTING_BRIGHTNESS) {
            (void)device_idle_activity(&s_idle, s_now_ms);
            s_brightness_requested = (int)s_settings.brightness_percent;
            if (board_brightness_set(s_brightness_requested) != ESP_OK) { remote_device_failed(job, CP_UNAVAILABLE, true); return; }
        }
        if (request->settings_fields & APP_REMOTE_SETTING_HAPTICS) board_haptics_enable(s_settings.haptics_enabled);
        job->saved_request = after.requested_revision; job->applied_revision = s_settings_revision; job->applied = true;
    }
    if (!device_settings_equal(&s_settings, &request->settings) || s_settings_revision != job->applied_revision) {
        remote_device_failed(job, CP_STALE, true); return;
    }
    if (request->settings_fields & APP_REMOTE_SETTING_BRIGHTNESS) {
        board_brightness_status_t brightness = board_brightness_status();
        if (brightness.error != ESP_OK) { remote_device_failed(job, CP_UNAVAILABLE, true); return; }
        if (brightness.pending || !brightness.applied_known || brightness.applied_percent != request->settings.brightness_percent) return;
    }
    if ((request->settings_fields & APP_REMOTE_SETTING_HAPTICS) && board_haptics_enabled() != request->settings.haptics_enabled) return;
    if (!request->durable) { remote_device_applied(job, job->applied_revision, false); return; }
    settings_store_status_t storage = settings_store_status();
    if (!storage.available || storage.error != ESP_OK) { remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return; }
    if (storage.pending || storage.saved_revision < job->saved_request) {
        job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_EFFECTIVE,
            .boundary_confirmed = true, .resource_revision = job->applied_revision}; return;
    }
    remote_device_applied(job, job->applied_revision, true);
}

static void remote_device_bluetooth_progress(remote_device_binding_job_t *job)
{
    const app_remote_device_request_t *request = &job->request;
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    if (bluetooth.failed) { remote_device_failed(job, CP_UNAVAILABLE, job->action_started); return; }
    if (request->operation == CP_OP_BLUETOOTH_PAIRING_CLOSE) {
        if (!bluetooth.pairing_open) remote_device_applied(job, bluetooth.generation, false);
        return;
    }
    if (!job->action_started) {
        if (bluetooth.connected) {
            if (!job->disconnect_requested && bluetooth.generation != request->bluetooth_generation) { remote_device_failed(job, CP_STALE, false); return; }
            if (!job->disconnect_requested && !s_engine.state.release_pending && !bluetooth.congested) {
                if (!ble_hid_disconnect_generation(bluetooth.generation)) { remote_device_failed(job, CP_STALE, false); return; }
                job->disconnect_requested = true;
            }
            return;
        }
        if (job->cancelling) {
            job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true}; return;
        }
        if (!remote_device_session(&request->identity)) { remote_device_failed(job, CP_UNAUTHENTICATED, false); return; }
        if (request->operation == CP_OP_BLUETOOTH_DISCONNECT || request->operation == CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE) {
            remote_device_applied(job, bluetooth.generation, false); return;
        }
        if (request->operation == CP_OP_BLUETOOTH_PAIRING_OPEN) {
            if (!ble_hid_pairing_open(request->window_seconds * 1000U)) { remote_device_failed(job, CP_UNAVAILABLE, false); return; }
        } else if (request->operation == CP_OP_BLUETOOTH_BONDS_FORGET_ALL) {
            if (!ble_hid_forget_bonds_without_pairing(bluetooth.generation)) { remote_device_failed(job, CP_UNAVAILABLE, false); return; }
        } else if (request->operation == CP_OP_BLUETOOTH_BOND_FORGET) {
            ble_hid_target_t target;
            if (!remote_device_target(request, &target) ||
                ble_hid_forget_bond_request(bluetooth.generation, request->bond_revision, &target, &job->bond_forget_id) != ESP_OK) {
                remote_device_failed(job, CP_STALE, false); return;
            }
        } else if (request->operation == CP_OP_BLUETOOTH_TARGET_REQUEST) {
            ble_hid_target_t target;
            if (!remote_device_target(request, &target) || ble_hid_target_select(request->bond_revision, &target) != ESP_OK) {
                remote_device_failed(job, CP_STALE, false); return;
            }
            job->target_id = ble_hid_target_snapshot().request_id;
        }
        job->action_started = true;
        return;
    }
    if (request->operation == CP_OP_BLUETOOTH_PAIRING_OPEN) {
        if (bluetooth.pairing_open && bluetooth.pairing_remaining_ms <= request->window_seconds * 1000U)
            remote_device_applied(job, bluetooth.generation, false);
    } else if (request->operation == CP_OP_BLUETOOTH_BONDS_FORGET_ALL) {
        if (bluetooth.bond_forget_state == BLE_HID_FORGET_FAILED) remote_device_failed(job, CP_STORAGE_UNCERTAIN, true);
        else if (bluetooth.bond_forget_state == BLE_HID_FORGET_DONE && bluetooth.bond_count_known && !bluetooth.bond_count)
            remote_device_applied(job, ble_hid_bond_list().revision, true);
    } else if (request->operation == CP_OP_BLUETOOTH_BOND_FORGET) {
        ble_hid_bond_forget_snapshot_t status = ble_hid_bond_forget_snapshot();
        if (status.request_id != job->bond_forget_id) remote_device_failed(job, CP_STALE, true);
        else if (status.status == BLE_HID_BOND_FORGET_SUCCEEDED) remote_device_applied(job, ble_hid_bond_list().revision, true);
        else if (status.status == BLE_HID_BOND_FORGET_FAILED || status.status == BLE_HID_BOND_FORGET_UNCERTAIN)
            remote_device_failed(job, CP_STORAGE_UNCERTAIN, status.status == BLE_HID_BOND_FORGET_UNCERTAIN);
        else if (status.status == BLE_HID_BOND_FORGET_CANCELLED)
            job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true};
    } else if (request->operation == CP_OP_BLUETOOTH_TARGET_REQUEST) {
        ble_hid_target_snapshot_t selected = ble_hid_target_snapshot();
        if (!job->target_id || selected.request_id != job->target_id) remote_device_failed(job, CP_STALE, true);
        else if (selected.request_status == BLE_HID_TARGET_FAILED || selected.request_status == BLE_HID_TARGET_UNCERTAIN)
            remote_device_failed(job, CP_STORAGE_UNCERTAIN, selected.request_status == BLE_HID_TARGET_UNCERTAIN);
        else if (selected.request_status == BLE_HID_TARGET_APPLIED && selected.selected && selected.available && selected.durable) {
            char id[BLE_HID_BOND_ID_BYTES * 2U + 1U]; remote_device_hex(selected.target.bond_id, sizeof(selected.target.bond_id), id);
            if (!selected.target.bond_id_known || strcmp(id, request->peer_id)) remote_device_failed(job, CP_STALE, true);
            else remote_device_applied(job, selected.revision, true);
        }
    }
}
static bool remote_device_microphone_safe(void)
{
    app_media_snapshot_t media;
    if (!app_media_runtime_snapshot(&media) || !media.audio_known || media.audio.quarantined) return false;
    return media.audio.hardware_off_confirmed ||
        (media.audio.state == AE_RUNNING && media.audio.microphone_muted && !media.audio.capture_indicator_required);
}
static void remote_device_display_progress(remote_device_binding_job_t *job)
{
    bool sleep = job->request.operation == CP_OP_DISPLAY_SLEEP;
    if (sleep && !job->action_started) {
        /* Unknown/active capture never loses its warning before off ACK. */
        if (!remote_device_microphone_safe()) return;
        if (job->cancelling) {
            job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true}; return;
        }
        if (!device_idle_request_sleep(&s_idle, s_now_ms)) { remote_device_failed(job, CP_LOCAL_BUSY, false); return; }
        s_brightness_requested = 0;
        if (board_brightness_set(0) != ESP_OK) { remote_device_failed(job, CP_UNAVAILABLE, false); return; }
        job->action_started = true; ++s_ui_revision;
    }
    board_brightness_status_t brightness = board_brightness_status();
    if (brightness.error != ESP_OK) { remote_device_failed(job, CP_UNAVAILABLE, true); return; }
    uint8_t wanted = sleep ? 0 : s_settings.brightness_percent;
    if (!brightness.pending && brightness.applied_known && brightness.applied_percent == wanted && s_idle.asleep == sleep)
        remote_device_applied(job, s_ui_revision, false);
}

#include "app_remote_device_maintenance.inc"
static cp_error_t remote_device_submit_action(remote_device_binding_job_t *job, const app_remote_device_request_t *request)
{
    cp_operation_id_t operation = request->operation;
    if (operation == CP_OP_SYSTEM_RESTART || operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) {
        bool found = false;
        for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
            remote_device_review_t *review = &s_remote_device_reviews[i];
            if (review->used && !strcmp(review->approval.id, request->approval_id) && review->approval.consumed &&
                remote_device_id_equal(review->approval.action_digest, request->action_digest, 32)) {
                job->wifi_revision = review->wifi_revision; job->target_revision = review->target_revision; found = true; break;
            }
        }
        if (!found || s_remote_device_maintenance_revision == UINT64_MAX) return CP_STALE;
        ++s_remote_device_maintenance_revision; s_remote_device_maintenance_active = true;
        cancel_input(); ble_hid_pairing_close(); bt_audio_pairing_close();
        app_media_runtime_stop(AE_STOP_MAINTENANCE); job->stop_requested = true;
        if (operation == CP_OP_SYSTEM_RESTART) { board_haptics_enable(false); remote_device_restart_progress(job); }
        else remote_device_factory_progress(job);
    } else if (request->settings_fields) remote_device_settings_progress(job);
    else if (operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && operation <= CP_OP_BLUETOOTH_TARGET_REQUEST) {
        if (operation == CP_OP_BLUETOOTH_TARGET_REQUEST || operation == CP_OP_BLUETOOTH_BOND_FORGET) {
            ble_hid_target_t target; if (!remote_device_target(request, &target)) return CP_STALE;
        } else if (request->peer_id[0]) {
            ble_hid_bond_list_t bonds = ble_hid_bond_list(); ble_hid_snapshot_t link = ble_hid_snapshot(); bool matched = false;
            for (uint8_t i = 0; i < bonds.count && i < BLE_HID_MAX_BONDS; ++i) {
                char id[65]; remote_device_hex(bonds.bonds[i].bond_id, sizeof(bonds.bonds[i].bond_id), id);
                if (bonds.known && !bonds.updating && link.connected && bonds.current_match_known &&
                    bonds.bonds[i].is_current && bonds.bonds[i].bond_id_known && !strcmp(id, request->peer_id)) matched = true;
            }
            if (!matched) return CP_STALE;
        }
        ble_hid_pairing_close();
        if (operation != CP_OP_BLUETOOTH_PAIRING_CLOSE) cancel_input();
        remote_device_bluetooth_progress(job);
    } else if (operation == CP_OP_IMU_CALIBRATE) {
        cancel_input(); interaction_calibrate(&s_interaction); ++s_ui_revision;
    } else if (operation == CP_OP_DISPLAY_WAKE) {
        (void)device_idle_activity(&s_idle, s_now_ms); s_brightness_requested = (int)s_settings.brightness_percent;
        if (board_brightness_set(s_brightness_requested) != ESP_OK) return CP_UNAVAILABLE;
        job->action_started = true; ++s_ui_revision; remote_device_display_progress(job);
    } else if (operation == CP_OP_DISPLAY_SLEEP) {
        if (!remote_device_microphone_safe()) { app_media_runtime_stop(AE_STOP_PRIVACY); job->stop_requested = true; }
        remote_device_display_progress(job);
    } else if (operation == CP_OP_HAPTICS_TEST || operation == CP_OP_HAPTICS_RECOVER) {
        board_feedback_t pattern = BOARD_FEEDBACK_CONFIRM; uint32_t duration = 24; uint8_t strength = 35;
        if (request->pattern[0] && strcmp(request->pattern, "confirm")) {
            if (!strcmp(request->pattern, "mode")) { pattern = BOARD_FEEDBACK_MODE; duration = 45; strength = 55; }
            else if (!strcmp(request->pattern, "lock")) { pattern = BOARD_FEEDBACK_LOCK; duration = 65; strength = 65; }
            else if (!strcmp(request->pattern, "error")) { pattern = BOARD_FEEDBACK_ERROR; duration = 165; strength = 65; }
            else return CP_NOT_IMPLEMENTED;
        }
        if ((request->duration_provided && request->duration_ms != duration) ||
            (request->strength_provided && request->strength_percent != strength)) return CP_NOT_IMPLEMENTED;
        if (board_haptics_request(operation == CP_OP_HAPTICS_TEST ? BOARD_HAPTIC_TEST : BOARD_HAPTIC_RECOVER,
            pattern, &job->haptic_id) != ESP_OK) return CP_UNAVAILABLE;
        job->action_started = true;
    } else if (operation >= CP_OP_UI_NAVIGATE && operation <= CP_OP_UI_MODE_PANEL_OPEN) {
        if (s_overlay == APP_OVERLAY_CONFIRM || s_overlay == APP_OVERLAY_API_REVIEW || s_overlay == APP_OVERLAY_LOCAL_EDITOR) return CP_FORBIDDEN;
        if (operation == CP_OP_UI_PRIMARY_ACTIVATE) {
            if (!activate_primary(INPUT_REMOTE)) return CP_UNAVAILABLE;
        } else if (operation == CP_OP_UI_CLOSE) open_overlay(APP_OVERLAY_NONE);
        else if (operation == CP_OP_UI_MODE_PANEL_OPEN) open_overlay(APP_OVERLAY_MODES);
        else {
            static const char *const overlays[] = {"none","modes","target","settings","wifi_networks","wifi_network","common_keys"};
            static const app_overlay_t values[] = {APP_OVERLAY_NONE,APP_OVERLAY_MODES,APP_OVERLAY_TARGET,APP_OVERLAY_SETTINGS,
                APP_OVERLAY_WIFI_NETWORKS,APP_OVERLAY_WIFI_NETWORK,APP_OVERLAY_COMMON_KEYS};
            bool found = false;
            for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i) if (!strcmp(request->overlay, overlays[i])) {
                if (values[i] == APP_OVERLAY_WIFI_NETWORK && !s_wifi_selection_generation) return CP_STALE;
                if (request->page[0] && values[i] != APP_OVERLAY_SETTINGS) return CP_INVALID;
                if (request->page[0]) {
                    static const char *const pages[] = {"home","connectivity","display","input","about","bluetooth","wifi"};
                    bool page_found = false;
                    for (size_t p = 0; p < sizeof(pages)/sizeof(pages[0]); ++p) if (!strcmp(request->page,pages[p])) {
                        s_settings_page = (app_settings_page_t)p; page_found = true; break;
                    }
                    if (!page_found) return CP_INVALID;
                }
                open_overlay(values[i]); found = true; break;
            }
            if (!found) return CP_INVALID;
        }
        remote_device_applied(job, s_ui_revision, false);
    } else return CP_NOT_IMPLEMENTED;
    if (s_state_sequence != UINT64_MAX) ++s_state_sequence;
    return CP_OK;
}
static cp_error_t remote_device_submit(void *context, const app_remote_device_request_t *request, app_remote_device_outcome_t *outcome)
{
    (void)context;
    if (!remote_device_session(&request->identity)) return CP_UNAUTHENTICATED;
    remote_device_binding_job_t *job = NULL;
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i) {
        remote_device_binding_job_t *candidate = &s_remote_device_jobs[i];
        if (!candidate->used || candidate->outcome.phase == APP_REMOTE_DEVICE_COMPLETED || candidate->outcome.phase == APP_REMOTE_DEVICE_CANCELLED ||
            candidate->outcome.phase == APP_REMOTE_DEVICE_FAILED) { job = candidate; break; }
    }
    if (!job) return CP_BUSY;
    *job = (remote_device_binding_job_t){.used = true, .request = *request, .before = s_settings,
        .outcome = {.phase = APP_REMOTE_DEVICE_PENDING}};
    cp_error_t error = remote_device_submit_action(job, request);
    if (error != CP_OK) remote_device_failed(job, error, job->action_started || job->name_id || job->transport_done);
    *outcome = job->outcome; return CP_OK;
}

static bool remote_device_poll(void *context, uint64_t token, app_remote_device_outcome_t *outcome)
{
    (void)context;
    remote_device_binding_job_t *job = remote_device_binding(token); if (!job) return false;
    if (job->outcome.phase == APP_REMOTE_DEVICE_COMPLETED || job->outcome.phase == APP_REMOTE_DEVICE_FAILED || job->outcome.phase == APP_REMOTE_DEVICE_CANCELLED) {
        *outcome = job->outcome; return true;
    }
    cp_operation_id_t operation = job->request.operation;
    if (operation == CP_OP_SYSTEM_RESTART) remote_device_restart_progress(job);
    else if (operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) remote_device_factory_progress(job);
    else if (job->request.settings_fields) remote_device_settings_progress(job);
    else if (operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && operation <= CP_OP_BLUETOOTH_TARGET_REQUEST) remote_device_bluetooth_progress(job);
    else if (operation == CP_OP_IMU_CALIBRATE) {
        if (!board_capabilities().imu) remote_device_failed(job, CP_UNAVAILABLE, false);
        else if (!s_interaction.view.calibrating && s_interaction.view.calibrated) remote_device_applied(job, s_ui_revision, false);
    } else if (operation == CP_OP_DISPLAY_WAKE || operation == CP_OP_DISPLAY_SLEEP) remote_device_display_progress(job);
    else if (operation == CP_OP_HAPTICS_TEST || operation == CP_OP_HAPTICS_RECOVER) {
        board_haptic_status_t status = board_haptics_status();
        if (status.request_id != job->haptic_id) remote_device_failed(job, CP_STALE, true);
        else if (status.state == BOARD_HAPTIC_COMPLETED && status.off_confirmed &&
            (operation == CP_OP_HAPTICS_RECOVER || status.on_confirmed)) remote_device_applied(job, status.request_id, false);
        else if (status.state == BOARD_HAPTIC_NO_EFFECT) remote_device_failed(job, CP_BUSY, false);
        else if (status.state == BOARD_HAPTIC_FAILED || status.state == BOARD_HAPTIC_UNCERTAIN) remote_device_failed(job, CP_UNAVAILABLE, status.state == BOARD_HAPTIC_UNCERTAIN);
        else if (status.state == BOARD_HAPTIC_CANCELLED && status.off_confirmed)
            job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true};
    }
    *outcome = job->outcome; return true;
}
static cp_error_t remote_device_cancel(void *context, uint64_t token)
{
    (void)context;
    remote_device_binding_job_t *job = remote_device_binding(token); if (!job) return CP_STALE;
    job->cancelling = true;
    if (job->request.operation == CP_OP_SYSTEM_RESTART) {
        if (job->restart_id && app_device_restart_cancel(job->restart_id) != ESP_OK) return CP_TOO_LATE;
        return CP_OK;
    }
    if (job->request.operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) return job->reset_step ? CP_TOO_LATE : CP_OK;
    if (job->request.operation == CP_OP_HAPTICS_TEST || job->request.operation == CP_OP_HAPTICS_RECOVER)
        return board_haptics_cancel(job->haptic_id) == ESP_OK ? CP_OK : CP_TOO_LATE;
    if (job->request.operation == CP_OP_BLUETOOTH_BOND_FORGET && job->bond_forget_id)
        return ble_hid_forget_bond_cancel(job->bond_forget_id) == ESP_OK ? CP_OK : CP_TOO_LATE;
    if (job->request.operation == CP_OP_IMU_CALIBRATE) {
        interaction_calibration_cancel(&s_interaction);
        job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true}; return CP_OK;
    }
    if (!job->applied && !job->name_id && !job->transport_done && !job->action_started && !job->disconnect_requested && !job->stop_requested) {
        job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true}; return CP_OK;
    }
    return CP_TOO_LATE; /* Never invent rollback or undo a submitted SDK operation. */
}

static bool remote_device_approval(void *context, const char *id, app_remote_device_approval_t *approval)
{
    (void)context;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i)
        if (s_remote_device_reviews[i].used && !strcmp(id, s_remote_device_reviews[i].approval.id)) {
            *approval = s_remote_device_reviews[i].approval; return true;
        }
    return false;
}
static cp_error_t remote_device_prepare_approval(void *context, const app_remote_device_request_t *request,
    app_remote_device_approval_t *approval)
{
    (void)context;
    if (s_overlay == APP_OVERLAY_CONFIRM || s_overlay == APP_OVERLAY_API_REVIEW || s_now_ms > UINT64_MAX - APP_REMOTE_DEVICE_STAGE_MS) return CP_BUSY;
    if (request->operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) {
        char digest[65]; remote_device_manifest_digest(digest);
        if (strcmp(digest, request->retained_manifest_digest)) return CP_STALE;
    }
    remote_device_review_t *review = NULL;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i)
        if (!s_remote_device_reviews[i].used || s_now_ms >= s_remote_device_reviews[i].approval.expires_ms ||
            s_remote_device_reviews[i].approval.cancelled || s_remote_device_reviews[i].approval.consumed) { review = &s_remote_device_reviews[i]; break; }
    if (!review) return CP_BUSY;
    char generated[CP_ID_HEX_CAPACITY] = {0}; bool unique = false;
    for (unsigned attempt = 0; attempt < 4 && !unique; ++attempt) {
        uint8_t id[CP_ID_BYTES];
        if (!remote_device_random(NULL, id, sizeof(id))) return CP_UNAVAILABLE;
        unsigned nonzero = 0; for (size_t i = 0; i < sizeof(id); ++i) nonzero |= id[i];
        if (!nonzero) continue;
        remote_device_hex(id, sizeof(id), generated); unique = true;
        for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i)
            if (s_remote_device_reviews[i].used && !strcmp(generated, s_remote_device_reviews[i].approval.id)) unique = false;
    }
    if (!unique) return CP_UNAVAILABLE;
    cp_scope_mask_t scopes = 0;
    for (size_t i = 0; i < cp_operation_count(); ++i) if (cp_operation_at(i)->id == request->operation) scopes = cp_operation_at(i)->scopes;
    app_wifi_profiles_snapshot_t wifi; app_wifi_profiles_snapshot(&wifi);
    *review = (remote_device_review_t){.used = true, .created_ms = s_now_ms, .request = *request,
        .wifi_revision = wifi.revision, .target_revision = ble_hid_target_snapshot().revision,
        .approval = {.operation = request->operation, .owner = request->identity, .required_scopes = scopes,
            .management_generation = request->management_generation, .resource_revision = request->resource_revision,
            .expires_ms = s_now_ms + APP_REMOTE_DEVICE_STAGE_MS,
            .physical_required = request->operation == CP_OP_BLUETOOTH_PAIRING_OPEN || request->operation >= CP_OP_SYSTEM_RESTART}};
    memcpy(review->approval.id, generated, sizeof(generated)); memcpy(review->approval.action_digest, request->action_digest, 32);
    s_confirmation = APP_CONFIRM_REMOTE;
    s_confirmation_generation = request->bluetooth_generation;
    (void)snprintf(s_remote_device_local_review, sizeof(s_remote_device_local_review), "%s", review->approval.id);
    open_overlay(APP_OVERLAY_CONFIRM); review->ui_revision = s_ui_revision;
    *approval = review->approval; return CP_OK;
}
static cp_error_t remote_device_approval_decide(void *context, const app_remote_device_approval_t *approval, bool confirm)
{
    (void)context;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        remote_device_review_t *review = &s_remote_device_reviews[i];
        if (!review->used || strcmp(approval->id, review->approval.id) ||
            !remote_device_id_equal(approval->action_digest, review->approval.action_digest, 32)) continue;
        if (review->approval.consumed) return CP_TOO_LATE;
        if (confirm && (s_now_ms >= review->approval.expires_ms || review->approval.cancelled ||
            (review->approval.physical_required && !review->approval.physical_satisfied))) return CP_FORBIDDEN;
        if (confirm) review->approval.confirmed = true; else review->approval.cancelled = true;
        if (!strcmp(s_remote_device_local_review, approval->id)) {
            s_remote_device_local_review[0] = 0;
            if (s_overlay == APP_OVERLAY_CONFIRM && s_ui_revision == review->ui_revision) {
                s_confirmation = APP_CONFIRM_NONE; open_overlay(APP_OVERLAY_SETTINGS);
            }
        }
        return CP_OK;
    }
    return CP_STALE;
}
static cp_error_t remote_device_approval_claim(void *context, const app_remote_device_approval_t *approval)
{
    (void)context;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        remote_device_review_t *review = &s_remote_device_reviews[i];
        if (!review->used || strcmp(approval->id, review->approval.id)) continue;
        if (!review->approval.confirmed || review->approval.cancelled || review->approval.consumed || s_now_ms >= review->approval.expires_ms ||
            review->approval.management_generation != s_management_generation ||
            !remote_device_id_equal(approval->action_digest, review->approval.action_digest, 32)) return CP_STALE;
        if (review->request.settings_revision != s_settings_revision) return CP_STALE;
        if (review->request.operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) {
            app_wifi_profiles_snapshot_t wifi; app_wifi_profiles_snapshot(&wifi);
            if (!wifi.known || wifi.uncertain || wifi.operation_pending || wifi.revision != review->wifi_revision ||
                ble_hid_bond_list().revision != review->request.bond_revision ||
                ble_hid_target_snapshot().revision != review->target_revision) return CP_STALE;
        }
        review->approval.consumed = true; return CP_OK;
    }
    return CP_STALE;
}
static void remote_device_review_view(app_remote_review_view_t *view)
{
    memset(view, 0, sizeof(*view));
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        remote_device_review_t *review = &s_remote_device_reviews[i];
        if (!review->used || strcmp(review->approval.id, s_remote_device_local_review)) continue;
        const app_remote_device_approval_t *a = &review->approval;
        view->pending = !a->cancelled && !a->consumed; view->clock_known = true; view->now_ms = s_now_ms;
        view->request.management_generation = a->management_generation; view->request.resource_revision = a->resource_revision;
        view->request.expires_ms = a->expires_ms;
        for (size_t n = 0; n < cp_operation_count(); ++n) if (cp_operation_at(n)->id == a->operation)
            (void)snprintf(view->request.operation, sizeof(view->request.operation), "%s", cp_operation_at(n)->name);
        memcpy(view->request.approval_id, a->id, sizeof(a->id)); remote_device_hex(a->action_digest, sizeof(a->action_digest), view->request.action_digest);
        (void)snprintf(view->reset_group, sizeof(view->reset_group), "%s", review->request.group);
        (void)snprintf(view->retained_manifest_digest, sizeof(view->retained_manifest_digest), "%s", review->request.retained_manifest_digest);
        const char *details = a->operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST ?
            "Reset preferences and LE/Wi-Fi connections. Keep trust, Classic bonds, firmware/OTA, models, recordings, unknown data, NAND and backups. No restart." :
            a->operation == CP_OP_SYSTEM_RESTART ? "Release input, stop media, then restart this device. Saved data is preserved. Reconnect to observe the new boot." :
            a->operation == CP_OP_PREFERENCES_RESET ? "Restore only the named preference group to built-in defaults; wait for live owner and storage acknowledgements. Connections and trust remain saved." :
            a->operation == CP_OP_BLUETOOTH_PAIRING_OPEN ? "Release input and disconnect the current LE host, then open the requested bounded new-device pairing window." :
            a->operation == CP_OP_BLUETOOTH_BOND_FORGET ? "Forget only the exact selected LE bond fingerprint. Other saved bonds and trust remain." :
            "Release input and forget all saved LE HID bonds. Classic audio bonds and trust remain.";
        (void)snprintf(view->details, sizeof(view->details), "%s", details);
        if (a->operation == CP_OP_BLUETOOTH_PAIRING_OPEN)
            (void)snprintf(view->details, sizeof(view->details),
                "Release input; disconnect LE host %s; open new-device pairing for %u seconds. Existing bonds remain. Confirm locally to admit new trust.",
                review->request.peer_id[0] ? review->request.peer_id : "current", (unsigned)review->request.window_seconds);
        else if (a->operation == CP_OP_BLUETOOTH_BOND_FORGET)
            (void)snprintf(view->details, sizeof(view->details),
                "Forget LE bond %s at list revision %" PRIu64 ". Keep other LE bonds, Classic bonds and trust. Do not open pairing.",
                review->request.peer_id, review->request.bond_revision);
        else if (a->operation == CP_OP_BLUETOOTH_BONDS_FORGET_ALL)
            (void)snprintf(view->details, sizeof(view->details),
                "Forget every saved LE HID bond at list revision %" PRIu64 ". Keep Classic audio bonds, Wi-Fi profiles and trust. Do not open pairing.",
                review->request.bond_revision);
        app_remote_device_state_t current;
        view->complete = view->request.operation[0] && view->request.approval_id[0] && view->request.action_digest[0];
        view->can_confirm = view->pending && view->complete && s_now_ms < a->expires_ms && !s_engine.state.locked &&
            remote_device_session(&a->owner) && s_overlay == APP_OVERLAY_CONFIRM && s_ui_revision == review->ui_revision &&
            remote_device_observe(NULL, a->operation, &current) && current.management_generation == a->management_generation &&
            current.resource_revision == a->resource_revision && current.settings_revision == review->request.settings_revision;
        return;
    }
}
/* Only the validated native gesture path can submit this immutable view. */
static bool remote_device_confirm_local(bool confirm, const app_remote_review_request_t *request)
{
    if (!s_remote_device_local_review[0]) return false;
    app_remote_review_view_t view; remote_device_review_view(&view);
    if (!request || !view.pending || (confirm && !view.can_confirm) ||
        s_overlay != APP_OVERLAY_CONFIRM || s_confirmation != APP_CONFIRM_REMOTE ||
        request->management_generation != view.request.management_generation || request->resource_revision != view.request.resource_revision ||
        request->expires_ms != view.request.expires_ms || strcmp(request->operation, view.request.operation) ||
        strcmp(request->approval_id, view.request.approval_id) || strcmp(request->action_digest, view.request.action_digest)) return true;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        remote_device_review_t *review = &s_remote_device_reviews[i];
        if (!review->used || strcmp(review->approval.id, request->approval_id)) continue;
        portENTER_CRITICAL(&s_control_lock);
        bool fresh = s_user_activity_ms > review->created_ms;
        portEXIT_CRITICAL(&s_control_lock);
        if (!fresh) return true;
        review->approval.physical_satisfied = true;
        (void)remote_device_approval_decide(NULL, &review->approval, confirm); return true;
    }
    return true;
}

static cp_error_t remote_device_board_read(void *context, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    (void)context; (void)identity; int used = -1;
    cp_error_t peers = remote_device_peers_read(resource, json, capacity, length);
    if (peers != CP_NOT_IMPLEMENTED) return peers;
    if (!strcmp(resource, "/api/v1/imu")) {
        board_imu_status_t imu = board_imu_status();
        used = snprintf(json, capacity, "{\"resource_revision\":\"%" PRIu64 "\",\"available\":%s,\"calibrating\":%s,\"calibrated\":%s,"
            "\"sample_count\":\"%" PRIu64 "\",\"sample_age_known\":%s,\"sample_age_us\":\"%" PRIu64 "\",\"error\":%d}",
            s_ui_revision, imu.available ? "true":"false", s_interaction.view.calibrating ? "true":"false", s_interaction.view.calibrated ? "true":"false",
            imu.sample_count, imu.sample_age_known ? "true":"false", imu.sample_age_us, imu.error);
    } else if (!strcmp(resource, "/api/v1/display")) {
        board_brightness_status_t brightness = board_brightness_status(); char applied[8];
        if (brightness.applied_known) (void)snprintf(applied, sizeof(applied), "%u", brightness.applied_percent);
        else (void)snprintf(applied, sizeof(applied), "%s", "null");
        used = snprintf(json, capacity, "{\"resource_revision\":\"%" PRIu64 "\",\"awake_requested\":%s,\"brightness_known\":%s,"
            "\"applied_percent\":%s,\"pending\":%s,\"error\":%d}", s_ui_revision, s_idle.asleep ? "false":"true",
            brightness.applied_known ? "true":"false", applied, brightness.pending ? "true":"false", brightness.error);
    } else if (!strcmp(resource, "/api/v1/haptics")) {
        board_haptic_status_t status = board_haptics_status();
        used = snprintf(json, capacity, "{\"resource_revision\":\"%" PRIu64 "\",\"available\":%s,\"enabled\":%s,\"active\":%s,"
            "\"request_state\":%u,\"on_confirmed\":%s,\"off_confirmed\":%s,\"error\":%d}", status.request_id,
            board_capabilities().haptics ? "true":"false", board_haptics_enabled() ? "true":"false", board_haptics_active() ? "true":"false",
            (unsigned)status.state, status.on_confirmed ? "true":"false", status.off_confirmed ? "true":"false", status.error);
    } else return CP_NOT_IMPLEMENTED;
    if (used < 0 || (size_t)used >= capacity) { if (capacity) json[0] = 0; *length = 0; return CP_TOO_LARGE; }
    *length = (size_t)used; return CP_OK;
}

static bool remote_device_initialize(void)
{
    if (s_remote_device) return true;
    remote_device_restart_available = app_device_restart_init() == ESP_OK;
    void *memory = heap_caps_calloc(1, app_remote_device_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return false;
    app_remote_device_port_t port = {.observe = remote_device_observe, .available = remote_device_owner_available,
        .validate_settings = remote_device_validate_settings, .random = remote_device_random,
        .submit = remote_device_submit, .poll = remote_device_poll, .cancel = remote_device_cancel, .complete = remote_complete,
        .approval = remote_device_approval, .approval_prepare = remote_device_prepare_approval,
        .approval_decide = remote_device_approval_decide, .approval_claim = remote_device_approval_claim,
        .read_cached = remote_device_board_read};
    if (app_remote_device_init(memory, app_remote_device_bytes(), &port, &s_remote_device) == CP_OK) return true;
    heap_caps_free(memory); return false;
}
static bool remote_device_available(cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{
    return app_remote_device_available(s_remote_device, operation, reason);
}
static cp_error_t remote_device_execute(const cp_command_t *command, cp_result_t *result)
{
    return app_remote_device_execute(s_remote_device, command, s_now_ms, result);
}
static cp_error_t remote_device_execute_ui(const cp_command_t *command, cp_result_t *result)
{
    return app_remote_device_execute_ui_authorized(s_remote_device, command, s_now_ms, result);
}
static void remote_device_service(void)
{
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        remote_device_review_t *review = &s_remote_device_reviews[i];
        if (!review->used || review->approval.confirmed || review->approval.cancelled || review->approval.consumed) continue;
        if (s_overlay != APP_OVERLAY_CONFIRM || s_ui_revision != review->ui_revision ||
            s_now_ms < review->created_ms || s_now_ms >= review->approval.expires_ms || !remote_device_session(&review->approval.owner))
            (void)remote_device_approval_decide(NULL, &review->approval, false);
    }
    app_remote_device_service(s_remote_device, s_now_ms);
}
static void remote_device_revoke(cp_safety_reason_t reason, const cp_identity_t *identity)
{
    app_remote_device_revoke(s_remote_device, reason, identity, s_now_ms);
}
static cp_error_t remote_device_read(const cp_identity_t *identity, const char *resource, char *json, size_t capacity, size_t *length)
{
    cp_error_t error = app_remote_device_read(s_remote_device, identity, resource, json, capacity, length);
    if (error != CP_OK || strcmp(resource, "/api/v1/config")) return error;
    if (!*length || json[*length - 1U] != '}') return CP_INTERNAL;
    --*length;
    settings_store_status_t storage = settings_store_status();
    if (!remote_device_json(json, capacity, length,
        ",\"runtime_overrides\":%s,\"persistence\":{\"available\":%s,\"loaded\":%s,\"pending\":%s,"
        "\"requested_revision\":\"%" PRIu64 "\",\"saved_revision\":\"%" PRIu64 "\",\"error\":%d}}",
        device_settings_equal(&s_settings, &s_durable_settings) ? "false":"true", storage.available ? "true":"false",
        storage.loaded ? "true":"false", storage.pending ? "true":"false", storage.requested_revision, storage.saved_revision, storage.error)) {
        *length = 0; return CP_TOO_LARGE;
    }
    return CP_OK;
}
