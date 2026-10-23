// SPDX-License-Identifier: Apache-2.0
/* Production LVGL rendering and pointer admission for the grip reference UI. */
static void motion_grip_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_NONE;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.connected = s_state.ready = s_state.bluetooth_available = true;
    s_state.input_transport = APP_INPUT_BLUETOOTH;
    s_state.transport_switch_pending = s_state.transport_error = s_state.release_pending = false;
    s_state.motion_available = s_state.calibration_available = true;
    s_state.calibrating = false; s_state.motion_reason = APP_MOTION_READY;
    s_state.motion_enabled = s_state.motion_recenter_available = true;
    s_state.dragging = s_state.congested = false; s_state.gain_divisor = 1;
    s_state.tap_to_click_available = s_state.tap_to_click_enabled = true;
    strcpy(s_state.preview, "Unsent draft");
    const struct { app_motion_reference_t reference; const char *scene, *hint, *caption; } cases[] = {
        {APP_REFERENCE_READY, "ready", "Grip ready. Touch pauses motion.", "Motion / on"},
        {APP_REFERENCE_ACQUIRING, "acquiring", "Hold still to set your grip.", "Motion / waiting"},
        {APP_REFERENCE_UNSTABLE, "restoring", "Hold still to restore your grip.", "Motion / waiting"},
        {APP_REFERENCE_UNKNOWN, "unknown", "Grip status unavailable. Open Status.", "Motion / unavailable"},
        {APP_REFERENCE_UNAVAILABLE, "unavailable", "Grip unavailable. Open Status.", "Motion / unavailable"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        s_state.motion_settings.reference = cases[i].reference;
        s_state.motion_recenter_available = i < 3;
        ++s_state.ui_revision;
        char scene[64]; snprintf(scene, sizeof(scene), "mouse-grip-%s", cases[i].scene);
        screenshot(scene);
        assert(has_visible_label(lv_screen_active(), cases[i].hint));
        assert_rect(control("Recenter"), 140, 8, 132, 48);
        assert_rect(control(cases[i].caption), 12, 424, 224, 48);
        assert_rect(control("Fine gain / 1x"), 244, 424, 224, 48);
        assert_rect(lv_obj_get_parent(find_visible_label(lv_screen_active(), "Touchpad")), 12, 64, 396, 292);
        const char *row[] = {"Left click", "Right click", "Drag"};
        row_geometry(row, 3);
        for (unsigned j = 0; j < 3; ++j) assert_rect(control(row[j]), 12 + (int)j * 156, 368, 144, 48);
        lv_obj_t *hint = find_visible_label(lv_screen_active(), cases[i].hint);
        lv_point_t size;
        lv_text_get_size(&size, lv_label_get_text(hint), lv_obj_get_style_text_font(hint, 0),
            lv_obj_get_style_text_letter_space(hint, 0), lv_obj_get_style_text_line_space(hint, 0),
            lv_obj_get_content_width(hint), LV_TEXT_FLAG_NONE);
        assert(size.y <= lv_obj_get_content_height(hint));
        clear_intents(); tap_control(cases[i].caption);
        assert(action_count(APP_MOTION) == 1 && last_action(APP_MOTION)->x == 0);
        assert(s_state.motion_enabled); /* Only the coordinator acknowledges stopping. */
        clear_intents(); tap_control("Recenter");
        assert(action_count(APP_RECENTER) == (i < 3 ? 1U : 0U));
        assert(action_count(APP_CALIBRATE) == 0 && action_count(APP_MOTION) == 0 && action_count(APP_MOVE) == 0);
        if (i < 3) {
            const app_intent_t *request = last_action(APP_RECENTER);
            assert(request->epoch == s_state.epoch && request->ui_revision == s_state.ui_revision);
            assert(request->gesture != 0 && request->x == 0 && request->y == 0);
            assert(action_count(APP_TOUCH_BEGIN) == 1 && action_count(APP_TOUCH_END) == 1);
            const app_intent_t *begin = last_action(APP_TOUCH_BEGIN), *end = last_action(APP_TOUCH_END);
            assert(begin < end && end < request);
            assert(begin->surface == APP_SURFACE_CONTROL && end->surface == APP_SURFACE_CONTROL);
            assert(begin->gesture == request->gesture && end->gesture == request->gesture);
        }
        assert(s_state.motion_settings.reference == cases[i].reference);
        assert(!strcmp(s_state.preview, "Unsent draft"));
    }
    s_state.motion_settings.reference = APP_REFERENCE_READY;
    s_state.motion_enabled = false; s_state.motion_recenter_available = false;
    ++s_state.ui_revision; screenshot("mouse-grip-off");
    assert(has_visible_label(lv_screen_active(), "Enable Motion to set your grip."));
    clear_intents(); tap_control("Recenter");
    assert(lv_obj_has_state(control("Recenter"), LV_STATE_DISABLED));
    assert(action_count(APP_RECENTER) == 0 && action_count(APP_MOTION) == 0 && !s_state.motion_enabled);
    s_state.motion_enabled = true;
    s_state.motion_settings.reference = APP_REFERENCE_ACQUIRING;
    ++s_state.ui_revision; render();
    assert(has_visible_label(lv_screen_active(), "Motion / waiting"));
    s_state.motion_settings.reference = APP_REFERENCE_READY;
    ++s_state.ui_revision; render();
    assert(has_visible_label(lv_screen_active(), "Motion / on"));
    /* Transport/sensor reasons override even an otherwise ready reference. */
    const struct { app_motion_reason_t reason; const char *hint; } blockers[] = {
        {APP_MOTION_DISCONNECTED, "Connect a Bluetooth input host."},
        {APP_MOTION_SENSOR_UNAVAILABLE, "Motion sensor unavailable."},
        {APP_MOTION_CALIBRATION_REQUIRED, "Calibrate sensor in Input settings."},
    };
    for (unsigned i = 0; i < sizeof(blockers) / sizeof(blockers[0]); ++i) {
        s_state.motion_reason = blockers[i].reason; s_state.motion_recenter_available = false;
        ++s_state.ui_revision; render();
        assert(has_visible_label(lv_screen_active(), blockers[i].hint));
        assert(has_visible_label(lv_screen_active(), "Motion / paused"));
        clear_intents(); tap_control("Recenter"); assert(action_count(APP_RECENTER) == 0);
        clear_intents(); tap_control("Motion / paused");
        assert(action_count(APP_MOTION) == 1 && last_action(APP_MOTION)->x == 0);
    }
    s_state.motion_reason = APP_MOTION_READY; s_state.motion_recenter_available = true;
    for (unsigned stale = 0; stale < 3; ++stale) {
        ++s_state.ui_revision; render(); clear_intents(); pointer(206, 32, true);
        app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
        if (stale == 0) ++s_state.epoch;
        else if (stale == 1) ++s_state.ui_revision;
        else s_state.motion_recenter_available = false;
        render(); pointer(206, 32, false); assert_terminal(&begin);
        assert(action_count(APP_RECENTER) == 0 && action_count(APP_CALIBRATE) == 0);
        s_state.motion_recenter_available = true;
    }
    for (unsigned gate = 0; gate < 3; ++gate) {
        s_state.locked = gate == 0; s_state.screen_off = gate == 1; s_state.remote_owned = gate == 2;
        s_state.motion_recenter_available = false;
        ++s_state.ui_revision; render(); clear_intents(); pointer(206, 32, true);
        s_state.locked = s_state.screen_off = s_state.remote_owned = false;
        s_state.motion_recenter_available = true;
        ++s_state.ui_revision; render(); pointer(206, 32, false);
        assert(action_count(APP_RECENTER) == 0);
    }
    const app_overlay_t covers[] = {APP_OVERLAY_MODES, APP_OVERLAY_LOCAL_EDITOR};
    for (unsigned i = 0; i < sizeof(covers) / sizeof(covers[0]); ++i) {
        render(); clear_intents(); pointer(206, 32, true);
        app_intent_t covered_begin = *last_action(APP_TOUCH_BEGIN);
        s_state.overlay = covers[i];
        s_state.local_editor = (app_local_editor_view_t){.session = 901 + i,
            .purpose = APP_LOCAL_EDIT_BLUETOOTH_NAME, .seconds_left = 300, .submit_available = true};
        ++s_state.ui_revision; render(); pointer(206, 32, false); assert_terminal(&covered_begin);
        assert(action_count(APP_RECENTER) == 0);
        clear_intents(); pointer(206, 32, true);
        s_state.overlay = APP_OVERLAY_NONE; ++s_state.ui_revision;
        render(); pointer(206, 32, false);
        assert(action_count(APP_RECENTER) == 0);
    }
    clear_intents(); pointer(206, 32, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.mode = INPUT_KEYBOARD; s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    grid_groups(); ++s_state.ui_revision; render(); pointer(206, 32, false); assert_terminal(&begin);
    assert(action_count(APP_RECENTER) == 0 && action_count(APP_MENU) == 0);
    assert_rect(control("Touch"), 140, 8, 76, 48);
    clear_intents(); tap_control("Touch");
    assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_KEYBOARD);
    s_state.remote_owned = true; ++s_state.ui_revision; render();
    clear_intents(); tap_control("Touch");
    assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_KEYBOARD);
    assert(action_count(APP_RECENTER) == 0);
    s_state.remote_owned = false;
    s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_INPUT; ++s_state.ui_revision;
    screenshot("input-sensor-calibration");
    clear_intents(); tap_control("Calibrate sensor");
    assert(action_count(APP_CALIBRATE) == 1 && action_count(APP_RECENTER) == 0);
    s_state = original; render(); clear_intents();
    puts("PASS: grip acquisition/recovery, truthful stoppable Motion state, distinct Recenter intent, captured admission and unchanged Mouse geometry");
}
