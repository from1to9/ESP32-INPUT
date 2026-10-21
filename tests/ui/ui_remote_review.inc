// SPDX-License-Identifier: Apache-2.0
static void remote_review_checks(void)
{
    app_view_t original = s_state;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.capture_indicator_required = s_state.audio_active = s_state.listening = false;
    s_state.overlay = APP_OVERLAY_CONFIRM; s_state.confirmation = APP_CONFIRM_REMOTE; ++s_state.ui_revision;
    s_state.remote_review = (app_remote_review_view_t){0}; screenshot("remote-review-unbound");
    clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    clear_intents(); tap_control("Cancel"); assert(last_action(APP_CONFIRM)->x == 0);
    ui_fixture_remote_review(&s_state); screenshot("remote-review-restart");
    clear_intents(); tap_control("Confirm locally");
    assert(last_action(APP_CONFIRM)->x == 1 && last_action(APP_CONFIRM)->remote_review.resource_revision == 41 &&
        !strcmp(last_action(APP_CONFIRM)->remote_review.approval_id, s_state.remote_review.request.approval_id) &&
        !strcmp(last_action(APP_CONFIRM)->remote_review.action_digest, s_state.remote_review.request.action_digest));
    assert(s_state.remote_review.pending); /* Rendering cannot approve its owner record. */
    strcpy(s_state.remote_review.request.operation, "preferences.reset");
    strcpy(s_state.remote_review.reset_group, "input");
    strcpy(s_state.remote_review.details, "Restore only the named preference group to built-in defaults; wait for live owner and storage acknowledgements. Connections and trust remain saved.");
    screenshot("remote-review-preferences");
    const char *groups[] = {"all", "display", "input", "haptics", "bluetooth"};
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        strcpy(s_state.remote_review.reset_group, groups[i]); render(); clear_intents();
        tap_control("Confirm locally"); assert(last_action(APP_CONFIRM)->x == 1);
    }
    strcpy(s_state.remote_review.request.operation, "device.factory_reset.request");
    strcpy(s_state.remote_review.retained_manifest_digest, s_state.remote_review.request.action_digest);
    strcpy(s_state.remote_review.details, "Reset device preferences, LE bonds, Wi-Fi profiles and HID target preferences. Keep API trust, Classic bonds, firmware/OTA, models, recordings, unknown namespaces, NAND and backups. No restart.");
    screenshot("remote-review-reset-retained");
    clear_intents(); tap_control("Confirm locally"); assert(last_action(APP_CONFIRM)->x == 1);
    const char *bluetooth_operations[] = {"bluetooth.pairing.open", "bluetooth.bonds.forget_all", "bluetooth.bond.forget"};
    const char *bluetooth_scenes[] = {"remote-review-pairing", "remote-review-forget-all", "remote-review-forget-one"};
    const char *bluetooth_details[] = {
        "Disconnect the current LE host, release input, and open new-device pairing for 120 seconds. Existing saved bonds stay.",
        "Release input and forget all saved LE HID bonds. Classic audio bonds and API trust stay.",
        "Forget LE HID bond:\n0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\nOther saved bonds, Classic audio bonds and API trust stay."
    };
    for (size_t i = 0; i < sizeof(bluetooth_operations) / sizeof(bluetooth_operations[0]); ++i) {
        ui_fixture_remote_review(&s_state);
        strcpy(s_state.remote_review.request.operation, bluetooth_operations[i]);
        strcpy(s_state.remote_review.details, bluetooth_details[i]);
        screenshot(bluetooth_scenes[i]); clear_intents(); tap_control("Confirm locally");
        assert(last_action(APP_CONFIRM)->x == 1 &&
            !strcmp(last_action(APP_CONFIRM)->remote_review.operation, bluetooth_operations[i]) &&
            !strcmp(last_action(APP_CONFIRM)->remote_review.action_digest, s_state.remote_review.request.action_digest));
        clear_intents(); pointer(356, 448, true);
        s_state.remote_review.request.action_digest[0] = 'a'; render(); pointer(356, 448, false);
        assert(action_count(APP_CONFIRM) == 0);
    }
    strcpy(s_state.remote_review.request.operation, "bluetooth.pairing.unknown");
    render(); clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    for (unsigned field = 0; field < 5; ++field) {
        ui_fixture_remote_review(&s_state); render(); clear_intents(); pointer(356, 448, true);
        if (field == 0) s_state.remote_review.request.approval_id[0] = 'a';
        else if (field == 1) s_state.remote_review.request.action_digest[0] = 'a';
        else if (field == 2) ++s_state.remote_review.request.resource_revision;
        else if (field == 3) ++s_state.remote_review.request.expires_ms;
        else strcpy(s_state.remote_review.details, "Replaced consequences");
        render(); pointer(356, 448, false); assert(action_count(APP_CONFIRM) == 0);
    }
    ui_fixture_remote_review(&s_state); s_state.remote_review.request.expires_ms = s_state.remote_review.now_ms + 50;
    render(); clear_intents(); pointer(356, 448, true); lv_tick_inc(60); pointer(356, 448, false);
    assert(action_count(APP_CONFIRM) == 0); screenshot("remote-review-expired");
    ui_fixture_remote_review(&s_state); s_state.remote_review.clock_known = false; screenshot("remote-review-clock-unknown");
    clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    ui_fixture_remote_review(&s_state); s_state.remote_review.request.approval_id[32] = 'a';
    screenshot("remote-review-malformed-id"); clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    ui_fixture_remote_review(&s_state); memset(s_state.remote_review.details, '\n', 200); s_state.remote_review.details[200] = 0;
    screenshot("remote-review-overflow"); clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    ui_fixture_remote_review(&s_state); render(); clear_intents(); pointer(356, 448, true);
    s_state.locked = true; render(); pointer(356, 448, false); assert(action_count(APP_CONFIRM) == 0);
    s_state.locked = false; s_state.remote_owned = true; ++s_state.ui_revision; render();
    clear_intents(); tap_control("Confirm locally"); assert(action_count(APP_CONFIRM) == 0);
    s_state.remote_owned = false;
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_DISPLAY;
    s_state.settings_save_state = APP_SAVE_VOLATILE; ++s_state.ui_revision;
    screenshot("settings-volatile"); assert(has_visible_label(lv_screen_active(), "Temporary settings / not saved"));
    s_state.settings_page = APP_SETTINGS_BLUETOOTH; s_state.bluetooth_name.save_state = APP_SAVE_VOLATILE;
    s_state.bluetooth_name.pending = s_state.bluetooth_name.uncertain = false;
    screenshot("bluetooth-name-volatile"); assert(has_visible_label(lv_screen_active(), "temporary name / not saved"));
    s_state = original; render(); clear_intents();
    puts("PASS: complete immutable remote review, exact physical confirmation identity, replacement/expiry/overflow rejection and truthful volatile settings");
}
