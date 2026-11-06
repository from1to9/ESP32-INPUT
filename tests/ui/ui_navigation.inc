// SPDX-License-Identifier: Apache-2.0
static void navigation_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_NONE;
    s_state.navigation_enabled = s_state.navigation_available = true;
    s_state.motion_recenter_available = true;
    screenshot("mouse-navigation");
    assert_rect(control("Home"), 140, 8, 62, 48);
    assert_rect(control("Back"), 210, 8, 62, 48);
    assert_rect(lv_obj_get_parent(find_visible_label(lv_screen_active(), "Touchpad")), 12, 64, 396, 292);
    assert_rect(control("Menu"), 404, 8, 64, 48);
    clear_intents(); tap_control("Home");
    assert(action_count(APP_NAV_HOME) == 1 && action_count(APP_NAV_BACK) == 0);
    clear_intents(); tap_control("Back");
    assert(action_count(APP_NAV_BACK) == 1 && action_count(APP_BACK) == 0);
    clear_intents(); pointer(171, 32, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.navigation_enabled = false;
    ++s_state.ui_revision; render(); pointer(171, 32, false); assert_terminal(&begin);
    assert(action_count(APP_NAV_HOME) == 0 && action_count(APP_RECENTER) == 0);
    assert_rect(control("Recenter"), 140, 8, 132, 48);
    clear_intents(); pointer(171, 32, true);
    begin = *last_action(APP_TOUCH_BEGIN);
    s_state.navigation_enabled = true; ++s_state.ui_revision;
    render(); pointer(171, 32, false); assert_terminal(&begin);
    assert(action_count(APP_RECENTER) == 0 && action_count(APP_NAV_HOME) == 0);
    for (unsigned gate = 0; gate < 3; ++gate) {
        s_state.navigation_available = true;
        ++s_state.ui_revision; render(); clear_intents(); pointer(171, 32, true);
        begin = *last_action(APP_TOUCH_BEGIN);
        if (gate == 0) ++s_state.epoch;
        else if (gate == 1) ++s_state.ui_revision;
        else s_state.navigation_available = false;
        render(); pointer(171, 32, false); assert_terminal(&begin);
        assert(action_count(APP_NAV_HOME) == 0);
    }
    for (unsigned gate = 0; gate < 4; ++gate) {
        s_state.navigation_available = false;
        s_state.remote_owned = gate == 0;
        s_state.locked = gate == 1;
        s_state.screen_off = gate == 2;
        s_state.dragging = gate == 3;
        ++s_state.ui_revision; render(); clear_intents();
        tap(171, 32); tap(241, 32);
        assert(action_count(APP_NAV_HOME) == 0 && action_count(APP_NAV_BACK) == 0);
        s_state.remote_owned = s_state.locked = s_state.screen_off = s_state.dragging = false;
    }
    ++s_state.ui_revision;
    screenshot("mouse-navigation-unavailable");
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_NAVIGATION;
    s_state.navigation_enabled = false;
    ++s_state.ui_revision;
    screenshot("navigation-setup-off");
    assert(has_visible_label(lv_screen_active(), "Home / setup"));
    clear_intents(); tap_control("Navigation / off");
    assert(action_count(APP_NAVIGATION) == 1 && last_action(APP_NAVIGATION)->x == 1);
    assert(!s_state.navigation_enabled); /* Only coordinator acknowledgement selects it. */
    s_state.navigation_enabled = true; ++s_state.ui_revision;
    screenshot("navigation-setup-on");
    clear_intents(); tap_control("Back");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_INPUT);
    s_state.settings_page = APP_SETTINGS_INPUT; ++s_state.ui_revision;
    screenshot("input-navigation-reset-grip");
    clear_intents(); tap_control("Recenter aim");
    assert(action_count(APP_RECENTER) == 1 && action_count(APP_CALIBRATE) == 0);
    clear_intents(); tap_control("Navigation / on");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_NAVIGATION);
    s_state.motion_reason = APP_MOTION_SENSOR_UNAVAILABLE;
    s_state.motion_recenter_available = false;
    for (unsigned navigation = 0; navigation < 2; ++navigation) {
        s_state.navigation_enabled = navigation != 0; ++s_state.ui_revision;
        screenshot(navigation ? "input-navigation-sensor-unavailable" : "input-ordinary-sensor-unavailable");
        assert(has_visible_label(lv_screen_active(), "Motion sensor unavailable."));
        assert(lv_obj_has_state(control("Recenter aim"), LV_STATE_DISABLED));
    }
    s_state.navigation_enabled = true;
    s_state.overlay = APP_OVERLAY_NONE; s_state.mode = INPUT_KEYBOARD;
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH; strcpy(s_state.preview, "KEEP");
    grid_groups(); ++s_state.ui_revision;
    screenshot("keyboard-navigation-preference-keeps-draft");
    assert_rect(control("Touch"), 140, 8, 76, 48);
    assert_rect(control("Keys"), 224, 8, 48, 48);
    assert(has_visible_label(lv_screen_active(), "KEEP"));
    assert(!has_visible_label(lv_screen_active(), "Home"));
    s_state = original; render(); clear_intents();
    puts("PASS: optional Navigation, exact shared header region, Mouse/Input reset grip, setup copy, stale/blocked touch and unchanged keyboard draft");
}
