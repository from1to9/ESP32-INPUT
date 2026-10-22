// SPDX-License-Identifier: Apache-2.0
/* Explicitly reviewed maintenance. Each step consumes its owning manager's
 * actual acknowledgement; no namespace/partition/raw-device erase exists here. */
static bool remote_device_neutral(void)
{
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot(); input_link_t link = input_link_snapshot(&bluetooth);
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    return !s_engine.count && !s_engine.mouse_buttons &&
        (!link.connected || (!s_engine.state.release_pending && !link.congested &&
            (link.kind != APP_INPUT_USB || usb.submitted == usb.completed)));
}
static bool remote_device_media_off(void)
{
    static EXT_RAM_BSS_ATTR app_media_snapshot_t media;
    return app_media_runtime_snapshot(&media) && remote_update_media_quiet(&media);
}
static void remote_device_maintenance_cancelled(remote_device_binding_job_t *job)
{
    job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true};
    s_remote_device_maintenance_active = false;
}
static void remote_device_restart_progress(remote_device_binding_job_t *job)
{
    bool authorized = !job->cancelling && !s_engine.state.locked && remote_device_session(&job->request.identity) &&
        s_management_generation == job->request.management_generation && s_now_ms < job->request.deadline_ms;
    bool ready = remote_device_neutral() && remote_device_media_off() && !board_haptics_active() && !settings_store_status().pending;
    if (!job->restart_id) {
        if (!authorized) { remote_device_maintenance_cancelled(job); return; }
        if (!ready) return;
        if (app_device_restart_request(job->request.deadline_ms, &job->restart_id) != ESP_OK) {
            remote_device_failed(job, CP_UNAVAILABLE, false); s_remote_device_maintenance_active = false; return;
        }
    }
    app_device_restart_authorize(job->restart_id, authorized, ready);
    app_device_restart_status_t status = app_device_restart_status();
    if (status.request_id != job->restart_id) { remote_device_failed(job, CP_STALE, true); return; }
    if (status.state == APP_DEVICE_RESTART_CANCELLED) remote_device_maintenance_cancelled(job);
    else if (status.state == APP_DEVICE_RESTART_UNCERTAIN) remote_device_failed(job, CP_TOO_LATE, true);
    else job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_RESTART_PENDING,
        .resource_revision = s_remote_device_maintenance_revision, .irreversible = status.state == APP_DEVICE_RESTART_DISPATCHED};
}
static void remote_device_factory_progress(remote_device_binding_job_t *job)
{
    if (job->cancelling && job->reset_step == 0) {
        if (remote_device_neutral() && remote_device_media_off()) remote_device_maintenance_cancelled(job);
        return;
    }
    if (job->cancelling && job->reset_step > 0 && !(job->reset_step == 4 && job->wifi_id)) {
        remote_device_failed(job, CP_TOO_LATE, true); return; /* A partial reset is never atomic rollback. */
    }
    if ((!remote_device_session(&job->request.identity) || s_engine.state.locked) && !(job->reset_step == 4 && job->wifi_id)) {
        job->cancelling = true; return;
    }
    if (job->reset_step == 0) {
        if (!remote_device_neutral() || !remote_device_media_off()) return;
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
        if (bluetooth.connected) {
            if (!job->disconnect_requested) {
                if (bluetooth.generation != job->request.bluetooth_generation || !ble_hid_disconnect_generation(bluetooth.generation)) {
                    remote_device_failed(job, CP_STALE, false); s_remote_device_maintenance_active = false; return;
                }
                job->disconnect_requested = true;
            }
            return;
        }
        job->reset_step = 1;
    }
    if (job->reset_step == 1) {
        remote_device_settings_progress(job);
        if (job->outcome.phase != APP_REMOTE_DEVICE_COMPLETED) return;
        job->outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_PENDING};
        job->reset_step = 2;
    }
    if (job->reset_step == 2) {
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
        static EXT_RAM_BSS_ATTR ble_hid_bond_list_t bonds;
        ble_hid_copy_bond_list(&bonds);
        if (!job->action_started) {
            if (!bonds.known || bonds.updating || bonds.revision != job->request.bond_revision || bluetooth.connected ||
                !ble_hid_forget_bonds_without_pairing(bluetooth.generation)) { remote_device_failed(job, CP_STALE, true); return; }
            job->action_started = true; return;
        }
        if (bluetooth.bond_forget_state == BLE_HID_FORGET_FAILED) { remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return; }
        if (bluetooth.bond_forget_state != BLE_HID_FORGET_DONE || !bonds.known || bonds.count || bonds.updating) return;
        job->reset_step = 3;
    }
    if (job->reset_step == 3) {
        ble_hid_target_snapshot_t target = ble_hid_target_snapshot();
        static EXT_RAM_BSS_ATTR ble_hid_bond_list_t bonds;
        ble_hid_copy_bond_list(&bonds);
        if (!job->target_id) {
            static EXT_RAM_BSS_ATTR ble_hid_target_preferences_request_t request;
            memset(&request, 0, sizeof(request));
            request.expected_bond_revision = bonds.revision;
            request.expected_revision = job->target_revision;
            /* Deleting the bonds makes a durable selected key unavailable.
             * Clearing that orphan needs store health, not selectability. A
             * successfully loaded absent record is also a known empty default. */
            bool admitted = bonds.known && !bonds.updating && bonds.error == ESP_OK && !bonds.count &&
                remote_device_target_store_ready(&target) && target.revision == job->target_revision &&
                ble_hid_target_preferences_request(&request, &job->target_id) == ESP_OK;
            /* The owner copied the request before return; retain no scratch. */
            clear_private(&request, sizeof(request));
            if (!admitted) {
                remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return;
            }
            return;
        }
        if (target.request_id != job->target_id || target.request_status == BLE_HID_TARGET_FAILED || target.request_status == BLE_HID_TARGET_UNCERTAIN) {
            remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return;
        }
        if (target.request_status != BLE_HID_TARGET_APPLIED || target.selected || !target.durable) return;
        job->reset_step = 4;
    }
    if (job->reset_step == 4) {
        static EXT_RAM_BSS_ATTR app_wifi_profiles_snapshot_t wifi;
        app_wifi_profiles_snapshot(&wifi);
        if (!job->wifi_id) {
            if (!wifi.known || wifi.uncertain || wifi.revision != job->wifi_revision ||
                app_wifi_profiles_forget_all_request(job->wifi_revision, false, &job->wifi_id) != ESP_OK) {
                remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return;
            }
            return;
        }
        if (wifi.request_id != job->wifi_id || wifi.uncertain ||
            (wifi.completed_request_id == job->wifi_id && wifi.operation_error != ESP_OK)) {
            remote_device_failed(job, CP_STORAGE_UNCERTAIN, true); return;
        }
        if (wifi.operation_pending || wifi.completed_request_id != job->wifi_id || !wifi.known || wifi.count) return;
        job->reset_step = 5;
        remote_device_applied(job, s_remote_device_maintenance_revision, true);
        s_remote_device_maintenance_active = false;
    }
}
