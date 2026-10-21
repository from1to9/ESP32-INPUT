// SPDX-License-Identifier: Apache-2.0
/* Presentation arming is observed separately from the real native flush. */
static void capture_indicator_checks(void)
{
    app_view_t original = s_state;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.listening = s_state.audio_active = false;
    s_state.microphone_muted = true;
    s_state.capture_indicator_required = true;
    s_state.capture_indicator_generation = 77;
    s_state.capture_state = APP_CAPTURE_ACTIVE;
    s_state.settings_page = APP_SETTINGS_HOME;
    ui_fixture_native_networks(&s_state);
    const app_overlay_t overlays[] = {APP_OVERLAY_NONE, APP_OVERLAY_MODES, APP_OVERLAY_TARGET,
        APP_OVERLAY_SETTINGS, APP_OVERLAY_WIFI_SETUP, APP_OVERLAY_CONFIRM, APP_OVERLAY_KEYBOARD,
        APP_OVERLAY_AUDIO, APP_OVERLAY_WIFI_NETWORKS, APP_OVERLAY_WIFI_NETWORK,
        APP_OVERLAY_COMMON_KEYS, APP_OVERLAY_LOCAL_EDITOR, APP_OVERLAY_RECORDING_STORAGE,
        APP_OVERLAY_MEDIA_OPTIONS, APP_OVERLAY_RECORDING_START, APP_OVERLAY_RECORDINGS, APP_OVERLAY_RECORDING_DETAIL,
        APP_OVERLAY_API_CONNECTION, APP_OVERLAY_API_REVIEW, APP_OVERLAY_WIFI_PROFILES, APP_OVERLAY_BLUETOOTH_TARGET};
    uint64_t continuous_revision = 0;
    for (unsigned i = 0; i < sizeof(overlays) / sizeof(overlays[0]); ++i) {
        s_state.overlay = overlays[i];
        s_state.mode = i % 3;
        s_state.confirmation = APP_CONFIRM_WIFI_FORGET;
        s_state.local_editor = (app_local_editor_view_t){.purpose = APP_LOCAL_EDIT_BLUETOOTH_NAME,
            .session = 991 + i, .submit_available = true, .seconds_left = 60};
        strcpy(s_state.local_editor.bluetooth_initial, "Synthetic");
        ++s_state.ui_revision;
        char name[64]; snprintf(name, sizeof(name), "capture-indicator-overlay-%u", i);
        screenshot(name);
        assert(app_ui_capture_indicator_armed_generation() == 77);
        app_ui_capture_indicator_t indicator = app_ui_capture_indicator();
        assert(indicator.armed && indicator.indicator_revision);
        assert(indicator.x1 == 280 && indicator.y1 == 8 && indicator.x2 == 395 && indicator.y2 == 55);
        if (continuous_revision) assert(indicator.indicator_revision == continuous_revision);
        continuous_revision = indicator.indicator_revision;
        assert(has_visible_label(lv_screen_active(), "Mic active"));
        clear_intents(); tap(338, 32);
        assert(action_count(APP_LOCK) == 1);
    }
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.mode = INPUT_MOUSE;
    ++s_state.ui_revision;
    const app_capture_state_t states[] = {APP_CAPTURE_STARTING, APP_CAPTURE_STOPPING,
        APP_CAPTURE_UNCERTAIN, APP_CAPTURE_UNKNOWN};
    const char *captions[] = {"Mic starting", "Mic stopping", "Mic uncertain", "Mic unknown"};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        s_state.capture_state = states[i];
        char name[64]; snprintf(name, sizeof(name), "capture-indicator-state-%u", i);
        screenshot(name);
        assert(has_visible_label(lv_screen_active(), captions[i]));
        assert(app_ui_capture_indicator_armed_generation() == 77);
    }
    /* Replacing a generation with identical pixels still schedules a frame.
     * The getter is an armed value before flush, never acknowledgement. */
    uint64_t before = s_flush_count;
    ++s_state.capture_indicator_generation;
    app_ui_render(&s_state);
    assert(app_ui_capture_indicator_armed_generation() == 78 && s_flush_count == before);
    assert(app_ui_capture_indicator().indicator_revision > continuous_revision);
    lv_refr_now(NULL);
    assert(s_flush_count > before);
    before = s_flush_count;
    app_ui_render(&s_state);
    lv_refr_now(NULL);
    assert(s_flush_count == before);
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_INPUT;
    ++s_state.ui_revision; render();
    pointer(120, 342, true);
    s_state.overlay = APP_OVERLAY_CONFIRM; ++s_state.ui_revision;
    s_state.capture_state = APP_CAPTURE_UNCERTAIN;
    ++s_state.capture_indicator_generation;
    render();
    assert(has_visible_label(lv_screen_active(), "Mic uncertain"));
    assert(app_ui_capture_indicator_armed_generation() == 79);
    screenshot("capture-indicator-deferred-panel");
    pointer(120, 342, false); render();
    s_state.locked = true; ++s_state.ui_revision;
    screenshot("capture-indicator-locked-uncertain");
    assert(has_visible_label(lv_screen_active(), "Microphone stop unconfirmed"));
    assert(app_ui_capture_indicator_armed_generation() == 79);
    app_ui_capture_indicator_t locked = app_ui_capture_indicator();
    assert(locked.x1 == 0 && locked.y1 == 0 && locked.x2 == 479 && locked.y2 == 479);
    s_state.screen_off = true;
    screenshot("capture-indicator-screen-off");
    assert(app_ui_capture_indicator_armed_generation() == 0);
    s_state.screen_off = false;
    s_state.capture_indicator_generation = 0;
    screenshot("capture-indicator-generation-unknown");
    assert(has_visible_label(lv_screen_active(), "Microphone stop unconfirmed"));
    assert(app_ui_capture_indicator_armed_generation() == 0);
    s_state.capture_indicator_required = false;
    s_state.capture_state = APP_CAPTURE_OFF_CONFIRMED;
    screenshot("capture-indicator-locked-off-confirmed");
    assert(has_visible_label(lv_screen_active(), "Microphone off (confirmed)"));
    assert(app_ui_capture_indicator_armed_generation() == 0);
    s_state.capture_state = APP_CAPTURE_UNKNOWN;
    screenshot("capture-indicator-locked-unknown");
    assert(has_visible_label(lv_screen_active(), "Microphone status unavailable"));
    s_state = original; render(); clear_intents();
    puts("PASS: capture warning and Stop across all panels, lock/stop uncertainty, deferred navigation and generation-bound redraw arming");
}
