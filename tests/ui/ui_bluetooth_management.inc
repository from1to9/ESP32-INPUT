// SPDX-License-Identifier: Apache-2.0
static void bluetooth_management_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE; s_state.locked = s_state.screen_off = false;
    s_state.overlay = APP_OVERLAY_BLUETOOTH_PAIRING;
    s_state.bluetooth_name.effective_valid = true;
    strcpy(s_state.bluetooth_name.effective, "Mosaico");
    s_state.bluetooth_management = (app_bluetooth_management_view_t){.known = true, .pending = true, .can_stop = true};
    strcpy(s_state.bluetooth_management.status, "Preparing pairing");
    strcpy(s_state.bluetooth_management.reason, "Waiting for the current Bluetooth host to disconnect.");
    ++s_state.ui_revision; screenshot("bluetooth-add-preparing");
    assert(has_visible_label(lv_screen_active(), "Mosaico"));
    assert(has_visible_label(lv_screen_active(), "Preparing pairing"));
    clear_intents(); tap_control("Stop pairing");
    assert(action_count(APP_PAIRING_REQUEST) == 1 && last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_CLOSE);
    assert(s_state.overlay == APP_OVERLAY_BLUETOOTH_PAIRING); /* Callback only queues intent. */
    clear_intents(); tap_control("Try again"); assert(!action_count(APP_PAIRING_REQUEST));
    s_state.bluetooth_management.pending = false;
    s_state.bluetooth_management.window_open = s_state.bluetooth_management.advertising = true;
    s_state.bluetooth_management.remaining_known = true; s_state.bluetooth_management.remaining_seconds = 119;
    strcpy(s_state.bluetooth_management.status, "Ready to pair");
    strcpy(s_state.bluetooth_management.reason, "Choose this device in your host's Bluetooth settings.");
    ++s_state.ui_revision; screenshot("bluetooth-add-open");
    assert(has_visible_label(lv_screen_active(), "Ready to pair\n119s remaining"));
    clear_intents(); tap_control("Try again"); assert(!action_count(APP_PAIRING_REQUEST));
    s_state.bluetooth_management.connected = true;
    s_state.bluetooth_management.can_stop = false;
    strcpy(s_state.bluetooth_management.status, "Connected");
    strcpy(s_state.bluetooth_management.reason, "Bluetooth input is ready. Manage saved hosts to change or disconnect.");
    ++s_state.ui_revision; screenshot("bluetooth-add-connected");
    assert(has_visible_label(lv_screen_active(), "Connected"));
    assert(!has_visible_label(lv_screen_active(), "Ready to pair\n119s remaining"));
    assert(!has_visible_label(lv_screen_active(), "Choose this device in your host's Bluetooth settings."));
    clear_intents(); tap_control("Stop pairing"); assert(!action_count(APP_PAIRING_REQUEST));
    s_state.bluetooth_management.connected = false;
    s_state.bluetooth_management.window_open = s_state.bluetooth_management.advertising = false;
    s_state.bluetooth_management.remaining_known = false;
    s_state.bluetooth_management.can_add = true; s_state.bluetooth_management.can_stop = false;
    strcpy(s_state.bluetooth_management.status, "Pairing closed");
    strcpy(s_state.bluetooth_management.reason, "Target policy was not verified. Pairing remains closed.");
    ++s_state.ui_revision; screenshot("bluetooth-add-failed");
    assert(has_visible_label(lv_screen_active(), "Pairing closed"));
    clear_intents(); tap_control("Try again");
    assert(action_count(APP_PAIRING_REQUEST) == 1 && last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_OPEN);
    clear_intents(); tap_control("Saved Bluetooth hosts");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_BLUETOOTH);
    s_state.overlay = APP_OVERLAY_BLUETOOTH_TARGET;
    s_state.bluetooth_target = (app_bluetooth_target_view_t){.known = true, .can_forget = true, .bond_revision = 17, .preferences_revision = 23,
        .target = {.address = {0xc3}, .address_type = 1, .bond_id_known = true}};
    memset(s_state.bluetooth_target.target.bond_id, 0x5a, 32);
    strcpy(s_state.bluetooth_target.name, "Saved input host"); strcpy(s_state.bluetooth_target.identity, "C3:00:00:00:00:00 (type 1)");
    ++s_state.ui_revision; screenshot("bluetooth-single-forget");
    clear_intents(); tap_control("Forget this device");
    const app_intent_t *forget = last_action(APP_BLUETOOTH_TARGET_CONTROL);
    assert(forget && forget->bluetooth_target.operation == APP_BLUETOOTH_TARGET_FORGET_REQUEST &&
        forget->bluetooth_target.bond_revision == 17 && forget->bluetooth_target.preferences_revision == 23 &&
        !memcmp(forget->bluetooth_target.target.bond_id, s_state.bluetooth_target.target.bond_id, 32));
    s_state.overlay = APP_OVERLAY_CONFIRM; s_state.confirmation = APP_CONFIRM_BLUETOOTH_FORGET_ONE;
    strcpy(s_state.bluetooth_management.forget_identity, "C3:00:00:00:00:00 (type 1)");
    ++s_state.ui_revision; screenshot("bluetooth-single-forget-confirm");
    assert(has_visible_label(lv_screen_active(), "Forget this saved device?"));
    clear_intents(); tap_control("Forget this device");
    assert(action_count(APP_CONFIRM) == 1 && last_action(APP_CONFIRM)->x == 1);
    s_state.confirmation = APP_CONFIRM_PAIR_OPEN;
    ++s_state.ui_revision; screenshot("bluetooth-add-connected-confirm");
    assert(has_visible_label(lv_screen_active(), "Disconnect and add host?"));
    clear_intents(); tap_control("Disconnect and add");
    assert(action_count(APP_CONFIRM) == 1 && last_action(APP_CONFIRM)->x == 1);
    s_state = original; render(); clear_intents();
    puts("PASS: native Bluetooth preparation/open/failure, exact Forget identity and explicit connected Add pointer paths");
}
