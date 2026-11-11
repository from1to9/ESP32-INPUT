// SPDX-License-Identifier: Apache-2.0
/* Coordinator-owned local reset. Reuse restart's input/media acknowledgements;
 * erasure runs only on the next boot, before any user-store writers start. */
static uint64_t s_local_reset_review_until, s_local_reset_deadline, s_local_reset_restart_id;
static bool s_local_reset_failed;

static void local_reset_finish_cancelled(const char *message)
{
    app_local_access_t previous = local_access();
    s_local_reset_active = false;
    portENTER_CRITICAL(&s_update_gate_lock);
    s_update_gate.local_reset = false;
    portEXIT_CRITICAL(&s_update_gate_lock);
    button_access_changed(previous);
    board_haptics_enable(s_settings.haptics_enabled);
    s_settings_page = APP_SETTINGS_ABOUT;
    open_overlay(APP_OVERLAY_RESET);
    s_local_reset_message = message;
}
static void local_reset_begin(void)
{
    /* Serialize with the update worker's claim admission. A queued update is
     * already busy even before the coordinator has rendered its maintenance. */
    portENTER_CRITICAL(&s_update_gate_lock);
    bool busy = s_update_gate.active;
    if (!busy) s_update_gate.local_reset = true;
    portEXIT_CRITICAL(&s_update_gate_lock);
    if (busy || app_update_flash_busy()) {
        portENTER_CRITICAL(&s_update_gate_lock);
        s_update_gate.local_reset = false;
        portEXIT_CRITICAL(&s_update_gate_lock);
        open_overlay(APP_OVERLAY_RESET);
        s_local_reset_message = "Finish the firmware update before resetting. No data was cleared.";
        return;
    }
    app_local_access_t previous = local_access();
    s_local_reset_active = true;
    s_local_reset_failed = false;
    s_local_reset_restart_id = 0;
    s_local_reset_deadline = s_now_ms + 5000;
    app_media_controls_revoke_devices();
    app_media_controls_pause_boot();
    button_access_changed(previous);
    remote_local_override(false);
    cancel_input();
    discard_intents();
    ble_hid_pairing_close();
    bt_audio_pairing_close();
    app_media_runtime_stop(AE_STOP_MAINTENANCE);
    board_haptics_enable(false);
    open_overlay(APP_OVERLAY_RESET);
    s_local_reset_message = "Stopping input and audio before reset...";
}
static bool local_reset_handle(const app_intent_t *intent)
{
    if (intent->action == APP_RESET_REQUEST) {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_ABOUT) return true;
        s_local_reset_review_until = s_now_ms + 30000;
        s_confirmation = APP_CONFIRM_DEVICE_RESET;
        open_overlay(APP_OVERLAY_CONFIRM);
        return true;
    }
    if (intent->action != APP_CONFIRM || s_confirmation != APP_CONFIRM_DEVICE_RESET) return false;
    if (intent->x != 0 && intent->x != 1) return true;
    bool approved = intent->x == 1 && s_now_ms < s_local_reset_review_until &&
        s_local_reset_review_until - s_now_ms <= 30000;
    s_confirmation = APP_CONFIRM_NONE;
    s_settings_page = APP_SETTINGS_ABOUT;
    open_overlay(APP_OVERLAY_SETTINGS);
    if (approved) local_reset_begin();
    else s_message = intent->x ? "Reset confirmation expired. No data was cleared." : "Reset cancelled.";
    return true;
}
static void local_reset_service(void)
{
    if (s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_DEVICE_RESET &&
        (s_now_ms >= s_local_reset_review_until || s_local_reset_review_until - s_now_ms > 30000)) {
        s_confirmation = APP_CONFIRM_NONE;
        s_settings_page = APP_SETTINGS_ABOUT;
        open_overlay(APP_OVERLAY_SETTINGS);
        s_message = "Reset confirmation expired. No data was cleared.";
    }
    if (!s_local_reset_active) return;
    if (s_local_reset_failed) {
        s_local_reset_message = "Reset could not finish. Restart the device to check and continue.";
        return;
    }
    bool authorized = !s_engine.state.locked && s_now_ms < s_local_reset_deadline &&
        s_local_reset_deadline - s_now_ms <= 5000;
    bool ready = remote_device_neutral() && remote_device_media_off() && !board_haptics_active();
    if (!s_local_reset_restart_id) {
        if (!authorized) { local_reset_finish_cancelled("Reset stopped before erasure. No data was cleared."); return; }
        if (!ready) return;
        esp_err_t e = app_device_restart_init();
        if (e == ESP_OK) e = app_device_restart_request_local_reset(s_local_reset_deadline, &s_local_reset_restart_id);
        if (e != ESP_OK) { local_reset_finish_cancelled("Reset could not start. No data was cleared."); return; }
    }
    app_device_restart_authorize(s_local_reset_restart_id, authorized, ready);
    app_device_restart_status_t status = app_device_restart_status();
    if (status.request_id != s_local_reset_restart_id || status.state == APP_DEVICE_RESTART_UNCERTAIN ||
        status.state == APP_DEVICE_RESTART_RESET_FAILED) {
        s_local_reset_failed = true;
        s_local_reset_message = "Reset could not finish. Restart the device to check and continue.";
    } else if (status.state == APP_DEVICE_RESTART_CANCELLED) {
        local_reset_finish_cancelled("Reset stopped before erasure. No data was cleared.");
    } else {
        s_local_reset_message = "Restarting to clear saved data. Keep the device connected to power.";
    }
}
