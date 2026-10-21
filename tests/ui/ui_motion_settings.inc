// SPDX-License-Identifier: Apache-2.0
/* Included by the native UI harness; exercises actual LVGL hit targets. */
static void check_motion_patch(app_motion_field_t field, unsigned value)
{
    assert(action_count(APP_MOTION_CONFIG) == 1);
    const app_intent_t *intent = last_action(APP_MOTION_CONFIG);
    assert(intent->motion_patch.field == field);
    assert(intent->motion_patch.expected_settings_revision == s_state.motion_settings.revision);
    assert(intent->epoch == s_state.epoch && intent->ui_revision == s_state.ui_revision);
    assert(intent->x == 0 && intent->y == 0 && intent->gesture != 0);
    if (field <= APP_MOTION_FIELD_GAIN_Y) assert(intent->motion_patch.value.gain_centi == value);
    else if (field == APP_MOTION_FIELD_DEADZONE) assert(intent->motion_patch.value.deadzone_mdps == value);
    else if (field <= APP_MOTION_FIELD_FREEZE) assert(intent->motion_patch.value.milliseconds == value);
    else assert(intent->motion_patch.value.inverted == (value != 0));
}

static void motion_settings_checks(void)
{
    app_view_t original = s_state;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_INPUT;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.dragging = s_state.release_pending = false;
    s_state.tap_to_click_available = s_state.tap_to_click_enabled = true;
    s_state.motion_settings = (app_motion_settings_view_t){
        .available = true, .editable = true, .revision = 17,
        .gain_x_centi = 400, .gain_y_centi = 400, .deadzone_mdps = 500,
        .filter_ms = 30, .freeze_ms = 150, .reference = APP_REFERENCE_READY
    };
    ++s_state.ui_revision;
    screenshot("input-motion-settings-entry");
    clear_intents(); tap_control("Motion settings");
    assert(last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_MOTION);
    s_state.settings_page = APP_SETTINGS_MOTION; ++s_state.ui_revision;
    screenshot("motion-settings-defaults");
    clear_intents(); tap(432, 148); check_motion_patch(APP_MOTION_FIELD_GAIN_X, 425);
    assert(s_state.motion_settings.gain_x_centi == 400 && s_state.motion_settings.gain_y_centi == 400);
    clear_intents(); tap(40, 268); check_motion_patch(APP_MOTION_FIELD_GAIN_Y, 375);
    const unsigned presets[] = {200, 400, 800};
    for (unsigned axis = 0; axis < 2; ++axis) {
        for (unsigned preset = 0; preset < 3; ++preset) {
            clear_intents(); tap(84 + (int)preset * 152, axis ? 316 : 196);
            check_motion_patch((app_motion_field_t)axis, presets[preset]);
        }
    }
    clear_intents(); pointer(240, 148, true); pointer(300, 148, true); pointer(300, 148, false);
    assert(action_count(APP_MOTION_CONFIG) == 1);
    const app_intent_t *slide = last_action(APP_MOTION_CONFIG);
    assert(slide->motion_patch.field == APP_MOTION_FIELD_GAIN_X && slide->motion_patch.value.gain_centi >= 25 &&
        slide->motion_patch.value.gain_centi <= 10000);
    assert(s_state.motion_settings.gain_x_centi == 400 && s_state.motion_settings.gain_y_centi == 400);
    render(); clear_intents(); pointer(432, 148, true);
    ++s_state.motion_settings.revision; render(); pointer(432, 148, false);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    clear_intents(); pointer(432, 148, true);
    ++s_state.ui_revision; render(); pointer(432, 148, false);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    s_state.motion_settings.gain_x_centi = 25;
    s_state.motion_settings.gain_y_centi = 10000;
    screenshot("motion-settings-bounds");
    clear_intents(); tap(40, 148); tap(432, 268);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    clear_intents(); tap(432, 148); check_motion_patch(APP_MOTION_FIELD_GAIN_X, 50);
    clear_intents(); tap(40, 268); check_motion_patch(APP_MOTION_FIELD_GAIN_Y, 9975);
    s_state.motion_settings.gain_x_centi = 24;
    screenshot("motion-settings-invalid");
    clear_intents(); tap(432, 148); tap(388, 196);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    s_state.motion_settings.gain_x_centi = 400;
    s_state.motion_settings.gain_y_centi = 400;
    s_state.motion_settings.available = false;
    screenshot("motion-settings-unavailable");
    clear_intents(); tap(432, 148); tap(388, 316);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    s_state.motion_settings.available = true;
    s_state.motion_settings.editable = false;
    screenshot("motion-settings-pending");
    clear_intents(); tap(432, 148); assert(action_count(APP_MOTION_CONFIG) == 0);
    s_state.motion_settings.editable = true;
    const app_motion_reference_t references[] = {APP_REFERENCE_UNKNOWN, APP_REFERENCE_UNAVAILABLE,
        APP_REFERENCE_ACQUIRING, APP_REFERENCE_UNSTABLE};
    for (unsigned i = 0; i < sizeof(references) / sizeof(references[0]); ++i) {
        s_state.motion_settings.reference = references[i];
        char name[64]; snprintf(name, sizeof(name), "motion-reference-%u", i);
        screenshot(name);
    }
    clear_intents(); tap_control("Back");
    assert(last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_INPUT);
    clear_intents(); tap_control("Smoothing and direction");
    assert(last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_MOTION_ADVANCED);
    s_state.settings_page = APP_SETTINGS_MOTION_ADVANCED; ++s_state.ui_revision;
    screenshot("motion-settings-tuning");
    clear_intents(); tap(432, 108); check_motion_patch(APP_MOTION_FIELD_DEADZONE, 600);
    clear_intents(); tap(432, 200); check_motion_patch(APP_MOTION_FIELD_FILTER, 40);
    clear_intents(); tap(40, 292); check_motion_patch(APP_MOTION_FIELD_FREEZE, 125);
    clear_intents(); tap_control("Horizontal / normal"); check_motion_patch(APP_MOTION_FIELD_INVERT_X, 1);
    clear_intents(); tap_control("Vertical / normal"); check_motion_patch(APP_MOTION_FIELD_INVERT_Y, 1);
    s_state.motion_settings.invert_x = s_state.motion_settings.invert_y = true;
    screenshot("motion-settings-reversed");
    clear_intents(); tap_control("Horizontal / reversed"); check_motion_patch(APP_MOTION_FIELD_INVERT_X, 0);
    clear_intents(); tap_control("Back"); assert(last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_MOTION);
    s_state.motion_settings.deadzone_mdps = 10000;
    s_state.motion_settings.filter_ms = 0;
    s_state.motion_settings.freeze_ms = 1000;
    screenshot("motion-settings-tuning-bounds");
    clear_intents(); tap(432, 108); tap(40, 200); tap(432, 292);
    assert(action_count(APP_MOTION_CONFIG) == 0);
    s_state.settings_page = APP_SETTINGS_GESTURES; ++s_state.ui_revision;
    s_state.two_finger_scroll_available = s_state.swipe_scroll_available = false;
    screenshot("gesture-help-taps");
    assert(has_visible_label(lv_screen_active(), "Use the Scroll controls"));
    clear_intents(); tap_control("Cancel drag"); assert(action_count(APP_RELEASE) == 0);
    s_state.dragging = true;
    screenshot("gesture-help-dragging");
    clear_intents(); tap_control("Cancel drag"); assert(action_count(APP_RELEASE) == 1);
    s_state.dragging = false; s_state.release_pending = true;
    screenshot("gesture-help-release-pending");
    clear_intents(); tap_control("Cancel drag"); assert(action_count(APP_RELEASE) == 1);
    s_state.release_pending = false; s_state.tap_to_click_enabled = false;
    screenshot("gesture-help-movement-only");
    clear_intents(); tap_control("Tap to click / off");
    assert(last_action(APP_TAP_TO_CLICK)->x == 1 && !s_state.tap_to_click_enabled);
    s_state.tap_to_click_available = false;
    screenshot("gesture-help-unavailable");
    clear_intents(); tap_control("Tap to click / unavailable"); assert(action_count(APP_TAP_TO_CLICK) == 0);
    s_state.tap_to_click_available = s_state.tap_to_click_enabled = true;
    s_state.two_finger_scroll_available = true;
    screenshot("gesture-help-two-finger-capability");
    assert(has_visible_label(lv_screen_active(), "Use two fingers to scroll"));
    s_state.two_finger_scroll_available = false; s_state.swipe_scroll_available = true;
    screenshot("gesture-help-swipe-capability");
    assert(has_visible_label(lv_screen_active(), "Swipe to scroll"));
    clear_intents(); tap_control("Back"); assert(last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_INPUT);
    s_state = original; render(); clear_intents();
    puts("PASS: typed axis-isolated Motion patches, bounds, stale gestures, reference states and capability-bound gesture help");
}
