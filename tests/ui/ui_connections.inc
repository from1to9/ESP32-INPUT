// SPDX-License-Identifier: Apache-2.0
static void connection_checks(void)
{
    app_view_t original = s_state;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.capture_indicator_required = s_state.listening = s_state.audio_active = false;
    s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_API_CONNECTION; ++s_state.ui_revision;
    s_state.api = (app_api_view_t){0}; screenshot("api-unknown");
    clear_intents(); tap_control("Open USB window"); assert(action_count(APP_API_COMMISSIONING) == 0);
    ui_fixture_connections(&s_state); screenshot("api-local-window-closed");
    clear_intents(); tap_control("Allow legacy API input");
    assert(last_action(APP_API_INPUT_ALLOW_SET)->x == 1 && last_action(APP_API_INPUT_ALLOW_SET)->api.revision == 997);
    assert(!s_state.api.remote_input_allowed);
    s_state.api.remote_input_allowed = true; ++s_state.api.remote_input_generation; screenshot("api-remote-input-allowed");
    clear_intents(); tap_control("Block legacy API input");
    assert(last_action(APP_API_INPUT_ALLOW_SET)->x == 0 && last_action(APP_API_INPUT_ALLOW_SET)->api.revision == 998);
    clear_intents(); pointer(240, 400, true); ++s_state.api.remote_input_generation; render(); pointer(240, 400, false);
    assert(action_count(APP_API_INPUT_ALLOW_SET) == 0);
    s_state.api.remote_input_allowed = false; ++s_state.api.remote_input_generation; screenshot("api-remote-input-takeover");
    clear_intents(); tap_control("Open USB window");
    assert(last_action(APP_API_COMMISSIONING)->api.operation == APP_API_WINDOW_OPEN && last_action(APP_API_COMMISSIONING)->api.revision == 701);
    assert(!s_state.api.commissioning_open);
    s_state.api.commissioning_open = s_state.api.can_close = s_state.api.remaining_known = true;
    s_state.api.can_open = false; s_state.api.remaining_seconds = 83;
    screenshot("api-local-window-open");
    clear_intents(); tap_control("Close window"); assert(last_action(APP_API_COMMISSIONING)->api.operation == APP_API_WINDOW_CLOSE);
    s_state.api.ready = true; s_state.api.tls = false;
    strcpy(s_state.api.url, "http://unconfirmed.invalid/"); screenshot("api-no-http-fallback");
    assert(!has_visible_label(lv_screen_active(), "http://unconfirmed.invalid/"));
    s_state.api.tls = true; strcpy(s_state.api.url, "https://synthetic-device.example:8443/"); screenshot("api-actual-https");
    assert(has_visible_label(lv_screen_active(), "https://synthetic-device.example:8443/"));
    clear_intents(); tap_control("Review imported identity"); assert(last_action(APP_MENU)->x == APP_OVERLAY_API_REVIEW);
    s_state.overlay = APP_OVERLAY_API_REVIEW; ++s_state.ui_revision; screenshot("api-review-first-page");
    clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    clear_intents(); tap_control("Next"); assert(last_action(APP_API_COMMISSIONING)->api.operation == APP_API_REVIEW_PAGE && last_action(APP_API_COMMISSIONING)->api.page == 1);
    s_state.api.review_page = 1; render(); screenshot("api-review-last-page");
    clear_intents(); tap_control("Approve identity");
    assert(last_action(APP_API_COMMISSIONING)->api.operation == APP_API_REVIEW_APPROVE && !strcmp(last_action(APP_API_COMMISSIONING)->api.review_digest, s_state.api.review_digest));
    clear_intents(); pointer(240, 448, true); s_state.api.review_digest[0] = 'b'; render(); pointer(240, 448, false);
    assert(action_count(APP_API_COMMISSIONING) == 0);
    s_state.api.review_complete = false; screenshot("api-review-incomplete");
    clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    s_state.api.review_complete = true; memset(s_state.api.review_pages[0], '\n', 200); s_state.api.review_pages[0][200] = 0;
    screenshot("api-review-text-overflow"); clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    ui_fixture_connections(&s_state); s_state.api.review_page = 1; s_state.api.review_digest[64] = 'a';
    render(); clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    ui_fixture_connections(&s_state); s_state.api.review_page = 1; memset(s_state.api.review_host, 'a', sizeof(s_state.api.review_host));
    render(); clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    ui_fixture_connections(&s_state); s_state.overlay = APP_OVERLAY_WIFI_PROFILES; ++s_state.ui_revision;
    screenshot("saved-wifi-first-page");
    clear_intents(); tap(296, 130);
    assert(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.operation == APP_SAVED_WIFI_JOIN && !strcmp(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.id, "immutable-profile-0"));
    clear_intents(); tap(346, 130);
    assert(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.operation == APP_SAVED_WIFI_PREFER);
    clear_intents(); tap(428, 130);
    assert(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.operation == APP_SAVED_WIFI_FORGET_REQUEST &&
        !strcmp(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.id, "immutable-profile-0") &&
        last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.revision == s_state.saved_wifi.revision &&
        action_count(APP_CONFIRM) == 0); /* Only opens the owner's separate captured review. */
    clear_intents(); pointer(428, 130, true); ++s_state.saved_wifi.revision; render(); pointer(428, 130, false);
    assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    clear_intents(); pointer(428, 130, true); strcpy(s_state.saved_wifi.rows[0].id, "replaced-profile");
    render(); pointer(428, 130, false); assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    s_state.saved_wifi.pending = true; screenshot("saved-wifi-forget-pending");
    clear_intents(); tap(428, 130); assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    s_state.saved_wifi.pending = false; render();
    clear_intents(); pointer(428, 130, true); s_state.saved_wifi.count = 0; render(); pointer(428, 130, false);
    assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    ui_fixture_connections(&s_state); render();
    clear_intents(); pointer(296, 130, true); ++s_state.saved_wifi.revision; render(); pointer(296, 130, false);
    assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    clear_intents(); pointer(296, 130, true); s_state.saved_wifi.count = 0; render(); pointer(296, 130, false);
    assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    ui_fixture_connections(&s_state); render();
    clear_intents(); tap_control("Next"); assert(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.page == 1);
    s_state.saved_wifi.page = 1; screenshot("saved-wifi-second-page");
    clear_intents(); tap(296, 130); assert(!strcmp(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.id, "immutable-profile-4"));
    clear_intents(); tap(428, 130); assert(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.operation == APP_SAVED_WIFI_FORGET_REQUEST &&
        !strcmp(last_action(APP_WIFI_PROFILE_CONTROL)->wifi_profile.id, "immutable-profile-4"));
    s_state.saved_wifi.uncertain = true; screenshot("saved-wifi-uncertain");
    clear_intents(); tap(296, 130); tap(428, 130); assert(action_count(APP_WIFI_PROFILE_CONTROL) == 0);
    s_state.overlay = APP_OVERLAY_BLUETOOTH_TARGET; ++s_state.ui_revision; screenshot("saved-bluetooth-target");
    clear_intents(); tap_control("Select target");
    assert(last_action(APP_BLUETOOTH_TARGET_CONTROL)->bluetooth_target.operation == APP_BLUETOOTH_TARGET_SELECT);
    assert(last_action(APP_BLUETOOTH_TARGET_CONTROL)->bluetooth_target.target.bond_id[31] == 0x75);
    clear_intents(); tap_control("Move to first"); assert(last_action(APP_BLUETOOTH_TARGET_CONTROL)->bluetooth_target.operation == APP_BLUETOOTH_TARGET_PREFER);
    clear_intents(); pointer(120, 392, true); ++s_state.bluetooth_target.preferences_revision; render(); pointer(120, 392, false);
    assert(action_count(APP_BLUETOOTH_TARGET_CONTROL) == 0);
    s_state.bluetooth_target.pending = true; screenshot("saved-bluetooth-target-pending");
    clear_intents(); tap_control("Select target"); assert(action_count(APP_BLUETOOTH_TARGET_CONTROL) == 0);
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    s_state.bluetooth_hosts_available = true; s_state.bluetooth_host_count = 1; s_state.bluetooth_page = 0;
    s_state.bluetooth_hosts[0] = (app_bond_view_t){.target_known = true, .bond_revision = 17, .preferences_revision = 19};
    strcpy(s_state.bluetooth_hosts[0].name, "Synthetic input target");
    s_state.bluetooth_hosts[0].target.bond_id_known = true;
    s_state.bluetooth_hosts[0].target.bond_id[31] = 0x63; ++s_state.ui_revision; render();
    clear_intents(); tap_control("Synthetic input target");
    assert(last_action(APP_BLUETOOTH_TARGET_CONTROL)->bluetooth_target.operation == APP_BLUETOOTH_TARGET_DETAILS &&
        last_action(APP_BLUETOOTH_TARGET_CONTROL)->bluetooth_target.target.bond_id[31] == 0x63);
    lv_area_t row; lv_obj_get_coords(control("Synthetic input target"), &row);
    clear_intents(); pointer((row.x1 + row.x2) / 2, (row.y1 + row.y2) / 2, true);
    s_state.bluetooth_host_count = 0; render(); pointer((row.x1 + row.x2) / 2, (row.y1 + row.y2) / 2, false);
    assert(action_count(APP_BLUETOOTH_TARGET_CONTROL) == 0);
    s_state = original; render(); clear_intents();
    puts("PASS: native local API enrollment and exact complete review, actual HTTPS, saved Wi-Fi IDs/priorities and full Bluetooth target fingerprints");
}

static void saved_wifi_label_checks(void)
{
    app_view_t original = s_state;
    s_state.overlay = APP_OVERLAY_WIFI_PROFILES;
    s_state.saved_wifi = (app_saved_wifi_view_t){.known = true, .count = 2, .revision = 17};
    ++s_state.ui_revision;
    for (unsigned i = 0; i < 2; ++i) {
        app_saved_wifi_row_t *row = &s_state.saved_wifi.rows[i];
        row->current = i == 0;
        row->enabled = i == 0;
        row->has_label = true;
        row->priority = i + 1U;
        row->can_forget = true;
        snprintf(row->label, sizeof(row->label), "Work WiFi");
        snprintf(row->original_ssid, sizeof(row->original_ssid), "Warehouse-Access-Point-East-0%u", i);
        snprintf(row->id, sizeof(row->id), "123456789abcdef0123456789abcdef%u", i);
    }
    screenshot("wifi-saved-duplicate-labels");
    assert(has_visible_label(lv_screen_active(), "Current / priority 1"));
    assert(has_visible_label(lv_screen_active(), "Disabled / priority 2"));
    for (unsigned i = 0; i < 2; ++i) strcpy(s_state.saved_wifi.rows[i].label, "WWWWWWWWWWWWWWWW");
    screenshot("wifi-saved-wide-labels");
    const char *details[] = {"wifi-saved-detail-first", "wifi-saved-detail-second"};
    for (unsigned i = 0; i < 2; ++i) {
        clear_intents(); tap(104, 130 + (int)i * 64);
        const app_intent_t *open = last_action(APP_WIFI_PROFILE_CONTROL);
        const app_saved_wifi_row_t *row = &s_state.saved_wifi.rows[i];
        assert(open && open->wifi_profile.operation == APP_SAVED_WIFI_DETAILS &&
            open->wifi_profile.revision == 17 && !strcmp(open->wifi_profile.id, row->id));
        s_state.overlay = APP_OVERLAY_WIFI_PROFILE;
        s_state.saved_wifi.detail_known = true;
        s_state.saved_wifi.detail = *row;
        ++s_state.ui_revision;
        screenshot(details[i]);
        assert(has_visible_label(lv_screen_active(), "Display label\nWWWWWWWWWWWWWWWW"));
        assert(has_visible_label(lv_screen_active(), i ?
            "Original SSID\nWarehouse-Access-Point-East-01" : "Original SSID\nWarehouse-Access-Point-East-00"));
        char identity[96];
        snprintf(identity, sizeof(identity), "Profile ID\n%s\n\n%s / priority %u", row->id,
            row->current ? "Current" : "Disabled", row->priority);
        assert(has_visible_label(lv_screen_active(), identity));
        clear_intents(); tap_control("Back to saved networks");
        assert(last_action(APP_MENU)->x == APP_OVERLAY_WIFI_PROFILES);
        s_state.overlay = APP_OVERLAY_WIFI_PROFILES;
        ++s_state.ui_revision;
        render();
    }
    clear_intents(); tap(428, 194);
    const app_intent_t *forget = last_action(APP_WIFI_PROFILE_CONTROL);
    assert(forget && forget->wifi_profile.operation == APP_SAVED_WIFI_FORGET_REQUEST &&
        !strcmp(forget->wifi_profile.id, s_state.saved_wifi.rows[1].id));
    s_state.overlay = APP_OVERLAY_CONFIRM;
    s_state.confirmation = APP_CONFIRM_WIFI_FORGET;
    s_state.saved_wifi.detail = s_state.saved_wifi.rows[1];
    s_state.saved_wifi.reviewing = s_state.saved_wifi.review_current = true;
    ++s_state.ui_revision;
    screenshot("wifi-forget-exact-identity");
    char review[256];
    snprintf(review, sizeof(review), "Display label: WWWWWWWWWWWWWWWW\nOriginal SSID: %s\nProfile ID: %s\n\n"
        "This removes the saved network and disconnects it if in use.",
        s_state.saved_wifi.detail.original_ssid, s_state.saved_wifi.detail.id);
    assert(has_visible_label(lv_screen_active(), review));
    s_state.saved_wifi.review_current = false;
    screenshot("wifi-forget-changed-identity");
    clear_intents(); tap_control("Forget network"); assert(action_count(APP_CONFIRM) == 0);
    s_state = original;
    render();
    clear_intents();
    puts("PASS: saved Wi-Fi rows retain state and exact detail/forget identity for long duplicate labels");
}
