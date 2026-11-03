// SPDX-License-Identifier: Apache-2.0
/* Actual coordinator/device adapter, substituting only existing owning manager
 * boundaries. Completion events are delivered independently of admission. */
static void remote_device_case_setup(void)
{
    setup(INPUT_MOUSE); test_control_session_alive = true; test_restart_error = ESP_OK;
    test_storage.loaded = true;
    test_bonds = (ble_hid_bond_list_t){.known = true, .revision = 31, .capacity = 32};
    test_wifi_profiles = (app_wifi_profiles_snapshot_t){.known = true, .revision = 41, .capacity = 8};
    test_update_target = (ble_hid_target_snapshot_t){.available = true, .loaded = true, .durable = true, .revision = 51};
    assert(remote_device_initialize());
}
static cp_command_t remote_device_case_command(cp_operation_id_t operation, uint64_t key, const char *arguments)
{
    app_remote_device_state_t state; assert(remote_device_observe(NULL, operation, &state));
    cp_command_t command = {.version = CP_API_VERSION, .operation = operation, .command_key = key,
        .deadline_ms = s_now_ms + 1000, .management_generation = s_management_generation,
        .resource_revision = state.resource_revision, .ui_revision = s_ui_revision,
        .identity = {.principal_id = {11}, .session_id = {12}, .scopes = CP_SCOPE_ALL}};
    assert(strlen(arguments) < sizeof(command.args_json)); memcpy(command.args_json, arguments, strlen(arguments) + 1);
    return command;
}
static cp_result_t remote_device_case_execute(cp_operation_id_t operation, uint64_t key, const char *arguments)
{
    cp_command_t command = remote_device_case_command(operation, key, arguments); cp_result_t result;
    cp_error_t error = remote_device_execute(&command, &result);
    if (error != CP_OK) fprintf(stderr,"%s: operation=%u key=%llu error=%s reason=%s\n",test_case,
        (unsigned)operation,(unsigned long long)key,cp_error_name(error),result.reason);
    assert(error == CP_OK); return result;
}
static void remote_device_case_tick(void) { ++s_now_ms; remote_device_service(); }
static void remote_device_case_off(void)
{
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true,
        .audio = {.state = AE_OFF, .hardware_off_confirmed = true, .microphone_muted = true, .speaker_muted = true}};
}
static void remote_device_case_confirm(void)
{
    app_remote_review_view_t view; remote_device_review_view(&view); assert(view.pending && view.can_confirm);
    s_user_activity_ms = ++s_now_ms;
    assert(remote_device_confirm_local(true, &view.request)); remote_device_case_tick();
}
static void remote_device_runtime_settings_and_durability_use_real_boundaries(void)
{
    remote_device_case_setup();
    cp_result_t stage = remote_device_case_execute(CP_OP_CONFIG_STAGE, 100,
        "{\"patch\":{\"display.brightness_percent\":80},\"durable\":false}");
    assert(!test_storage_requests && !test_brightness_count && s_settings.brightness_percent == 50);
    char args[80]; (void)snprintf(args, sizeof(args), "{\"transaction_id\":\"%s\"}", stage.object_id);
    cp_result_t result = remote_device_case_execute(CP_OP_CONFIG_APPLY, 101, args);
    assert(result.state == CP_RESULT_RUNNING && s_settings.brightness_percent == 80 && !test_storage_requests);
    assert(test_brightness_count == 1 && test_brightness_requests[0] == 80 && s_durable_settings.brightness_percent == 50);
    remote_device_case_tick(); assert(!test_control_completions);
    brightness_applied(80); remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_APPLIED && !test_storage_requests && s_durable_settings.brightness_percent == 50);
    device_settings_t native = s_settings; native.theme = DEVICE_THEME_LIGHT; assert(update_settings(&native));
    assert(test_saved_request.theme == DEVICE_THEME_LIGHT && test_saved_request.brightness_percent == 50);
}
static void remote_device_haptic_jobs_wait_for_on_and_final_off(void)
{
    remote_device_case_setup(); test_remote_haptic_accept = true;
    cp_result_t result = remote_device_case_execute(CP_OP_HAPTICS_TEST, 200, "{\"pattern\":\"error\"}");
    assert(result.state == CP_RESULT_RUNNING && test_remote_haptic_calls == 1 && test_haptic_status.pattern == BOARD_FEEDBACK_ERROR);
    remote_device_case_tick(); assert(!test_control_completions);
    test_haptic_status.state = BOARD_HAPTIC_ACTIVE; test_haptic_status.on_confirmed = true;
    remote_device_case_tick(); assert(!test_control_completions);
    test_haptic_status.state = BOARD_HAPTIC_COMPLETED; test_haptic_status.off_confirmed = false;
    remote_device_case_tick(); assert(!test_control_completions);
    test_haptic_status.off_confirmed = true; remote_device_case_tick(); assert(test_control_result.state == CP_RESULT_COMPLETED);
    (void)remote_device_case_execute(CP_OP_HAPTICS_TEST, 201, "{}");
    remote_device_revoke(CP_SAFETY_SESSION_LOST, NULL); remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_CANCELLED && test_haptic_status.off_confirmed);
}
static void remote_device_sleep_preserves_capture_warning_until_off(void)
{
    remote_device_case_setup();
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true,
        .audio = {.state = AE_RUNNING, .microphone_enabled = true, .capture_gate_open = true, .capture_indicator_required = true}};
    cp_result_t result = remote_device_case_execute(CP_OP_DISPLAY_SLEEP, 300, "{}");
    assert(result.state == CP_RESULT_RUNNING && test_media_stops == 1 && !test_brightness_count && !s_idle.asleep);
    remote_device_case_tick(); assert(!test_brightness_count);
    remote_device_case_off(); remote_device_case_tick();
    assert(s_idle.asleep && test_brightness_count == 1 && test_brightness_requests[0] == 0 && !test_control_completions);
    brightness_applied(0); remote_device_case_tick(); assert(test_control_result.state == CP_RESULT_COMPLETED);
    remote_device_case_setup(); test_media_snapshot.audio_known = false;
    (void)remote_device_case_execute(CP_OP_DISPLAY_SLEEP, 301, "{}");
    remote_device_revoke(CP_SAFETY_SESSION_LOST, NULL); remote_device_case_off(); remote_device_case_tick();
    assert(!s_idle.asleep && !test_brightness_count && test_control_result.state == CP_RESULT_CANCELLED);
}
static void remote_device_review_is_inert_immutable_and_physically_confirmed(void)
{
    remote_device_case_setup(); remote_device_case_off(); test_link.connected = false;
    cp_result_t result = remote_device_case_execute(CP_OP_SYSTEM_RESTART, 400, "{}");
    assert(result.state == CP_RESULT_ACCEPTED && result.object_id[0] && !test_restart_requests && !test_media_stops);
    app_remote_review_view_t view; remote_device_review_view(&view);
    assert(view.pending && view.can_confirm && !strcmp(view.request.operation,"system.restart"));
    app_remote_review_request_t wrong = view.request; wrong.action_digest[0] = wrong.action_digest[0] == 'a' ? 'b':'a';
    s_user_activity_ms = ++s_now_ms; assert(remote_device_confirm_local(true, &wrong)); remote_device_case_tick();
    assert(!test_restart_requests && !s_remote_device_reviews[0].approval.confirmed);
    remote_device_revoke(CP_SAFETY_LOCAL_OVERRIDE, NULL); remote_device_case_tick();
    assert(!s_remote_device_reviews[0].approval.cancelled); /* Fresh physical approval is not input-lease cancellation. */
    assert(remote_device_confirm_local(true, &view.request)); remote_device_case_tick();
    assert(test_media_stops == 1 && test_restart_requests == 1 && test_restart_authorized && test_restart_maintenance);
    assert(test_control_result.state == CP_RESULT_RUNNING && !strcmp(test_control_result.reason,"restart_pending"));
    test_restart_status.state = APP_DEVICE_RESTART_DISPATCHED; remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_RUNNING && test_restart_requests == 1);
    test_restart_status.state = APP_DEVICE_RESTART_UNCERTAIN; remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_CLEANUP_PENDING && test_restart_requests == 1);
}
static void remote_device_factory_reset_sequence(bool orphan_at_admission)
{
    remote_device_case_setup(); remote_device_case_off(); test_link.connected = false;
    s_settings.navigation = true;
    test_bonds.count = 1; test_wifi_profiles.count = 1;
    test_update_target.selected = true;
    test_update_target.target = (ble_hid_target_t){.bond_id_known = true, .bond_id = {0x42}};
    if (orphan_at_admission) { test_update_target.available = false; test_update_target.error = ESP_ERR_NOT_FOUND; }
    test_remote_target_accept = test_remote_wifi_forget_accept = test_pair_accept = true;
    char digest[65], args[128]; remote_device_manifest_digest(digest);
    (void)snprintf(args,sizeof(args),"{\"retained_manifest_digest\":\"%s\"}",digest);
    cp_result_t result = remote_device_case_execute(CP_OP_DEVICE_FACTORY_RESET_REQUEST, 500, args);
    assert(result.state == CP_RESULT_ACCEPTED && !test_pair_forgets && !test_storage_requests && !test_remote_wifi_forget_calls);
    remote_device_case_confirm();
    assert(test_pair_forgets == 1 && !test_remote_target_calls && !test_remote_wifi_forget_calls && !test_pair_opens);
    remote_device_case_tick(); assert(!test_remote_target_calls);
    test_link.bond_forget_state = BLE_HID_FORGET_DONE; test_bonds.count = 0; ++test_bonds.revision;
    /* The real BLE owner retains the durable selection but reports its missing
     * key unavailable. That is an editable orphan, not an unknown store. */
    test_update_target.available = false; test_update_target.error = ESP_ERR_NOT_FOUND;
    remote_device_case_tick(); assert(test_remote_target_calls == 1 && !test_remote_wifi_forget_calls);
    assert(!test_remote_target_request.selected && !test_remote_target_request.count &&
        test_remote_target_request.expected_bond_revision == test_bonds.revision &&
        test_remote_target_request.expected_revision == 51);
    remote_device_case_tick(); assert(!test_remote_wifi_forget_calls);
    test_update_target.request_status = BLE_HID_TARGET_APPLIED; test_update_target.durable = true;
    remote_device_case_tick(); assert(test_remote_wifi_forget_calls == 1 && test_remote_wifi_forget_revision == 41 && !test_remote_wifi_forget_acknowledge);
    assert(test_control_result.state != CP_RESULT_COMPLETED && !test_restart_requests && !test_pair_opens);
    test_wifi_profiles.operation_pending = false; test_wifi_profiles.completed_request_id = test_wifi_profiles.request_id;
    test_wifi_profiles.count = 0; test_control_session_alive = false; /* The admitted final step may sever management. */
    remote_device_case_tick(); assert(test_control_result.state == CP_RESULT_COMPLETED && !remote_device_maintenance());
    assert(!s_settings.navigation && !test_saved_request.navigation);
    assert(!test_restart_requests && !test_pair_opens && !test_wifi_forgets);
}
static void remote_device_factory_reset_waits_for_each_exact_manager_receipt(void)
{
    remote_device_factory_reset_sequence(false);
}
static void remote_device_factory_reset_admits_already_orphaned_target(void)
{
    remote_device_factory_reset_sequence(true);
}
static void remote_device_factory_admission_rejects_unknown_store_and_preserves_reviewed_revision(void)
{
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        remote_device_case_setup(); remote_device_case_off(); test_link.connected = false;
        test_update_target.selected = true; test_update_target.available = false; test_update_target.error = ESP_ERR_NOT_FOUND;
        if (scenario == 0) test_update_target.loaded = false;
        else if (scenario == 1) test_update_target.durable = false;
        else if (scenario == 2) test_update_target.request_status = BLE_HID_TARGET_UNCERTAIN;
        else if (scenario == 3) test_update_target.request_status = BLE_HID_TARGET_PENDING;
        else if (scenario == 4) test_update_target.request_status = BLE_HID_TARGET_FAILED;
        else if (scenario == 5) test_update_target.request_error = ESP_FAIL;
        char digest[65], args[128]; remote_device_manifest_digest(digest);
        (void)snprintf(args,sizeof(args),"{\"retained_manifest_digest\":\"%s\"}",digest);
        cp_command_t command = remote_device_case_command(CP_OP_DEVICE_FACTORY_RESET_REQUEST, 950 + scenario, args);
        cp_result_t result;
        if (scenario < 6) {
            assert(remote_device_execute(&command, &result) == CP_UNAVAILABLE && result.state == CP_RESULT_REJECTED);
            assert(s_overlay == APP_OVERLAY_NONE);
        } else {
            assert(remote_device_execute(&command, &result) == CP_OK && result.state == CP_RESULT_ACCEPTED);
            ++test_update_target.revision; remote_device_case_confirm();
            assert(test_control_result.state == CP_RESULT_CANCELLED);
        }
        assert(!test_storage_requests && !test_pair_forgets && !test_remote_target_calls && !test_remote_wifi_forget_calls);
    }
}
static void remote_device_factory_target_clear_rejects_unknown_or_unreviewed_store(void)
{
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        remote_device_case_setup(); remote_device_case_off(); test_link.connected = false;
        test_bonds.count = 1; test_wifi_profiles.count = 1; test_update_target.selected = true;
        if (scenario == 6) {
            test_update_target.selected = false; test_update_target.durable = false; test_update_target.revision = 0;
        }
        test_remote_target_accept = test_remote_wifi_forget_accept = test_pair_accept = true;
        char digest[65], args[128]; remote_device_manifest_digest(digest);
        (void)snprintf(args,sizeof(args),"{\"retained_manifest_digest\":\"%s\"}",digest);
        (void)remote_device_case_execute(CP_OP_DEVICE_FACTORY_RESET_REQUEST, 550 + scenario, args);
        remote_device_case_confirm(); assert(test_pair_forgets == 1 && !test_remote_target_calls);
        test_link.bond_forget_state = BLE_HID_FORGET_DONE; test_bonds.count = 0; ++test_bonds.revision;
        test_update_target.available = false; test_update_target.error = ESP_ERR_NOT_FOUND;
        if (scenario == 0) test_update_target.loaded = false;
        else if (scenario == 1) test_update_target.durable = false;
        else if (scenario == 2) test_update_target.request_status = BLE_HID_TARGET_UNCERTAIN;
        else if (scenario == 3) test_update_target.request_status = BLE_HID_TARGET_PENDING;
        else if (scenario == 4) test_update_target.request_status = BLE_HID_TARGET_FAILED;
        else if (scenario == 5) ++test_update_target.revision;
        remote_device_case_tick();
        if (scenario == 6) {
            /* A loaded, absent record is known, not corrupt or uncertain. */
            assert(test_remote_target_calls == 1 && test_remote_target_request.expected_revision == 0 && !test_remote_wifi_forget_calls);
        } else assert(test_control_result.state == CP_RESULT_CLEANUP_PENDING && !test_remote_target_calls &&
                !test_remote_wifi_forget_calls && !test_restart_requests);
    }
}
static void remote_device_factory_failure_never_advances_to_unreviewed_deletion(void)
{
    remote_device_case_setup(); remote_device_case_off(); test_link.connected = false;
    char digest[65], args[128]; remote_device_manifest_digest(digest);
    (void)snprintf(args,sizeof(args),"{\"retained_manifest_digest\":\"%s\"}",digest);
    (void)remote_device_case_execute(CP_OP_DEVICE_FACTORY_RESET_REQUEST, 600, args);
    remote_device_case_confirm();
    assert(test_pair_forgets == 1 && !test_remote_target_calls && !test_remote_wifi_forget_calls);
    test_link.bond_forget_state = BLE_HID_FORGET_FAILED; remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_CLEANUP_PENDING && !test_remote_target_calls && !test_remote_wifi_forget_calls && !test_restart_requests);
}
static void remote_device_peer_pages_and_private_reviews_are_truthful(void)
{
    remote_device_case_setup();
    test_bonds.count = 9; test_bonds.current_match_known = false;
    for (uint8_t i=0;i<test_bonds.count;++i) { test_bonds.bonds[i].bond_id_known = true; test_bonds.bonds[i].bond_id[0] = (uint8_t)(i+1); }
    test_bonds.bonds[0].is_current = true;
    test_classic_bonds = (bt_audio_bond_list_t){.known = true,.revision = 71,.count = 1,.capacity = 32};
    test_classic_bonds.bonds[0].address[0] = 0x92;
    cp_identity_t who = {.principal_id={11},.session_id={12},.scopes=CP_SCOPE_VIEW}; char json[4096]; size_t length;
    assert(remote_device_read(&who,"/api/v1/bluetooth/peers",json,sizeof(json),&length)==CP_OK);
    assert(length == strlen(json) && strstr(json,"\"current_match_known\":false") && strstr(json,"\"current\":false") && strstr(json,"\"next_cursor\":8"));
    assert(remote_device_read(&who,"/api/v1/bluetooth/peers?cursor=8&limit=8",json,sizeof(json),&length)==CP_OK);
    assert(strstr(json,"classic_audio") && strstr(json,"\"next_cursor\":0"));
    assert(remote_device_read(&who,"/api/v1/bluetooth/peers?cursor=8junk",json,sizeof(json),&length)==CP_INVALID);
    assert(remote_device_read(&who,"/api/v1/bluetooth/peers",json,8,&length)==CP_TOO_LARGE && !length && !json[0]);
    (void)remote_device_case_execute(CP_OP_SYSTEM_RESTART,700,"{}");
    assert(remote_device_state_json(&who,json,sizeof(json),&length)==CP_OK && strstr(json,"\"operation_id\":\"") && strstr(json,"\"percent\":null"));
    who.session_id[0] = 99;
    assert(remote_device_state_json(&who,json,sizeof(json),&length)==CP_OK && strstr(json,"\"approval\":null"));
}
static void remote_device_bad_diagnostic_parameters_do_not_exhaust_slots(void)
{
    remote_device_case_setup();
    for (uint64_t i=0;i<12;++i) {
        cp_command_t bad = remote_device_case_command(CP_OP_HAPTICS_TEST,800+i,"{\"pattern\":\"unknown\"}"); cp_result_t result;
        assert(remote_device_execute(&bad,&result)==CP_NOT_IMPLEMENTED && result.state==CP_RESULT_FAILED);
    }
    test_remote_haptic_accept = true;
    cp_result_t result = remote_device_case_execute(CP_OP_HAPTICS_TEST,820,"{}");
    assert(result.state==CP_RESULT_RUNNING && test_remote_haptic_calls==1);
}
static void remote_device_stale_and_expired_reviews_remain_locally_cancellable(void)
{
    remote_device_case_setup();
    (void)remote_device_case_execute(CP_OP_PREFERENCES_RESET,900,"{\"group\":\"display\"}");
    app_remote_review_view_t view; remote_device_review_view(&view); assert(view.can_confirm);
    device_settings_t changed = s_settings; changed.brightness_percent = 80;
    assert(update_settings_internal(&changed,false));
    app_remote_review_view_t stale; remote_device_review_view(&stale); assert(!stale.can_confirm && stale.pending);
    s_user_activity_ms = ++s_now_ms;
    assert(remote_device_confirm_local(false,&view.request)); remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_CANCELLED && !test_storage_requests && !test_restart_requests);
    remote_device_case_setup();
    (void)remote_device_case_execute(CP_OP_SYSTEM_RESTART,901,"{}"); remote_device_review_view(&view);
    s_now_ms = view.request.expires_ms; s_user_activity_ms = s_now_ms;
    assert(remote_device_confirm_local(false,&view.request)); remote_device_case_tick();
    assert(test_control_result.state == CP_RESULT_CANCELLED && !test_restart_requests && s_overlay != APP_OVERLAY_CONFIRM);
}
#define REMOTE_DEVICE_CASES(RUN) \
    RUN(remote_device_runtime_settings_and_durability_use_real_boundaries); \
    RUN(remote_device_haptic_jobs_wait_for_on_and_final_off); \
    RUN(remote_device_sleep_preserves_capture_warning_until_off); \
    RUN(remote_device_review_is_inert_immutable_and_physically_confirmed); \
    RUN(remote_device_factory_reset_waits_for_each_exact_manager_receipt); \
    RUN(remote_device_factory_reset_admits_already_orphaned_target); \
    RUN(remote_device_factory_admission_rejects_unknown_store_and_preserves_reviewed_revision); \
    RUN(remote_device_factory_target_clear_rejects_unknown_or_unreviewed_store); \
    RUN(remote_device_factory_failure_never_advances_to_unreviewed_deletion); \
    RUN(remote_device_peer_pages_and_private_reviews_are_truthful); \
    RUN(remote_device_bad_diagnostic_parameters_do_not_exhaust_slots); \
    RUN(remote_device_stale_and_expired_reviews_remain_locally_cancellable)
