// SPDX-License-Identifier: Apache-2.0
/* Include after native helpers and before app_remote_input_bindings.inc.
 * Parent wires .update_authority=remote_update_authority, calls
 * remote_update_service() each coordinator turn, and supplies
 * remote_update_maintenance() to local/remote admission and media environment.
 * These functions never run an update, flash operation, network or LVGL call. */
static portMUX_TYPE s_update_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    cp_command_t command;
    bool active, pending, main_unlocked, maintenance_ready;
    uint64_t cached_ms, admitted_until_ms;
    uint32_t rejected;
} s_update_gate;
static bool s_update_maintenance;
static uint64_t s_update_health_since;

static bool remote_update_same_claim(const cp_command_t *a, const cp_command_t *b)
{
    return a->command_key == b->command_key && a->operation == b->operation &&
           a->deadline_ms == b->deadline_ms &&
           a->management_generation == b->management_generation &&
           a->resource_revision == b->resource_revision &&
           !memcmp(a->identity.principal_id, b->identity.principal_id, CP_ID_BYTES) &&
           !memcmp(a->identity.session_id, b->identity.session_id, CP_ID_BYTES) &&
           a->identity.scopes == b->identity.scopes && !strcmp(a->args_json, b->args_json);
}
/* Management thread: only copied trusted policy/session/clock snapshots and
 * this bounded mailbox. No coordinator-owned main state is read off-thread. */
static cp_error_t remote_update_authority(void *context, const cp_command_t *command,
                                          app_update_authority_t *out)
{
    (void)context;
    if (!command || !out) {
        return CP_INVALID;
    }
    cp_error_t error = app_control_runtime_update_authority(command, out);
    out->observation.runtime_status_known = true;
    out->observation.runtime_status = (int32_t)error;
    out->observation.observed_ms = (uint64_t)esp_timer_get_time() / 1000U;
    if (error != CP_OK) {
        return error;
    }
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    out->observation.observed_ms = now;
    portENTER_CRITICAL(&s_update_gate_lock);
    bool fresh = now >= s_update_gate.cached_ms && now - s_update_gate.cached_ms <= 250;
    out->observation.owner_observed = true;
    out->observation.owner_cached_ms = s_update_gate.cached_ms;
    out->observation.owner_rejected = s_update_gate.rejected;
    if (!fresh) out->observation.owner_rejected |= APP_UPDATE_OWNER_STALE;
    if (s_update_gate.active && !remote_update_same_claim(&s_update_gate.command, command)) {
        error = CP_BUSY;
        out->observation.owner_rejected |= APP_UPDATE_OWNER_CLAIM;
    }
    if (!s_update_gate.active && error == CP_OK) {
        if (now > command->deadline_ms) {
            error = CP_EXPIRED;
            out->observation.owner_rejected |= APP_UPDATE_OWNER_EXPIRED;
        } else {
            s_update_gate.command = *command;
            s_update_gate.active = true;
            s_update_gate.pending = true;
            s_update_gate.maintenance_ready = false;
            s_update_gate.admitted_until_ms = out->authority_expires_ms;
        }
    }
    out->unlocked = fresh && s_update_gate.main_unlocked;
    out->maintenance_ready = fresh && s_update_gate.maintenance_ready && s_update_gate.active;
    if (out->authority_expires_ms > s_update_gate.admitted_until_ms) {
        out->authority_expires_ms = s_update_gate.admitted_until_ms;
    }
    if (error != CP_OK || !fresh || now >= out->authority_expires_ms) {
        out->authorized = false;
    }
    if (!s_update_gate.main_unlocked) out->observation.owner_rejected |= APP_UPDATE_OWNER_LOCKED;
    if (!out->maintenance_ready) out->observation.owner_rejected |= APP_UPDATE_OWNER_NOT_READY;
    if (now >= out->authority_expires_ms) out->observation.owner_rejected |= APP_UPDATE_OWNER_EXPIRED;
    portEXIT_CRITICAL(&s_update_gate_lock);
    return error;
}
/* Coordinator only. Parent uses this gate for all ordinary input, setting,
 * bond/profile writes and media admission; safety stop/lock/query stay usable. */
static bool remote_update_maintenance(void)
{
    return s_update_maintenance || remote_device_maintenance();
}
static bool remote_update_media_quiet(const app_media_snapshot_t *media)
{
    if (!media->initialized || !media->audio_known || media->busy) {
        return false;
    }
    const ae_snapshot_t *audio = &media->audio;
    if (!audio->hardware_off_confirmed || audio->capture_gate_open || audio->playback_gate_open ||
        audio->microphone_enabled || audio->speaker_enabled || audio->capture_indicator_required ||
        audio->quarantined) {
        return false;
    }
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) {
        if (audio->capture[i].cleanup_pending || audio->capture[i].state == AE_JOB_STARTING ||
            audio->capture[i].state == AE_JOB_RUNNING ||
            audio->capture[i].state == AE_JOB_STOPPING ||
            audio->capture[i].state == AE_JOB_FINALIZING) {
            return false;
        }
    }
    return !audio->playback.cleanup_pending && audio->playback.state != AE_JOB_STARTING &&
           audio->playback.state != AE_JOB_RUNNING && audio->playback.state != AE_JOB_STOPPING &&
           audio->playback.state != AE_JOB_FINALIZING;
}
static void remote_update_health_service(void)
{
    app_update_snapshot_t update;
    app_update_snapshot(&update);
    if (update.state != APP_UPDATE_PENDING_HEALTH) {
        s_update_health_since = 0;
        return;
    }
    board_capabilities_t capabilities = board_capabilities();
    board_imu_status_t imu = board_imu_status();
    app_presenter_status_t ui = app_presenter_status();
    app_presenter_indicator_status_t renderer = app_presenter_indicator_status();
    settings_store_status_t settings = settings_store_status();
    static EXT_RAM_BSS_ATTR app_media_snapshot_t media;
    memset(&media, 0, sizeof(media));
    bool media_known = app_media_runtime_snapshot(&media);
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    bool sensors =
        (!capabilities.imu ||
         (imu.initialization_succeeded && imu.start_succeeded && imu.worker_running &&
          imu.sample_valid && imu.sample_fresh && !imu.fault_latched && imu.error == ESP_OK));
    bool healthy = capabilities.display && capabilities.touch && capabilities.lock_button &&
                   s_multitouch_available && ui.initialized && ui.init_error == ESP_OK &&
                   ui.has_applied && ui.apply_age_ms <= 500 && renderer.bound &&
                   renderer.owner_alive && renderer.error == ESP_OK &&
                   board_display_get() != NULL && settings.available && !settings.pending &&
                   settings.error == ESP_OK && device_settings_validate(&s_settings) && sensors &&
                   bluetooth.initialized && !bluetooth.failed && usb.initialized && !usb.failed &&
                   media_known && remote_update_media_quiet(&media);
    /* A live local coordinator, renderer and actual manager observations must
     * remain healthy for 45 seconds. No host connection, pairing or Internet
     * is a health gate; no queued render or accepted stop substitutes for ACK. */
    if (!healthy || s_now_ms < s_update_health_since) {
        s_update_health_since = 0;
    } else if (!s_update_health_since) {
        s_update_health_since = s_now_ms;
    } else if (s_now_ms - s_update_health_since >= 45000) {
        (void)app_update_confirm_health(true);
    }
}
/* Keep maintenance-only temporaries out of the loop's permanent frame. */
static void __attribute__((noinline)) remote_update_service(void)
{
    app_update_snapshot_t update;
    app_update_snapshot(&update);
    portENTER_CRITICAL(&s_update_gate_lock);
    bool active = s_update_gate.active, pending = s_update_gate.pending;
    /* Coordinator-owned copy, separate from the management-thread mailbox. */
    static EXT_RAM_BSS_ATTR cp_command_t command;
    command = s_update_gate.command;
    s_update_gate.pending = false;
    portEXIT_CRITICAL(&s_update_gate_lock);
    bool terminal = update.state == APP_UPDATE_STAGED || update.state == APP_UPDATE_CANCELLED ||
                    update.state == APP_UPDATE_ERROR || update.state == APP_UPDATE_UNAVAILABLE ||
                    update.state == APP_UPDATE_CONFIRMED;
    /* STAGED is terminal only for download; an activation arriving from STAGED
     * still needs a new actual maintenance barrier before boot selection. */
    if (active && terminal && !pending) {
        if (!(command.operation == CP_OP_FIRMWARE_ACTIVATE && update.state == APP_UPDATE_STAGED)) {
            portENTER_CRITICAL(&s_update_gate_lock);
            s_update_gate.active = false;
            s_update_gate.maintenance_ready = false;
            memset(&s_update_gate.command, 0, sizeof(s_update_gate.command));
            portEXIT_CRITICAL(&s_update_gate_lock);
            active = false;
        }
    }
    bool requested = active || update.state == APP_UPDATE_PENDING_HEALTH || app_update_flash_busy();
    if (requested && !s_update_maintenance) {
        cancel_local_editor();
        remote_local_override(false);
        hid_engine_cancel(&s_engine);
        s_drag_requested = false;
        discard_intents();
        app_media_runtime_stop(AE_STOP_MAINTENANCE);
        ble_hid_pairing_close();
    }
    s_update_maintenance = requested;
    app_wifi_set_maintenance(requested);
    bool ready = false;
    uint32_t rejected = 0;
    if (active && requested) {
        app_update_authority_t authority = {0};
        cp_error_t allowed = app_control_runtime_update_authority(&command, &authority);
        static EXT_RAM_BSS_ATTR app_media_snapshot_t media;
        memset(&media, 0, sizeof(media));
        bool media_known = app_media_runtime_snapshot(&media);
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
        input_link_t link = input_link_snapshot(&bluetooth);
        usb_hid_snapshot_t usb = usb_hid_snapshot();
        settings_store_status_t settings = settings_store_status();
        app_wifi_snapshot_t wifi;
        app_wifi_get_snapshot(&wifi);
        app_wifi_scan_snapshot_t scan;
        app_wifi_get_scan_snapshot(&scan);
        ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
        static EXT_RAM_BSS_ATTR ble_hid_bond_list_t bonds;
        ble_hid_copy_bond_list(&bonds);
        ble_hid_target_snapshot_t target = ble_hid_target_snapshot();
        bool neutral =
            !s_engine.count && !s_engine.mouse_buttons &&
            (!link.connected || (!s_engine.state.release_pending && !link.congested &&
                                 (link.kind != APP_INPUT_USB || usb.submitted == usb.completed)));
        bool flash_quiet =
            settings.available && !settings.pending && !wifi.operation_pending && !wifi.cleanup_pending &&
            !scan.cleanup_pending && scan.state != APP_WIFI_SCAN_RUNNING &&
            scan.state != APP_WIFI_SCAN_QUEUED && scan.state != APP_WIFI_SCAN_CANCELLING &&
            name.status != BLE_HID_NAME_PENDING && name.status != BLE_HID_NAME_APPLYING &&
            bonds.known && bonds.error == ESP_OK && !bonds.updating && target.loaded &&
            target.error == ESP_OK && target.request_status != BLE_HID_TARGET_PENDING &&
            target.request_status != BLE_HID_TARGET_APPLYING && target.request_status != BLE_HID_TARGET_UNCERTAIN &&
            !bluetooth.pairing_open &&
            bluetooth.bond_forget_state != BLE_HID_FORGET_RUNNING;
        ready = allowed == CP_OK && !s_engine.state.locked &&
                s_management_generation == command.management_generation && neutral &&
                !s_transport_switch_pending && s_pair_operation == APP_CONFIRM_NONE &&
                media_known && remote_update_media_quiet(&media) && flash_quiet;
        if (allowed != CP_OK) rejected |= APP_UPDATE_OWNER_POLICY;
        if (s_engine.state.locked) rejected |= APP_UPDATE_OWNER_LOCKED;
        if (s_management_generation != command.management_generation) rejected |= APP_UPDATE_OWNER_GENERATION;
        if (!neutral) rejected |= APP_UPDATE_OWNER_HID;
        if (s_transport_switch_pending) rejected |= APP_UPDATE_OWNER_TRANSPORT;
        if (s_pair_operation != APP_CONFIRM_NONE) rejected |= APP_UPDATE_OWNER_PAIRING;
        if (!media_known) rejected |= APP_UPDATE_OWNER_MEDIA_UNKNOWN;
        else if (!remote_update_media_quiet(&media)) rejected |= APP_UPDATE_OWNER_MEDIA;
        if (!flash_quiet) rejected |= APP_UPDATE_OWNER_FLASH;
    }
    portENTER_CRITICAL(&s_update_gate_lock);
    s_update_gate.main_unlocked = !s_engine.state.locked;
    s_update_gate.cached_ms = s_now_ms;
    s_update_gate.maintenance_ready = ready && s_update_gate.active;
    s_update_gate.rejected = rejected;
    portEXIT_CRITICAL(&s_update_gate_lock);
    clear_private(&command, sizeof(command));
    remote_update_health_service();
}
