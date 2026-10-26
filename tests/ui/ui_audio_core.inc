// SPDX-License-Identifier: Apache-2.0
static void audio_assert_text_fits(const char *expected)
{
    lv_obj_t *value = find_visible_label(lv_screen_active(), expected);
    assert(value && strcmp(lv_label_get_text(value), expected) == 0);
    lv_point_t size;
    lv_text_get_size(&size, expected, lv_obj_get_style_text_font(value, 0),
        lv_obj_get_style_text_letter_space(value, 0), lv_obj_get_style_text_line_space(value, 0),
        lv_obj_get_content_width(value), LV_TEXT_FLAG_NONE);
    assert(size.x <= lv_obj_get_content_width(value) && size.y <= lv_obj_get_content_height(value));
}

static void audio_start_block_checks(void)
{
    ui_fixture_media(&s_state);
    s_state.media.microphone.can_start = s_state.media.speaker.can_start = false;
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_STREAM_REQUIRED;
    screenshot("audio-core-host-stream-required");
    audio_assert_text_fits("Off / Waiting\nWaiting for host audio");
    audio_assert_text_fits("Mic: Open Mic on host\nSpeaker: Play audio to this device on host");
    clear_intents(); tap_control("Start mic"); tap_control("Start speaker");
    assert(action_count(APP_MEDIA_CONTROL) == 0 && !s_state.media.live_capture.known && !s_state.media.playback.known);
    s_state.media.microphone.start_block = APP_MEDIA_START_CODEC_UNAVAILABLE;
    s_state.media.speaker.start_block = APP_MEDIA_START_HOST_UNAVAILABLE;
    screenshot("audio-core-distinct-start-blocks");
    audio_assert_text_fits("Off / Unavailable\nAudio hardware unavailable");
    audio_assert_text_fits("Off / Unavailable\nHost audio is not connected");
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_UNSUPPORTED;
    screenshot("audio-core-long-start-blocks");
    audio_assert_text_fits("Off / Unavailable\nMic unsupported on this connection");
    audio_assert_text_fits("Off / Unavailable\nSpeaker unsupported on this connection");
    audio_assert_text_fits("Mic: Mic unsupported on this connection\nSpeaker: Speaker unsupported on this connection");
    s_state.media.microphone.start_block = APP_MEDIA_START_UNKNOWN;
    s_state.media.speaker.start_block = (app_media_start_block_t)999;
    screenshot("audio-core-start-status-unknown");
    audio_assert_text_fits("Mic: Start status unavailable\nSpeaker: Start status unavailable");
    assert(!has_visible_label(lv_screen_active(), "Open Mic on host"));
    assert(!has_visible_label(lv_screen_active(), "Off / Ready"));
}

static void audio_host_settling_checks(void)
{
    ui_fixture_media(&s_state);
    s_state.media.microphone.can_start = s_state.media.speaker.can_start = false;
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_PREPARING;
    screenshot("audio-core-host-preparing");
    audio_assert_text_fits("Off / Waiting\nWaiting for host audio");
    audio_assert_text_fits("Mic: Open Mic on host\nSpeaker: Play audio to this device on host");
    for (unsigned history = 0; history < 2; ++history) {
        if (history) {
            s_state.media.live_capture = s_state.media.playback = (app_media_job_view_t){.known = true, .state = APP_MEDIA_JOB_COMPLETED};
            strcpy(s_state.media.live_capture.reason, "Previous Mic session ended.");
            strcpy(s_state.media.playback.reason, "Previous Speaker session ended.");
            screenshot("audio-core-host-waiting-retained-completion");
        }
        lv_tick_inc(1000); lv_timer_handler(); render();
        ui_memory_stats_t before = ui_memory_stats();
        uint64_t flushes = s_flush_count;
        for (unsigned i = 0; i < 100; ++i) {
            /* Independent role openings and stream replacements change owner
             * identity without making either idle role actionable. */
            s_state.media.microphone.start_block = i % 2 ? APP_MEDIA_START_HOST_STREAM_REQUIRED : APP_MEDIA_START_HOST_PREPARING;
            s_state.media.speaker.start_block = i % 3 ? APP_MEDIA_START_HOST_PREPARING : APP_MEDIA_START_HOST_STREAM_REQUIRED;
            ++s_state.media.resource_revision;
            render(); lv_tick_inc(100); lv_timer_handler();
            audio_assert_text_fits("Off / Waiting\nWaiting for host audio");
            audio_assert_text_fits("Mic: Open Mic on host\nSpeaker: Play audio to this device on host");
            assert(!has_visible_label(lv_screen_active(), "Off / Ready"));
            assert(!has_visible_label(lv_screen_active(), "Previous Mic session ended."));
            assert(!has_visible_label(lv_screen_active(), "Previous Speaker session ended."));
            assert(lv_obj_has_state(control("Start mic"), LV_STATE_DISABLED));
            assert(lv_obj_has_state(control("Start speaker"), LV_STATE_DISABLED));
        }
        ui_memory_stats_t after = ui_memory_stats();
        assert(after.allocations == before.allocations && after.reallocations == before.reallocations && after.frees == before.frees);
        assert(after.live_bytes == before.live_bytes && after.live_blocks == before.live_blocks);
        assert(s_flush_count == flushes && lv_anim_count_running() == 0);
    }
    s_state.media.live_capture = s_state.media.playback = (app_media_job_view_t){0};
    clear_intents(); tap_control("Start mic"); tap_control("Start speaker");
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_UNAVAILABLE;
    screenshot("audio-core-host-disconnected");
    audio_assert_text_fits("Off / Unavailable\nHost audio is not connected");
    assert(!has_visible_label(lv_screen_active(), "Waiting for host audio"));
    clear_intents(); tap_control("Start mic"); tap_control("Start speaker");
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_PREPARING;
    render();
    clear_intents(); pointer(380, 284, true);
    s_state.media.speaker.can_start = true; s_state.media.speaker.start_block = APP_MEDIA_START_ALLOWED;
    render(); pointer(380, 284, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0); /* Disabled press cannot become deferred consent. */
    pointer(380, 284, true);
    s_state.media.speaker.can_start = false; s_state.media.speaker.start_block = APP_MEDIA_START_HOST_STREAM_REQUIRED;
    ++s_state.media.resource_revision;
    render(); pointer(380, 284, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0); /* A ready press cannot retain stale admission. */
    assert(!has_visible_label(lv_screen_active(), "Off / Ready"));
    s_state.media.speaker.can_start = true; s_state.media.speaker.start_block = APP_MEDIA_START_ALLOWED;
    render(); audio_assert_text_fits("Off / Ready");
    tap_control("Start speaker"); assert(action_count(APP_MEDIA_CONTROL) == 1);
    s_state.media.speaker.can_start = false; s_state.media.speaker.start_block = APP_MEDIA_START_HOST_PREPARING;
    s_state.media.playback = (app_media_job_view_t){.known = true, .state = APP_MEDIA_JOB_ERROR};
    strcpy(s_state.media.playback.reason, "State changed. Review the current controls.");
    screenshot("audio-core-host-preparing-previous-error");
    audio_assert_text_fits("Error\nState changed. Review the current controls.");
    audio_assert_text_fits("Speaker: State changed. Review the current controls.");
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_STREAM_REQUIRED;
    render();
    audio_assert_text_fits("Error\nState changed. Review the current controls.");
    audio_assert_text_fits("Speaker: State changed. Review the current controls.");
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_HOST_PREPARING;
    s_state.media.playback = (app_media_job_view_t){.known = true, .id = {{83}}, .consumer_generation = 93,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.speaker.start_block = APP_MEDIA_START_OCCUPIED;
    s_state.media.speaker.muted = false;
    screenshot("audio-core-host-preparing-active-speaker");
    clear_intents(); pointer(380, 284, true);
    s_state.media.microphone.can_start = true; s_state.media.microphone.start_block = APP_MEDIA_START_ALLOWED;
    render(); pointer(380, 284, false);
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LIVE_STOP);
    ui_fixture_media(&s_state);
    s_state.media.live_capture = (app_media_job_view_t){.known = true, .id = {{81}}, .consumer_generation = 91,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.microphone.can_start = false; s_state.media.microphone.muted = false; s_state.media.microphone.can_mute = true;
    s_state.media.speaker.can_start = false; s_state.media.speaker.start_block = APP_MEDIA_START_HOST_PREPARING;
    screenshot("audio-core-host-preparing-active-mic");
    clear_intents(); tap_control("Mute mic");
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_SET_MUTED &&
        last_action(APP_MEDIA_CONTROL)->media.value.role.value);
    puts("PASS: host waiting with/without completed jobs stays stable; 200 snapshots, zero flushes/allocations, readiness/disconnect/errors remain authoritative");
}

static void audio_core_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_AUDIO; s_state.overlay = APP_OVERLAY_NONE;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.audio_active = s_state.listening = s_state.capture_indicator_required = false;
    s_state.media = (app_media_view_t){0}; ++s_state.ui_revision;
    screenshot("audio-core-unknown");
    assert(has_visible_label(lv_screen_active(), "Mic") && has_visible_label(lv_screen_active(), "Speaker"));
    assert(has_visible_label(lv_screen_active(), "Status unknown"));
    assert(!find_control(lv_screen_active(), "Voice") && !find_control(lv_screen_active(), "Routes and roles"));
    assert(!find_control(lv_screen_active(), "Record") && !find_control(lv_screen_active(), "Recordings"));
    assert_rect(control("Start mic"), 316, 80, 136, 48);
    assert_rect(control("Start speaker"), 316, 260, 136, 48);
    assert_rect(control("Mute mic"), 28, 168, 224, 48);
    clear_intents(); tap_control("Start mic"); tap_control("Start speaker"); tap_control("Mute mic");
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    audio_start_block_checks();
    audio_host_settling_checks();
    ui_fixture_media(&s_state); screenshot("audio-core-usb-ready");
    assert(has_visible_label(lv_screen_active(), "Off / Ready"));
    assert(!has_visible_label(lv_screen_active(), "On")); /* Enabled hardware alone is not a running job. */
    clear_intents(); tap_control("Start mic");
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LIVE_START);
    assert(last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_MICROPHONE);
    assert(!s_state.media.live_capture.known);
    clear_intents(); tap_control("Start speaker");
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_SPEAKER);
    assert(!s_state.media.playback.known && action_count(APP_AUDIO_KIND) == 0);
    s_state.audio_kind = APP_AUDIO_SPEAKER; render(); clear_intents(); tap_control("Start mic");
    assert(last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_MICROPHONE);
    s_state.media.live_capture = (app_media_job_view_t){.known = true, .id = {{81}}, .consumer_generation = 91,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.state = APP_MEDIA_RUNNING;
    s_state.media.microphone.can_start = false; s_state.media.microphone.can_unmute = true;
    s_state.media.microphone.start_block = APP_MEDIA_START_OCCUPIED;
    s_state.capture_indicator_required = true; s_state.capture_indicator_generation = 19; s_state.capture_state = APP_CAPTURE_ACTIVE;
    screenshot("audio-core-mic-muted");
    assert(has_visible_label(lv_screen_active(), "On / Muted"));
    assert(!has_visible_label(lv_screen_active(), "Audio resource in use"));
    clear_intents(); tap_control("Stop mic");
    assert(last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LIVE_STOP &&
        last_action(APP_MEDIA_CONTROL)->media.job_id.bytes[0] == 81 && last_action(APP_MEDIA_CONTROL)->media.consumer_generation == 91);
    clear_intents(); tap_control("Unmute mic");
    assert(last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_SET_MUTED &&
        last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_MICROPHONE && !last_action(APP_MEDIA_CONTROL)->media.value.role.value);
    assert(s_state.media.microphone.muted);
    s_state.media.speaker.can_start = false;
    s_state.media.speaker.start_block = APP_MEDIA_START_HOST_STREAM_REQUIRED;
    screenshot("audio-core-active-mic-blocked-speaker");
    audio_assert_text_fits("Speaker: Play audio to this device on host");
    assert(has_visible_label(lv_screen_active(), "On / Muted") && find_control(lv_screen_active(), "Stop mic"));
    clear_intents(); tap_control("Start speaker"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.microphone.mute_confirmed = false; screenshot("audio-core-mute-unknown");
    assert(has_visible_label(lv_screen_active(), "On / Mute unconfirmed"));
    clear_intents(); tap_control("Mute mic / unconfirmed"); assert(last_action(APP_MEDIA_CONTROL)->media.value.role.value);
    s_state.media.microphone.mute_confirmed = true; s_state.media.microphone.muted = false;
    s_state.media.playback = (app_media_job_view_t){.known = true, .id = {{83}}, .consumer_generation = 93,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.speaker.muted = false; s_state.media.speaker.can_start = false;
    s_state.media.speaker.start_block = APP_MEDIA_START_HOST_STREAM_REQUIRED;
    screenshot("audio-core-both-running");
    clear_intents(); tap_control("Stop speaker");
    assert(last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_SPEAKER &&
        last_action(APP_MEDIA_CONTROL)->media.job_id.bytes[0] == 83 && last_action(APP_MEDIA_CONTROL)->media.consumer_generation == 93);
    clear_intents(); pointer(380, 284, true); ++s_state.media.playback.consumer_generation; render(); pointer(380, 284, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.playback.state = APP_MEDIA_JOB_STOPPING; s_state.media.playback.can_stop = false;
    screenshot("audio-core-speaker-stopping");
    clear_intents(); tap_control("Stop speaker"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.playback.state = APP_MEDIA_JOB_ERROR;
    strcpy(s_state.media.playback.reason, "USB host stopped sending audio.");
    screenshot("audio-core-speaker-fault");
    assert(has_visible_label(lv_screen_active(), "Error\nUSB host stopped sending audio."));
    assert(!has_visible_label(lv_screen_active(), "Play audio to this device on host"));
    assert(find_control(lv_screen_active(), "Stop mic"));
    strcpy(s_state.media.playback.reason, "The host stopped sending audio. Select this device for playback, then start audio on the host.");
    screenshot("audio-core-long-job-reason");
    audio_assert_text_fits("Speaker: The host stopped sending audio. Select this device for playback, then start audio on the host.");
    assert(find_control(lv_screen_active(), "Stop mic"));
    ui_fixture_media(&s_state);
    s_state.media.recording = (app_media_job_view_t){.known = true, .state = APP_MEDIA_JOB_RUNNING};
    s_state.media.state = APP_MEDIA_RUNNING;
    s_state.media.microphone.can_start = false;
    screenshot("audio-core-recording-in-use");
    assert(has_visible_label(lv_screen_active(), "In use / Recording"));
    clear_intents(); tap_control("Start mic"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    ui_fixture_media(&s_state); s_state.capture_indicator_required = false; render();
    clear_intents(); pointer(180, 382, true); pointer(220, 382, true); pointer(220, 382, false);
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_SET_VOLUME);
    assert(last_action(APP_MEDIA_CONTROL)->media.value.level <= s_state.media.safe_volume_max && s_state.media.volume_percent == 35);
    render(); clear_intents(); pointer(180, 382, true); pointer(430, 382, true); pointer(430, 382, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0); /* Unsafe volume cannot be submitted. */
    s_state.media.command_pending = true;
    s_state.media.microphone.can_start = s_state.media.speaker.can_start = false;
    s_state.media.microphone.start_block = s_state.media.speaker.start_block = APP_MEDIA_START_PENDING;
    screenshot("audio-core-pending");
    assert(has_visible_label(lv_screen_active(), "Updating\nAudio change in progress"));
    clear_intents(); tap_control("Start mic"); tap_control("Start speaker"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    ui_fixture_media(&s_state); s_state.media.speaker.can_start = false;
    screenshot("audio-core-speaker-unavailable");
    clear_intents(); tap_control("Start speaker"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    clear_intents(); tap_control("USB");
    assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_MEDIA_OPTIONS);
    s_state.overlay = APP_OVERLAY_MEDIA_OPTIONS; ++s_state.ui_revision;
    screenshot("audio-connection-usb");
    assert_rect(control("USB"), 12, 64, 224, 56); assert_rect(control("Bluetooth"), 244, 64, 224, 56);
    assert(!find_control(lv_screen_active(), "Local") && !find_control(lv_screen_active(), "Bluetooth calls"));
    clear_intents(); tap_control("Bluetooth");
    const app_media_request_t *request = &last_action(APP_MEDIA_CONTROL)->media;
    assert(request->operation == APP_MEDIA_SELECT_CONNECTION && request->value.connection == APP_MEDIA_CONNECTION_BLUETOOTH);
    assert(request->management_generation == 31 && request->audio_generation == 47 && request->resource_revision == 59);
    assert(s_state.media.connection == APP_MEDIA_CONNECTION_USB && action_count(APP_AUDIO_KIND) == 0);
    clear_intents(); pointer(352, 92, true); ++s_state.media.resource_revision; render(); pointer(352, 92, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.connection = APP_MEDIA_CONNECTION_BLUETOOTH; s_state.media.route = APP_MEDIA_ROUTE_HFP;
    s_state.media.can_select_peer = true; s_state.media.next_peer_id.bytes[15] = 93;
    s_state.media.routes[APP_MEDIA_ROUTE_HFP].connected = true;
    s_state.media.routes[APP_MEDIA_ROUTE_HFP].can_disconnect = true;
    screenshot("audio-connection-bluetooth");
    clear_intents(); tap_control("Synthetic audio peer");
    assert(last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_SELECT_PEER && last_action(APP_MEDIA_CONTROL)->media.value.peer_id.bytes[15] == 93);
    clear_intents(); tap_control("Connect"); assert(last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LINK_CONNECT);
    clear_intents(); tap_control("Disconnect"); assert(last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LINK_DISCONNECT);
    s_state.media.command_pending = true; screenshot("audio-connection-pending");
    clear_intents(); tap_control("USB"); tap_control("Connect"); tap_control("Disconnect"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.command_pending = false; s_state.media.can_select_connection[0] = false; render();
    clear_intents(); tap_control("USB"); assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.media.connection_known = false; screenshot("audio-connection-unknown");
    assert(has_visible_label(lv_screen_active(), "No confirmed selection"));
    s_state.media.connection_known = true; s_state.media.connection = (app_media_connection_t)99;
    screenshot("audio-connection-invalid"); assert(has_visible_label(lv_screen_active(), "No confirmed selection"));
    clear_intents(); tap_control("Back to Audio"); assert(action_count(APP_CLOSE) == 1);
    s_state.overlay = APP_OVERLAY_NONE; ++s_state.ui_revision; ui_fixture_media(&s_state); render();
    clear_intents(); pointer(380, 104, true); ++s_state.ui_revision; render(); pointer(380, 104, false);
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state.screen_off = true; render(); clear_intents(); pointer(380, 104, true);
    s_state.screen_off = false; render(); pointer(380, 104, false);
    assert(action_count(APP_TOUCH_BEGIN) == 1 && action_count(APP_MEDIA_CONTROL) == 0);
    s_state.locked = true; render(); clear_intents(); tap(380, 104); assert(action_count(APP_MEDIA_CONTROL) == 0);
    s_state = original; render(); clear_intents();
    puts("PASS: independent Mic/Speaker, observed jobs and privacy, safe volume, owner-selected USB/Bluetooth, real hits and stale/wake/lock rejection");
}
