// SPDX-License-Identifier: Apache-2.0
static void reset_open_about(void)
{
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_ABOUT);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_ABOUT);
}
static void local_reset_confirmation_and_stale_input(void)
{
    setup(INPUT_MOUSE);
    action(APP_RESET_REQUEST, 0); assert(s_overlay == APP_OVERLAY_NONE);
    reset_open_about();
    action(APP_RESET_REQUEST, 0);
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_DEVICE_RESET && !test_restart_requests);
    app_intent_t stale = make_intent(APP_CONFIRM, 1, 0, NULL);
    action(APP_CONFIRM, 0);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_ABOUT && !s_local_reset_active);
    action(APP_RESET_REQUEST, 0);
    deliver(stale); assert(!s_local_reset_active && !test_restart_requests);
    s_now_ms += 30000;
    action(APP_CONFIRM, 1);
    assert(!s_local_reset_active && !test_restart_requests && s_confirmation == APP_CONFIRM_NONE);
    action(APP_RESET_REQUEST, 0);
    s_now_ms += 30000; local_reset_service();
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_confirmation == APP_CONFIRM_NONE);
    action(APP_RESET_REQUEST, 0);
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock)); (void)process_priority();
    action(APP_CONFIRM, 1);
    assert(!s_local_reset_active && !test_restart_requests);
}
static void local_reset_waits_for_stop_and_blocks_repopulation(void)
{
    setup(INPUT_MOUSE); update_test_allow();
    reset_open_about(); action(APP_RESET_REQUEST, 0); action(APP_CONFIRM, 1);
    assert(s_local_reset_active && s_overlay == APP_OVERLAY_RESET && local_access() == APP_LOCAL_PAUSED);
    assert(test_media_stops == 1 && s_engine.state.release_pending);
    test_restart_error = ESP_OK;
    local_reset_service(); assert(!test_restart_requests);
    release_barrier();
    test_media_snapshot.audio.hardware_off_confirmed = false;
    local_reset_service(); assert(!test_restart_requests);
    test_media_snapshot.audio.hardware_off_confirmed = true;
    local_reset_service();
    assert(test_restart_requests == 1 && test_restart_authorized && test_restart_maintenance);
    cp_command_t claim = update_test_claim(1234);
    app_update_authority_t authority;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_BUSY && !authority.authorized);
    action(APP_MENU, APP_OVERLAY_MODES); action(APP_HAPTICS, 1);
    assert(s_overlay == APP_OVERLAY_RESET && s_local_reset_active);
    test_restart_status.state = APP_DEVICE_RESTART_RESET_FAILED;
    local_reset_service();
    assert(s_local_reset_failed && local_access() == APP_LOCAL_PAUSED && strstr(s_local_reset_message, "could not finish"));
    s_now_ms += 10000; local_reset_service(); assert(s_local_reset_active && test_restart_requests == 1);
}
static void local_reset_cancellation_and_update_conflict(void)
{
    setup(INPUT_MOUSE); update_test_allow();
    reset_open_about(); action(APP_RESET_REQUEST, 0);
    /* Claim won the serialized update lane before the local final tap. */
    s_update_gate.active = true;
    action(APP_CONFIRM, 1);
    assert(!s_local_reset_active && !s_update_gate.local_reset && !test_restart_requests);
    s_update_gate.active = false;
    action(APP_SETTINGS_PAGE, APP_SETTINGS_ABOUT);
    action(APP_RESET_REQUEST, 0); action(APP_CONFIRM, 1);
    s_now_ms += 5000; local_reset_service();
    assert(!s_local_reset_active && local_access() == APP_LOCAL_NORMAL && !test_restart_requests);
    assert(s_overlay == APP_OVERLAY_RESET);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_ABOUT);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_ABOUT);
    setup(INPUT_MOUSE); update_test_allow();
    reset_open_about(); action(APP_RESET_REQUEST, 0); action(APP_CONFIRM, 1);
    release_barrier(); test_restart_error = ESP_OK; local_reset_service();
    test_restart_status.state = APP_DEVICE_RESTART_CANCELLED;
    local_reset_service();
    assert(!s_local_reset_active && !s_update_gate.local_reset && local_access() == APP_LOCAL_NORMAL);
}
