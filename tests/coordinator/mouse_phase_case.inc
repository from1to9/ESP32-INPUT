// SPDX-License-Identifier: Apache-2.0
static jmp_buf phase_done;
static unsigned phase_turns;

static void phase_select(input_mode_t mode)
{
    app_intent_t intent = make_intent(APP_MODE, (int)mode, 0, NULL);
    assert(submit_intent(&intent));
}

static void phase_pad(app_action_t action, int x)
{
    app_intent_t intent = make_intent(action, x, 100, NULL);
    intent.surface = APP_SURFACE_TOUCHPAD;
    intent.gesture = 9002;
    assert(submit_intent(&intent));
}

static void phase_delay_fifo(void)
{
    test_before_intent_receive = NULL;
    test_clock_ms += 36;
}

static void phase_coordinator_delay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(COORDINATOR_PERIOD_MS));
    test_clock_ms += COORDINATOR_PERIOD_MS;
    switch (++phase_turns) {
    case 3:
        assert(!app_mouse_phase_active());
        assert(test_diagnostic_log_count == 1);
        phase_select(INPUT_MOUSE);
        break;
    case 7:
        assert(app_mouse_phase_active());
        assert(s_engine.state.mode == INPUT_MOUSE && !s_engine.state.release_pending);
        assert(test_diagnostic_log_count == 1);
        phase_pad(APP_TOUCH_BEGIN, 100);
        break;
    case 8:
        phase_pad(APP_TOUCH_MOVE, 12);
        test_before_intent_receive = phase_delay_fifo;
        break;
    case 9:
        assert(trace_count && trace[trace_count - 1].id == HID_ENGINE_MOUSE_REPORT_ID);
        assert((int8_t)trace[trace_count - 1].bytes[1] > 0);
        test_clock_ms += 2000;
        ++test_link.generation;
        break;
    case 10:
        assert(app_mouse_phase_active());
        test_clock_ms += 20000;
        break;
    case 15:
        assert(!app_mouse_phase_active());
        phase_select(INPUT_AUDIO);
        break;
    case 16:
        assert(s_engine.state.mode == INPUT_AUDIO);
        phase_select(INPUT_MOUSE);
        break;
    case 28:
        assert(s_engine.state.mode == INPUT_MOUSE && !app_mouse_phase_active());
        longjmp(phase_done, 1);
    }
}

static void mouse_phase_observes_local_transition_and_stall_once(void)
{
    transport_setup(INPUT_AUDIO, APP_INPUT_BLUETOOTH);
    /* Boot and the shared mode transition (used by remote) do not arm it. */
    assert(select_mode(INPUT_MOUSE));
    assert(!app_mouse_phase_active());
    assert(select_mode(INPUT_AUDIO));
    assert(!app_mouse_phase_active());
    phase_turns = 0;
    test_mouse_phase_logs_enabled = true;
    test_diagnostic_log_count = 0;
    test_clock_override = true;
    test_clock_ms = 1000;
    if (setjmp(phase_done) == 0) {
        test_coordinator_delay = phase_coordinator_delay;
        coordinator_run(ESP_OK);
    }
    test_coordinator_delay = NULL;
    test_clock_override = false;
    test_mouse_phase_logs_enabled = false;
    assert(test_diagnostic_log_count == 12); /* Arm, header and exactly ten slots. */
    assert(strstr(test_diagnostic_logs[1], "start_us=1030000 end_us=21030000 observed_end_us=23136000"));
    assert(strstr(test_diagnostic_logs[2], "slot=0 "));
    assert(strstr(test_diagnostic_logs[2], "2010000/0/36000/0/0/0/0/0/0"));
    assert(strstr(test_diagnostic_logs[2], "pad[begin/move/end/cancel]=1/1/0/0"));
    assert(strstr(test_diagnostic_logs[2], "enqueue[ok/fail]=1/0 send[ok/fail]=1/0"));
    assert(strstr(test_diagnostic_logs[3], "slot=1 "));
    assert(strstr(test_diagnostic_logs[3], "cancel[link/expiry/priority/reset/input]=1/0/0/1/1"));
    assert(strstr(test_diagnostic_logs[11], "slot=9 turns=0 states=0x0"));
}
