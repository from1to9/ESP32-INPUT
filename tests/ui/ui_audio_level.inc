// SPDX-License-Identifier: Apache-2.0
static size_t audio_level_object_count(lv_obj_t *object)
{
    size_t count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += audio_level_object_count(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static void audio_level_value(int dbfs, unsigned percent)
{
    s_state.media.microphone_level = (app_media_mic_level_t){
        .state = APP_MEDIA_LEVEL_LIVE, .dbfs = (int16_t)dbfs, .percent = (uint8_t)percent};
}

static void audio_level_expect(const char *caption, int percent)
{
    audio_assert_text_fits(caption);
    lv_obj_t *bar = find_widget(lv_screen_active(), &lv_bar_class);
    assert(bar && lv_bar_get_value(bar) == percent);
    assert_rect(bar, 268, 200, 184, 12);
    assert_rect(find_visible_label(lv_screen_active(), caption), 268, 170, 184, 20);
    assert(!lv_obj_has_flag(bar, LV_OBJ_FLAG_CLICKABLE));
    assert_rect(control(s_state.media.microphone.mute_confirmed && s_state.media.microphone.muted ?
        "Unmute mic" : s_state.media.microphone.mute_confirmed ? "Mute mic" : "Mute mic / unconfirmed"), 28, 168, 224, 48);
}

static void audio_level_quiet_updates(unsigned variant)
{
    app_view_t original = s_state;
    if (variant == 1) s_state.mode = INPUT_MOUSE;
    if (variant == 2) s_state.mode = INPUT_KEYBOARD;
    if (variant == 3) s_state.overlay = APP_OVERLAY_MEDIA_OPTIONS;
    if (variant == 4) s_state.locked = true;
    if (variant == 5) s_state.screen_off = true;
    if (variant >= 6) s_state.media.microphone_level.state = variant == 6 ? APP_MEDIA_LEVEL_OFF :
        variant == 7 ? APP_MEDIA_LEVEL_WAITING : APP_MEDIA_LEVEL_UNKNOWN;
    render();
    lv_tick_inc(1000); lv_timer_handler(); render();
    ui_memory_stats_t before = ui_memory_stats();
    uint64_t flushes = s_flush_count;
    size_t objects = audio_level_object_count(lv_screen_active());
    for (unsigned i = 0; i < 100; ++i) {
        if (variant) {
            s_state.media.microphone_level.dbfs = (int16_t)(-(int)(i % 61));
            s_state.media.microphone_level.percent = (uint8_t)(i % 101);
            s_state.media.microphone_level.clipping = i % 2 != 0;
        }
        render(); lv_tick_inc(100); lv_timer_handler();
    }
    ui_memory_stats_t after = ui_memory_stats();
    assert(after.allocations == before.allocations && after.reallocations == before.reallocations && after.frees == before.frees);
    assert(after.live_bytes == before.live_bytes && after.live_blocks == before.live_blocks);
    assert(s_flush_count == flushes && audio_level_object_count(lv_screen_active()) == objects);
    s_state = original; render();
}

static void audio_level_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_AUDIO; s_state.overlay = APP_OVERLAY_NONE;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.capture_indicator_required = s_state.audio_active = s_state.listening = false;
    ui_fixture_media(&s_state); ++s_state.ui_revision;
    screenshot("audio-level-unknown"); audio_level_expect("Level unavailable", 0);
    s_state.media.microphone_level = (app_media_mic_level_t){.state = APP_MEDIA_LEVEL_OFF,
        .dbfs = 127, .percent = 255, .clipping = true};
    screenshot("audio-level-off"); audio_level_expect("Mic off", 0);
    s_state.media.live_capture = (app_media_job_view_t){.known = true, .id = {{81}}, .consumer_generation = 91,
        .state = APP_MEDIA_JOB_STARTING, .can_stop = true};
    s_state.media.microphone.can_start = false; s_state.media.microphone.muted = false;
    s_state.capture_indicator_required = true; s_state.capture_indicator_generation = 19; s_state.capture_state = APP_CAPTURE_STARTING;
    s_state.media.microphone_level.state = APP_MEDIA_LEVEL_WAITING;
    screenshot("audio-level-waiting"); audio_level_expect("Waiting for signal", 0);
    s_state.media.live_capture.state = APP_MEDIA_JOB_RUNNING;
    s_state.media.state = APP_MEDIA_RUNNING; s_state.capture_state = APP_CAPTURE_ACTIVE;
    audio_level_value(-24, 60);
    screenshot("audio-level-live"); audio_level_expect("Input -24 dBFS", 60);
    audio_level_value(-96, 0);
    screenshot("audio-level-below-bar-floor"); audio_level_expect("Input -96 dBFS", 0);
    assert(!has_visible_label(lv_screen_active(), "Quiet"));
    s_state.media.microphone_level.silent = true; s_state.media.microphone_level.dbfs = 127;
    screenshot("audio-level-quiet"); audio_level_expect("Input: Quiet", 0);
    audio_level_value(0, 100);
    screenshot("audio-level-full-scale"); audio_level_expect("Input 0 dBFS", 100);
    s_state.media.microphone_level.clipping = true;
    screenshot("audio-level-clipping"); audio_level_expect("Too loud / 0 dBFS", 100);
    const app_media_mic_level_t invalid[] = {
        {.state = (app_media_level_state_t)99},
        {.state = APP_MEDIA_LEVEL_LIVE, .dbfs = 1, .percent = 50},
        {.state = APP_MEDIA_LEVEL_LIVE, .dbfs = -97, .percent = 50},
        {.state = APP_MEDIA_LEVEL_LIVE, .dbfs = -20, .percent = 101},
        {.state = APP_MEDIA_LEVEL_LIVE, .silent = true, .clipping = true}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        s_state.media.microphone_level = invalid[i]; render(); audio_level_expect("Level unavailable", 0);
    }
    screenshot("audio-level-invalid");
    audio_level_value(-12, 80); s_state.media.known = false;
    render(); audio_level_expect("Level unavailable", 0); s_state.media.known = true;
    s_state.media.live_capture = (app_media_job_view_t){.known = true, .id = {{81}}, .consumer_generation = 91,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.playback = (app_media_job_view_t){.known = true, .id = {{83}}, .consumer_generation = 93,
        .state = APP_MEDIA_JOB_RUNNING, .can_stop = true};
    s_state.media.microphone.can_start = s_state.media.speaker.can_start = false;
    s_state.media.microphone.muted = false; s_state.media.microphone.mute_confirmed = false;
    s_state.capture_indicator_required = true; s_state.capture_indicator_generation = 19; s_state.capture_state = APP_CAPTURE_ACTIVE;
    strcpy(s_state.media.live_capture.reason, "Microphone mute has not been confirmed. Use Stop mic before changing the audio connection.");
    audio_level_value(-96, 0); s_state.media.microphone_level.clipping = true;
    screenshot("audio-level-long-reason-clipping"); audio_level_expect("Too loud / -96 dBFS", 0);
    audio_assert_text_fits("Mute mic / unconfirmed");
    audio_assert_text_fits("Mic: Microphone mute has not been confirmed. Use Stop mic before changing the audio connection.");
    assert(find_control(lv_screen_active(), "Stop mic") && has_visible_label(lv_screen_active(), "Mic active"));
    s_state.media.microphone.mute_confirmed = true; s_state.media.live_capture.reason[0] = '\0'; render();

    /* Meter-only snapshots must preserve a real held gesture and its original
     * action/job identity. The meter cannot become a control or gain focus. */
    const int controls[][2] = {{380,104}, {140,192}, {380,284}, {436,32}};
    for (unsigned held = 0; held < 4; ++held) {
        clear_intents(); pointer(controls[held][0], controls[held][1], true);
        for (unsigned i = 0; i < 10; ++i) { audio_level_value(-(int)i * 6, 100 - i * 10); render(); }
        assert(action_count(APP_MEDIA_CONTROL) == 0 && action_count(APP_MENU) == 0);
        pointer(controls[held][0], controls[held][1], false);
        assert(action_count(APP_TOUCH_BEGIN) == 1 && action_count(APP_TOUCH_END) == 1 && action_count(APP_TOUCH_CANCEL) == 0);
        if (held == 3) assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_MODES);
        else {
            assert(action_count(APP_MEDIA_CONTROL) == 1);
            const app_media_request_t *request = &last_action(APP_MEDIA_CONTROL)->media;
            assert(request->operation == (held == 1 ? APP_MEDIA_SET_MUTED : APP_MEDIA_LIVE_STOP));
            if (held == 0) assert(request->job_id.bytes[0] == 81 && request->consumer_generation == 91);
            if (held == 1) assert(request->value.role.role == APP_MEDIA_MICROPHONE && request->value.role.value);
            if (held == 2) assert(request->job_id.bytes[0] == 83 && request->consumer_generation == 93);
        }
    }
    clear_intents(); pointer(180,382,true); pointer(220,382,true);
    audio_level_value(-3,95); render(); pointer(220,382,false);
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_SET_VOLUME);
    assert(last_action(APP_MEDIA_CONTROL)->media.value.level <= s_state.media.safe_volume_max);
    clear_intents(); tap(350,206); assert(action_count(APP_MEDIA_CONTROL) == 0);

    ui_fixture_media(&s_state); s_state.capture_indicator_required = false;
    s_state.media.microphone_level.state = APP_MEDIA_LEVEL_OFF; render();
    audio_level_expect("Mic off", 0);
    clear_intents(); pointer(380,104,true);
    s_state.media.microphone_level.state = APP_MEDIA_LEVEL_WAITING; render(); pointer(380,104,false);
    assert(action_count(APP_MEDIA_CONTROL) == 1 && last_action(APP_MEDIA_CONTROL)->media.operation == APP_MEDIA_LIVE_START);
    assert(last_action(APP_MEDIA_CONTROL)->media.value.role.role == APP_MEDIA_MICROPHONE);

    audio_level_value(-24,60); render();
    size_t objects = audio_level_object_count(lv_screen_active());
    assert(objects == 120); /* 118 baseline + the two-object Manage devices entry. */
    for (unsigned variant = 0; variant < 9; ++variant) audio_level_quiet_updates(variant);
    ui_memory_stats_t steady = ui_memory_stats();
    size_t update_allocations = 0, update_reallocations = 0, maximum_pending_bytes = 0;
    clear_intents();
    for (unsigned i = 0; i < 100; ++i) {
        audio_level_value(-(int)(i % 61), 100 - i % 101);
        s_state.media.microphone_level.clipping = i % 2 != 0;
        ui_memory_stats_t before = ui_memory_stats();
        app_ui_render(&s_state);
        ui_memory_stats_t after = ui_memory_stats();
        /* Pinned LVGL registers a transient label-layout callback even for
         * static text. Bound that bookkeeping; require its release after
         * layout and retain the stricter zero-work checks above for idle. */
        size_t pending_bytes = after.live_bytes - before.live_bytes;
        update_allocations += after.allocations - before.allocations;
        update_reallocations += after.reallocations - before.reallocations;
        if (pending_bytes > maximum_pending_bytes) maximum_pending_bytes = pending_bytes;
        assert(after.allocations - before.allocations <= 2 && pending_bytes <= 64);
        assert(after.reallocations - before.reallocations <= 1 && after.frees == before.frees);
        lv_tick_inc(100); lv_timer_handler(); render();
        after = ui_memory_stats();
        assert(after.live_bytes == steady.live_bytes && after.live_blocks == steady.live_blocks);
        assert(audio_level_object_count(lv_screen_active()) == objects && lv_anim_count_running() == 0);
    }
    assert(action_count(APP_MEDIA_CONTROL) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%saudio-level-resources.json", s_output, s_prefix);
    FILE *report = fopen(path, "wb"); assert(report);
    fprintf(report, "{\"passed\":true,\"core_objects\":%zu,\"quiet_cases\":9,\"snapshots_per_case\":100,"
        "\"quiet_allocations\":0,\"quiet_flushes\":0,\"live_updates\":100,\"layout_callback_allocations\":%zu,"
        "\"layout_array_reallocations\":%zu,\"maximum_pending_layout_bytes\":%zu,\"text_buffer\":\"static\","
        "\"live_bytes_before\":%zu,\"live_bytes_after\":%zu,\"active_animations\":0}\n",
        objects, update_allocations, update_reallocations, maximum_pending_bytes, steady.live_bytes, ui_memory_stats().live_bytes);
    fclose(report);
    s_state = original; render(); clear_intents();
    puts("PASS: real Mic meter state/bounds, held actions, static text, 120 objects, zero identical/hidden work and bounded transient layout allocation");
}
