// SPDX-License-Identifier: Apache-2.0
static void navigation_reports_use_both_transports_without_editing_draft(void)
{
    for (int transport = APP_INPUT_BLUETOOTH; transport <= APP_INPUT_USB; ++transport) {
        transport_setup(INPUT_KEYBOARD, (app_input_transport_t)transport);
        assert(interaction_append(&s_interaction, 'K') && interaction_append(&s_interaction, 'E'));
        action(APP_MODE, INPUT_MOUSE);
        release_barrier();
        s_settings.navigation = true;
        s_host_os = APP_HOST_CTRL; /* Back is always Command+[ independently of draft shortcuts. */
        uint64_t epoch = s_engine.state.epoch;
        const uint8_t home[] = {4, 0, 0, 0}, zero_mouse[] = {0, 0, 0, 0};
        const uint8_t back[] = {8, 0, 0x2f, 0, 0, 0, 0, 0}, zero_key[8] = {0};
        action(APP_NAV_HOME, 0);
        expect_report(2, home, sizeof home);
        assert(trace[trace_count - 1].transport == (app_input_transport_t)transport);
        expect_report(2, zero_mouse, sizeof zero_mouse);
        action(APP_NAV_BACK, 0);
        expect_report(1, back, sizeof back);
        assert(trace[trace_count - 1].transport == (app_input_transport_t)transport);
        expect_report(1, zero_key, sizeof zero_key);
        assert(s_engine.state.mode == INPUT_MOUSE && s_engine.state.epoch == epoch);
        assert(!strcmp(s_interaction.view.preview, "KE") && !s_send_token);
        assert(!s_engine.text_pending && !s_engine.text_accepted && !s_engine.text_completed);
        expect_idle();
    }
}

static void navigation_admission_rejects_stale_held_and_covered_input(void)
{
    for (unsigned gate = 0; gate < 13; ++gate) {
        transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
        s_settings.navigation = true;
        app_intent_t home = make_intent(APP_NAV_HOME, 0, 0, NULL);
        switch (gate) {
        case 0: s_settings.navigation = false; break;
        case 1: s_drag_requested = true; break;
        case 2: s_engine.mouse_buttons = 1; break;
        case 3: s_engine.state.locked = true; break;
        case 4: s_engine.state.owner = INPUT_REMOTE; break;
        case 5: s_transport_switch_pending = true; break;
        case 6: s_engine.congested = true; break;
        case 7: s_engine.ready = false; break;
        case 8: s_idle.asleep = true; break;
        case 9: s_update_maintenance = true; break;
        case 10: s_overlay = APP_OVERLAY_SETTINGS; break;
        case 11: --home.epoch; break;
        case 12: --home.ui_revision; break;
        default: assert(false);
        }
        deliver(home);
        home.action = APP_NAV_BACK;
        deliver(home);
        assert(!s_engine.count && !trace_count);
    }
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    s_settings.navigation = true;
    action(APP_DRAG, 1);
    const size_t drag_count = s_engine.count;
    action(APP_NAV_HOME, 0); action(APP_NAV_BACK, 0);
    assert(s_engine.count == drag_count); /* Queued drag also blocks both actions. */
    cancel_input(); release_barrier(); expect_idle();
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    s_settings.navigation = true;
    app_intent_t old_target = make_intent(APP_NAV_HOME, 0, 0, NULL);
    ++test_link.generation;
    input_link_t replacement = input_link_snapshot(&test_link);
    observe_input_link(&replacement, &test_previous_link);
    deliver(old_target);
    release_barrier(); expect_idle();
    for (size_t i = 0; i < trace_count; ++i) assert(trace[i].bytes[0] == 0);
}

static void navigation_failed_release_recovers_without_replaying_action(void)
{
    for (int transport = APP_INPUT_BLUETOOTH; transport <= APP_INPUT_USB; ++transport) {
        for (unsigned home = 0; home < 2; ++home) {
            transport_setup(INPUT_MOUSE, (app_input_transport_t)transport);
            s_settings.navigation = true;
            action(home ? APP_NAV_HOME : APP_NAV_BACK, 0);
            assert(step() == HID_ENGINE_STEP_SENT);
            if (transport == APP_INPUT_USB) test_usb_send_failure = true;
            else test_send_failure = true;
            assert(step() == HID_ENGINE_STEP_SEND_FAILED);
            if (transport == APP_INPUT_USB) test_usb_send_failure = false;
            else test_send_failure = false;
            assert(s_engine.state.release_pending && !s_engine.count);
            release_barrier(); expect_idle();
            assert(trace_count == 3); /* One accepted down, then only the two recovery zeros. */
        }
    }
}

static void navigation_setting_persists_and_input_reset_grip_keeps_motion_off(void)
{
    setup(INPUT_MOUSE);
    assert(!s_settings.navigation);
    settings_page(APP_SETTINGS_NAVIGATION);
    action(APP_NAVIGATION, 1);
    assert(s_settings.navigation && test_storage_requests == 1 && test_saved_request.navigation);
    release_barrier();
    render_view(&test_link, ESP_OK);
    assert(test_view.navigation_enabled && !test_view.navigation_available);
    action(APP_NAVIGATION, 2);
    assert(test_storage_requests == 1 && s_settings.navigation);
    action(APP_CLOSE, 0);
    render_view(&test_link, ESP_OK);
    assert(test_view.navigation_available);
    calibrate(); action(APP_MOTION, 0); settle_motion_reference();
    const float bias = s_interaction.bias[0];
    settings_page(APP_SETTINGS_INPUT);
    assert(!s_engine.state.gyro_enabled);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_recenter_available);
    action(APP_RECENTER, 0);
    assert(!s_engine.state.gyro_enabled && !s_interaction.view.motion_enabled);
    assert(!s_interaction.gravity_ready && s_interaction.bias[0] == bias && s_interaction.view.calibrated);
    assert(!s_engine.count);
}

#define NAVIGATION_CASES(RUN) \
    RUN(navigation_reports_use_both_transports_without_editing_draft); \
    RUN(navigation_admission_rejects_stale_held_and_covered_input); \
    RUN(navigation_failed_release_recovers_without_replaying_action); \
    RUN(navigation_setting_persists_and_input_reset_grip_keeps_motion_off)
