// SPDX-License-Identifier: Apache-2.0
/* Included by app_ui.c: the same LVGL owner, object tree and intent boundary. */
#define MEDIA_BINDINGS 24U
typedef struct {
    lv_obj_t *object;
    app_media_request_t request;
    bool admitted;
} media_binding_t;
static UI_TASK_BSS media_binding_t s_media_bindings[MEDIA_BINDINGS];
static media_binding_t s_media_pressed;
static size_t s_media_binding_count, s_core_media_bindings;
static lv_obj_t *s_voice_reject;
static lv_obj_t *s_media_connections[2], *s_media_connection_state, *s_media_connection_reason;
static lv_obj_t *s_media_peer, *s_media_link, *s_media_disconnect;
static lv_obj_t *s_record_profile, *s_record_duration, *s_record_duration_buttons[2];
static lv_obj_t *s_record_details, *s_record_state, *s_record_reason, *s_record_action, *s_record_cancel[2];
static lv_obj_t *s_record_rows[APP_MEDIA_ROWS], *s_record_meta[APP_MEDIA_ROWS];
static lv_obj_t *s_record_page, *s_record_pages[2], *s_record_play, *s_record_delete;

static bool media_id_valid(app_media_id_t id)
{
    uint8_t nonzero = 0;
    for (size_t i = 0; i < sizeof(id.bytes); ++i) nonzero |= id.bytes[i];
    return nonzero != 0;
}

static app_media_request_t media_request(app_media_operation_t operation)
{
    app_media_request_t request = {0};
    request.operation = operation;
    request.management_generation = s_view.media.management_generation;
    request.audio_generation = s_view.media.audio_generation;
    request.resource_revision = s_view.media.resource_revision;
    request.peer_id = s_view.media.peer_id;
    return request;
}

static bool media_identity_valid(void)
{
    return s_view.media.known && s_view.media.management_generation && s_view.media.audio_generation;
}

static void media_bind(lv_obj_t *object, app_media_request_t request, bool admitted)
{
    if (!object) return;
    size_t index = 0;
    while (index < s_media_binding_count && s_media_bindings[index].object != object) ++index;
    LV_ASSERT(index < MEDIA_BINDINGS);
    if (index >= MEDIA_BINDINGS) { enabled(object, false); return; }
    if (index == s_media_binding_count) ++s_media_binding_count;
    s_media_bindings[index] = (media_binding_t){object, request, admitted && media_identity_valid()};
    enabled(object, s_media_bindings[index].admitted);
}

static void media_press(lv_obj_t *target)
{
    memset(&s_media_pressed, 0, sizeof(s_media_pressed));
    for (size_t i = 0; i < s_media_binding_count; ++i)
        if (s_media_bindings[i].object == target) { s_media_pressed = s_media_bindings[i]; return; }
}

static bool media_request_equal(const app_media_request_t *a, const app_media_request_t *b)
{
    if (a->operation != b->operation || a->management_generation != b->management_generation ||
        a->audio_generation != b->audio_generation || a->resource_revision != b->resource_revision ||
        a->list_generation != b->list_generation || a->object_revision != b->object_revision ||
        a->consumer_generation != b->consumer_generation || a->speech_generation != b->speech_generation ||
        a->speech_revision != b->speech_revision || a->candidate_id != b->candidate_id ||
        memcmp(a->object_id.bytes, b->object_id.bytes, sizeof(a->object_id.bytes)) ||
        memcmp(a->job_id.bytes, b->job_id.bytes, sizeof(a->job_id.bytes)) ||
        memcmp(a->peer_id.bytes, b->peer_id.bytes, sizeof(a->peer_id.bytes))) return false;
    switch (a->operation) {
    case APP_MEDIA_SELECT_ROUTE: return a->value.route == b->value.route;
    case APP_MEDIA_SELECT_CONNECTION: return a->value.connection == b->value.connection;
    case APP_MEDIA_SET_ENABLED: case APP_MEDIA_SET_MUTED: case APP_MEDIA_LIVE_START: case APP_MEDIA_LIVE_STOP:
        return a->value.role.role == b->value.role.role && a->value.role.value == b->value.role.value;
    case APP_MEDIA_SELECT_PEER: return !memcmp(a->value.peer_id.bytes, b->value.peer_id.bytes, sizeof(a->value.peer_id.bytes));
    case APP_MEDIA_SET_GAIN: return a->value.level == b->value.level;
    case APP_MEDIA_RECORD_SET_PROFILE: return a->value.profile_id == b->value.profile_id;
    case APP_MEDIA_RECORD_SET_DURATION: return a->value.duration_seconds == b->value.duration_seconds;
    case APP_MEDIA_RECORD_LIST_PAGE: return a->value.page_delta == b->value.page_delta;
    case APP_MEDIA_SPEECH_CONFIRM: return a->value.candidate == b->value.candidate;
    default: return true;
    }
}

static void media_action(lv_obj_t *target, int level)
{
    if (!target || target != s_media_pressed.object || !s_media_pressed.admitted ||
        s_view.locked || s_view.screen_off || s_wake_touch || s_touch_local_blocked ||
        s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
        s_built_overlay != s_view.overlay ||
        (s_view.overlay != APP_OVERLAY_NONE && !in_overlay(target))) return;
    const media_binding_t *current = NULL;
    for (size_t i = 0; i < s_media_binding_count; ++i)
        if (s_media_bindings[i].object == target) current = &s_media_bindings[i];
    if (!current || !current->admitted || !media_request_equal(&current->request, &s_media_pressed.request)) return;
    app_intent_t intent = {.action = APP_MEDIA_CONTROL, .epoch = s_touch_epoch,
        .ui_revision = s_touch_revision, .gesture = s_gesture, .surface = APP_SURFACE_CONTROL,
        .media = s_media_pressed.request};
    if (intent.media.operation == APP_MEDIA_SET_VOLUME) {
        if (level < 0 || level > 100 || level > s_view.media.safe_volume_max) return;
        intent.media.value.level = (uint8_t)level;
    }
    if (target == s_confirm_accept && s_view.overlay == APP_OVERLAY_CONFIRM) {
        intent.action = APP_CONFIRM;
        intent.x = 1;
    }
    if ((!s_submit || !s_submit(&intent)) && s_status) label_text(s_status, "Input busy. Please try again.");
}

static const char *media_connection_name(const app_media_view_t *media)
{
    if (!media->known || !media->connection_known) return "Connection";
    return media->connection == APP_MEDIA_CONNECTION_USB ? "USB" :
        media->connection == APP_MEDIA_CONNECTION_BLUETOOTH ? "Bluetooth" : "Connection";
}

static const char *media_job_name(app_media_job_state_t state)
{
    switch (state) {
    case APP_MEDIA_JOB_STARTING: return "Starting";
    case APP_MEDIA_JOB_RUNNING: return "Recording";
    case APP_MEDIA_JOB_STOPPING: return "Stopping";
    case APP_MEDIA_JOB_FINALIZING: return "Saving";
    case APP_MEDIA_JOB_COMPLETED: return "Complete";
    case APP_MEDIA_JOB_PARTIAL: return "Partial";
    case APP_MEDIA_JOB_ERROR: return "Error";
    case APP_MEDIA_JOB_DELETING: return "Deleting";
    default: return "Status unknown";
    }
}

static bool media_job_active(const app_media_job_view_t *job)
{
    return job->known && (job->state == APP_MEDIA_JOB_STARTING || job->state == APP_MEDIA_JOB_RUNNING ||
        job->state == APP_MEDIA_JOB_STOPPING || job->state == APP_MEDIA_JOB_FINALIZING);
}

static bool media_mic_in_use(void)
{
    return media_job_active(&s_view.media.recording) ||
        s_view.media.speech.state == APP_SPEECH_STARTING ||
        s_view.media.speech.state == APP_SPEECH_LISTENING ||
        s_view.media.speech.state == APP_SPEECH_STOPPING;
}

static bool media_waiting_for_host(app_media_start_block_t block)
{
    return block == APP_MEDIA_START_HOST_STREAM_REQUIRED || block == APP_MEDIA_START_HOST_PREPARING;
}

static const char *media_start_reason(bool speaker, const app_media_role_view_t *role,
    const app_media_job_view_t *job)
{
    if (!s_view.media.known || !role->known || role->can_start || media_job_active(job) ||
        (job->known && job->state == APP_MEDIA_JOB_ERROR && role->start_block != APP_MEDIA_START_HOST_PREPARING) ||
        (!speaker && media_mic_in_use())) return NULL;
    /* The owner supplies the admission result; presentation never reconstructs
     * connection, hardware, token or stream readiness from other fields. */
    switch (role->start_block) {
    case APP_MEDIA_START_LOCKED: return "Unlock device to start";
    case APP_MEDIA_START_SCREEN_UNAVAILABLE: return "Wake screen to start";
    case APP_MEDIA_START_CLOCK_UNAVAILABLE: return "Timing unavailable";
    case APP_MEDIA_START_PENDING: return "Audio change in progress";
    case APP_MEDIA_START_QUARANTINED: return "Audio recovery required";
    case APP_MEDIA_START_CONNECTION_REQUIRED: return "Choose USB or Bluetooth";
    case APP_MEDIA_START_UNSUPPORTED: return speaker ? "Speaker unsupported on this connection" : "Mic unsupported on this connection";
    case APP_MEDIA_START_ROUTE_UNAVAILABLE: return "Audio connection unavailable";
    case APP_MEDIA_START_HOST_UNAVAILABLE: return "Host audio is not connected";
    case APP_MEDIA_START_HOST_STREAM_REQUIRED:
    case APP_MEDIA_START_HOST_PREPARING: return "Waiting for host audio";
    case APP_MEDIA_START_IDENTITY_UNAVAILABLE: return "Audio session unavailable";
    case APP_MEDIA_START_PEER_MISMATCH: return "Audio devices do not match";
    case APP_MEDIA_START_CLEANUP: return "Finishing previous audio";
    case APP_MEDIA_START_OCCUPIED: return "Audio resource in use";
    case APP_MEDIA_START_STOP_REQUIRED: return "Stop current audio first";
    case APP_MEDIA_START_CODEC_UNAVAILABLE: return "Audio hardware unavailable";
    case APP_MEDIA_START_FORMAT_UNSUPPORTED: return "Audio format unsupported";
    default: return "Start status unavailable";
    }
}

static const char *media_core_state(bool speaker, const app_media_role_view_t *role, const app_media_job_view_t *job)
{
    if (!s_view.media.known || !role->known) return "Status unknown";
    if (job->known) {
        if (job->state == APP_MEDIA_JOB_ERROR) return "Error";
        if (job->state == APP_MEDIA_JOB_STARTING) return "Starting";
        if (job->state == APP_MEDIA_JOB_STOPPING) return "Stopping";
        if (job->state == APP_MEDIA_JOB_FINALIZING) return "Finishing";
        if (job->state == APP_MEDIA_JOB_RUNNING)
            return !role->mute_confirmed ? "On / Mute unconfirmed" : role->muted ? "On / Muted" : "On";
    }
    if (!speaker && media_job_active(&s_view.media.recording)) return "In use / Recording";
    if (!speaker && (s_view.media.speech.state == APP_SPEECH_STARTING || s_view.media.speech.state == APP_SPEECH_LISTENING ||
        s_view.media.speech.state == APP_SPEECH_STOPPING)) return "In use / Spoken letters";
    if (s_view.media.state == APP_MEDIA_FAULT) return "Audio error";
    if (s_view.media.command_pending) return "Updating";
    if (role->can_start) return "Off / Ready";
    if (media_waiting_for_host(role->start_block)) return "Off / Waiting";
    return "Off / Unavailable";
}

static void render_mic_level(void)
{
    const app_media_mic_level_t *level = &s_view.media.microphone_level;
    app_media_level_state_t state = s_view.media.known ? level->state : APP_MEDIA_LEVEL_UNKNOWN;
    if (state == APP_MEDIA_LEVEL_LIVE && (level->percent > 100 ||
        (!level->silent && (level->dbfs < -96 || level->dbfs > 0)) ||
        (level->silent && level->clipping))) state = APP_MEDIA_LEVEL_UNKNOWN;
    char value[sizeof(s_mic_level_text)];
    unsigned percent = 0;
    uint32_t color = MUTED;
    switch (state) {
    case APP_MEDIA_LEVEL_OFF: snprintf(value, sizeof(value), "Mic off"); break;
    case APP_MEDIA_LEVEL_WAITING: snprintf(value, sizeof(value), "Waiting for signal"); break;
    case APP_MEDIA_LEVEL_LIVE:
        if (level->silent) snprintf(value, sizeof(value), "Input: Quiet");
        else {
            snprintf(value, sizeof(value), level->clipping ? "Too loud / %d dBFS" : "Input %d dBFS", level->dbfs);
            percent = level->percent;
            color = level->clipping ? ERROR : ACCENT;
        }
        break;
    default: snprintf(value, sizeof(value), "Level unavailable"); break;
    }
    /* This read-only meter never participates in action bindings. Its fixed
     * text buffer avoids allocator traffic as live values change length. */
    if (strcmp(value, s_mic_level_text)) {
        memcpy(s_mic_level_text, value, strlen(value) + 1);
        lv_label_set_text_static(s_mic_level_label, s_mic_level_text);
    }
    if (lv_bar_get_value(s_mic_level) != (int)percent)
        lv_bar_set_value(s_mic_level, (int)percent, LV_ANIM_OFF);
    foreground(s_mic_level_label, color, 0);
    background(s_mic_level, color, LV_PART_INDICATOR);
}

static void render_media_core(void)
{
    const app_media_view_t *media = &s_view.media;
    const char *start_reason[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) {
        const app_media_role_view_t *role = i ? &media->speaker : &media->microphone;
        const app_media_job_view_t *job = i ? &media->playback : &media->live_capture;
        bool active = media_job_active(job);
        app_media_request_t power = media_request(active ? APP_MEDIA_LIVE_STOP : APP_MEDIA_LIVE_START);
        power.value.role.role = i ? APP_MEDIA_SPEAKER : APP_MEDIA_MICROPHONE;
        power.job_id = job->id; power.consumer_generation = job->consumer_generation;
        media_bind(s_audio_power[i], power, active ? job->can_stop && media_id_valid(job->id) && job->consumer_generation :
            role->known && role->can_start && !media->command_pending);
        text(s_audio_power[i], i ? (active ? "Stop speaker" : "Start speaker") : (active ? "Stop mic" : "Start mic"));
        selected(s_audio_power[i], active);
        start_reason[i] = media_start_reason(i != 0, role, job);
        bool waiting = start_reason[i] && media_waiting_for_host(role->start_block);
        const char *reason = job->known && *job->reason && !(waiting && job->state == APP_MEDIA_JOB_COMPLETED) ?
            job->reason : start_reason[i];
        if (reason) label_format(s_audio_state[i], "%s\n%s", media_core_state(i != 0, role, job), reason);
        else label_text(s_audio_state[i], media_core_state(i != 0, role, job));
        /* Both host-waiting phases share stable setup guidance. The owner
         * retains the distinct blockers and remains the only start gate. */
        if (waiting)
            start_reason[i] = i ? "Play audio to this device on host" : "Open Mic on host";
    }
    const app_media_role_view_t *mic = &media->microphone;
    bool mute = !mic->mute_confirmed || !mic->muted;
    app_media_request_t request = media_request(APP_MEDIA_SET_MUTED);
    request.value.role.role = APP_MEDIA_MICROPHONE;
    request.value.role.value = mute;
    media_bind(s_audio_mute, request, mic->known && (mute ? mic->can_mute : mic->can_unmute));
    text(s_audio_mute, mic->known && !mic->mute_confirmed ? "Mute mic / unconfirmed" : mute ? "Mute mic" : "Unmute mic");
    selected(s_audio_mute, mic->known && mic->mute_confirmed && mic->muted);
    render_mic_level();
    bool volume = media->volume_known && media->volume_percent <= 100 && media->safe_volume_max <= 100;
    media_bind(s_volume, media_request(APP_MEDIA_SET_VOLUME), volume && media->can_set_volume && !media->command_pending);
    if (volume) { slider_value(s_volume, media->volume_percent); label_format(s_volume_text, "%u%%", media->volume_percent); }
    else label_text(s_volume_text, "--");
    bool waiting_hint = (start_reason[0] && media_waiting_for_host(media->microphone.start_block)) ||
        (start_reason[1] && media_waiting_for_host(media->speaker.start_block));
    if (!media->known) label_text(s_audio_note, "Audio status unavailable.");
    else if (*media->reason) label_text(s_audio_note, media->reason);
    else if (media->live_capture.known && *media->live_capture.reason &&
        !(waiting_hint && media->live_capture.state == APP_MEDIA_JOB_COMPLETED)) label_format(s_audio_note, "Mic: %s", media->live_capture.reason);
    else if (media->playback.known && *media->playback.reason &&
        !(waiting_hint && media->playback.state == APP_MEDIA_JOB_COMPLETED)) label_format(s_audio_note, "Speaker: %s", media->playback.reason);
    else if (start_reason[0] && start_reason[1]) label_format(s_audio_note, "Mic: %s\nSpeaker: %s", start_reason[0], start_reason[1]);
    else if (start_reason[0]) label_format(s_audio_note, "Mic: %s", start_reason[0]);
    else if (start_reason[1]) label_format(s_audio_note, "Speaker: %s", start_reason[1]);
    else label_text(s_audio_note, "Choose a connection, then start Mic or Speaker.");
}

static void render_speech(void)
{
    const app_media_speech_view_t *speech = &s_view.media.speech;
    bool candidate = speech->state == APP_SPEECH_CANDIDATE && speech->generation && speech->candidate_id &&
        ((speech->candidate >= 'A' && speech->candidate <= 'Z') || (speech->candidate >= 'a' && speech->candidate <= 'z') ||
         (speech->candidate >= '0' && speech->candidate <= '9'));
    char letter[2] = {candidate ? speech->candidate : 0, 0};
    label_text(s_voice_candidate, candidate ? letter : speech->state == APP_SPEECH_LISTENING ? "..." : "A-Z");
    const char *state = speech->state == APP_SPEECH_STARTING ? "Starting" :
        speech->state == APP_SPEECH_LISTENING ? "Listening" : speech->state == APP_SPEECH_STOPPING ? "Stopping" :
        speech->state == APP_SPEECH_ERROR ? "Recognition error" : speech->state == APP_SPEECH_IDLE ? "Ready to listen" :
        candidate ? "Confirm adds to preview only" : "Spoken letters unavailable";
    if (speech->remaining_known)
        label_format(s_voice_hint, "%s / %us left\n%.32s\n%.96s", state, speech->remaining_seconds, speech->profile, speech->reason);
    else label_format(s_voice_hint, "%s\n%.32s\n%.96s", state, speech->profile, speech->reason);
    lv_obj_set_height(s_voice_hint, 88);
    app_media_operation_t operation = candidate ? APP_MEDIA_SPEECH_CONFIRM : speech->can_stop ? APP_MEDIA_SPEECH_STOP : APP_MEDIA_SPEECH_START;
    app_media_request_t request = media_request(operation);
    request.speech_generation = speech->generation;
    request.speech_revision = speech->revision;
    request.candidate_id = speech->candidate_id;
    request.value.candidate = candidate ? speech->candidate : 0;
    text(s_voice_confirm, candidate ? "Confirm letter" : speech->can_stop ? "Stop listening" : "Start listening");
    media_bind(s_voice_confirm, request, !s_view.sending && speech->generation &&
        (candidate ? speech->can_confirm : speech->can_stop || speech->can_listen));
    request.operation = APP_MEDIA_SPEECH_REJECT;
    media_bind(s_voice_reject, request, candidate && speech->can_reject);
    visible(s_voice_reject, candidate);
}

static void clear_media_panel(void)
{
    s_media_binding_count = s_core_media_bindings;
    memset(s_media_bindings + s_core_media_bindings, 0,
        (MEDIA_BINDINGS - s_core_media_bindings) * sizeof(s_media_bindings[0]));
    memset(s_media_connections, 0, sizeof(s_media_connections));
    memset(s_record_duration_buttons, 0, sizeof(s_record_duration_buttons));
    memset(s_record_rows, 0, sizeof(s_record_rows));
    memset(s_record_meta, 0, sizeof(s_record_meta));
    memset(s_record_cancel, 0, sizeof(s_record_cancel));
    memset(s_record_pages, 0, sizeof(s_record_pages));
    s_media_peer = s_media_link = s_media_disconnect = NULL;
    s_media_connection_state = s_media_connection_reason = NULL;
    s_record_profile = s_record_duration = s_record_details = s_record_state = s_record_reason = NULL;
    s_record_action = s_record_page = s_record_play = s_record_delete = NULL;
}

static lv_obj_t *media_button(lv_obj_t *parent, const char *caption, int x, int y, int width)
{
    return button(parent, caption, x, y, width, 48, APP_MEDIA_CONTROL, 0);
}

static lv_obj_t *media_label(lv_obj_t *parent, int y, int height, uint32_t color)
{
    lv_obj_t *object = label(parent, "", 0, y, 456, color);
    lv_obj_set_height(object, height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static void create_media_panel(lv_obj_t *body, lv_obj_t *title, app_overlay_t kind)
{
    if (kind == APP_OVERLAY_MEDIA_OPTIONS) {
        label_text(title, "Audio connection");
        s_media_connections[0] = button(body, "USB", 0, 0, 224, 56, APP_MEDIA_CONTROL, 0);
        s_media_connections[1] = button(body, "Bluetooth", 232, 0, 224, 56, APP_MEDIA_CONTROL, 0);
        s_media_connection_state = media_label(body, 72, 32, INK);
        s_media_connection_reason = media_label(body, 112, 56, MUTED);
        s_media_peer = media_button(body, "Choose audio device", 0, 184, 456);
        s_media_link = media_button(body, "Connect", 0, 244, 224);
        s_media_disconnect = media_button(body, "Disconnect", 232, 244, 224);
        label_text(media_label(body, 312, 40, MUTED), "Connection selection does not start audio.");
        button(body, "Back to Audio", 0, 360, 456, 48, APP_CLOSE, 0);
    } else if (kind == APP_OVERLAY_RECORDING_START) {
        label_text(title, "Record");
        s_record_profile = media_button(body, "Profile unavailable", 0, 0, 456);
        s_record_duration_buttons[0] = media_button(body, "-1 sec", 0, 56, 104);
        s_record_duration_buttons[1] = media_button(body, "+1 sec", 352, 56, 104);
        s_record_duration = label(body, "Duration unknown", 112, 70, 232, INK);
        s_record_details = media_label(body, 112, 80, MUTED);
        s_record_state = media_label(body, 200, 32, INK);
        s_record_reason = media_label(body, 240, 56, MUTED);
        s_record_action = media_button(body, "Review recording", 0, 304, 456);
        s_record_cancel[0] = media_button(body, "Cancel / keep", 0, 360, 224);
        s_record_cancel[1] = media_button(body, "Cancel / discard", 232, 360, 224);
    } else if (kind == APP_OVERLAY_RECORDINGS) {
        label_text(title, "Recordings");
        s_record_state = media_label(body, 0, 28, MUTED);
        for (int i = 0; i < (int)APP_MEDIA_ROWS; ++i) {
            s_record_rows[i] = button(body, "", 0, 36 + i * 64, 456, 56, APP_MEDIA_CONTROL, 0);
            lv_obj_t *caption = lv_obj_get_child(s_record_rows[i], 0);
            lv_obj_set_y(caption, -9);
            s_record_meta[i] = metadata(s_record_rows[i], "", 8, 34, 440);
            lv_obj_set_style_text_align(s_record_meta[i], LV_TEXT_ALIGN_CENTER, 0);
        }
        s_record_page = media_label(body, 300, 48, MUTED);
        s_record_pages[0] = media_button(body, "Previous", 0, 360, 224);
        s_record_pages[1] = media_button(body, "Next", 232, 360, 224);
    } else {
        label_text(title, "Recording details");
        s_record_state = media_label(body, 0, 48, INK);
        s_record_details = media_label(body, 64, 112, MUTED);
        s_record_reason = media_label(body, 192, 96, MUTED);
        s_record_play = media_button(body, "Play", 0, 304, 224);
        s_record_delete = media_button(body, "Delete", 232, 304, 224);
        button(body, "Back to recordings", 0, 360, 456, 48, APP_MENU, APP_OVERLAY_RECORDINGS);
    }
}

static const app_media_profile_view_t *record_profile(const app_media_record_options_t *options)
{
    if (!options->known || options->profile_count > APP_MEDIA_PROFILES) return NULL;
    for (size_t i = 0; i < options->profile_count; ++i)
        if (options->profiles[i].available && options->profiles[i].id == options->profile_id) return &options->profiles[i];
    return NULL;
}

static bool record_options_valid(const app_media_record_options_t *options)
{
    return record_profile(options) && options->minimum_duration_seconds && options->duration_seconds >= options->minimum_duration_seconds &&
        options->duration_seconds <= options->maximum_duration_seconds && options->byte_limit;
}

static app_media_request_t record_object_request(app_media_operation_t operation, const app_media_record_view_t *record)
{
    app_media_request_t request = media_request(operation);
    request.list_generation = s_view.media.recordings.generation;
    request.object_id = record->id;
    request.object_revision = record->revision;
    return request;
}

static app_media_request_t record_job_request(app_media_operation_t operation, const app_media_job_view_t *job)
{
    app_media_request_t request = media_request(operation);
    request.job_id = job->id;
    request.object_id = job->object_id;
    request.consumer_generation = job->consumer_generation;
    return request;
}

static void render_media_panel(void)
{
    const app_media_view_t *media = &s_view.media;
    if (s_view.overlay == APP_OVERLAY_MEDIA_OPTIONS) {
        bool connection = media->known && media->connection_known && (unsigned)media->connection < APP_MEDIA_CONNECTION_COUNT;
        for (unsigned i = 0; i < APP_MEDIA_CONNECTION_COUNT; ++i) {
            app_media_request_t request = media_request(APP_MEDIA_SELECT_CONNECTION);
            request.value.connection = (app_media_connection_t)i;
            media_bind(s_media_connections[i], request, media->can_select_connection[i] && !media->command_pending);
            selected(s_media_connections[i], connection && (unsigned)media->connection == i);
        }
        bool bluetooth = connection && media->connection == APP_MEDIA_CONNECTION_BLUETOOTH;
        bool matching_route = connection && (bluetooth ? media->route == APP_MEDIA_ROUTE_HFP || media->route == APP_MEDIA_ROUTE_A2DP :
            media->route == APP_MEDIA_ROUTE_UAC);
        const app_media_route_view_t *route = matching_route ? &media->routes[media->route] : NULL;
        label_text(s_media_connection_state, !connection ? "No confirmed selection" : media->command_pending ? "Updating connection" :
            route && route->connected ? "Selected / Link connected" : "Selected / Not connected");
        label_text(s_media_connection_reason, !media->known ? "Audio status unavailable." : *media->reason ? media->reason :
            route && *route->reason ? route->reason : bluetooth ? "Select your audio device and connect." :
            connection ? "Connect the USB cable to use audio." : "Choose USB or Bluetooth. Audio stays off until started.");
        visible(s_media_peer, bluetooth); visible(s_media_link, bluetooth); visible(s_media_disconnect, bluetooth);
        text(s_media_peer, *media->peer_label ? media->peer_label : "Choose audio device");
        app_media_request_t peer_request = media_request(APP_MEDIA_SELECT_PEER);
        peer_request.value.peer_id = media->next_peer_id;
        media_bind(s_media_peer, peer_request, bluetooth && media->can_select_peer && !media->command_pending);
        media_bind(s_media_link, media_request(APP_MEDIA_LINK_CONNECT), bluetooth && route && route->can_connect && !media->command_pending);
        media_bind(s_media_disconnect, media_request(APP_MEDIA_LINK_DISCONNECT), bluetooth && route && route->can_disconnect && !media->command_pending);
    } else if (s_view.overlay == APP_OVERLAY_RECORDING_START) {
        const app_media_record_options_t *options = &media->recording_options;
        const app_media_job_view_t *job = &media->recording;
        const app_media_profile_view_t *profile = record_profile(options);
        char caption[64]; snprintf(caption, sizeof(caption), "Profile / %.32s", profile ? profile->label : "unavailable");
        text(s_record_profile, caption);
        app_media_request_t request = media_request(APP_MEDIA_RECORD_SET_PROFILE);
        request.value.profile_id = options->profile_id;
        if (profile) {
            size_t index = (size_t)(profile - options->profiles);
            for (size_t step = 1; step < options->profile_count; ++step) {
                const app_media_profile_view_t *next = &options->profiles[(index + step) % options->profile_count];
                if (next->available && next->id != options->profile_id) { request.value.profile_id = next->id; break; }
            }
        }
        media_bind(s_record_profile, request, profile && options->can_edit && !media->command_pending && request.value.profile_id != options->profile_id);
        bool valid = record_options_valid(options);
        if (valid) label_format(s_record_duration, "Limit / %us", options->duration_seconds);
        else label_text(s_record_duration, "Duration unknown");
        for (unsigned i = 0; i < 2; ++i) {
            request = media_request(APP_MEDIA_RECORD_SET_DURATION);
            request.value.duration_seconds = i ? options->duration_seconds + 1 : options->duration_seconds - 1;
            media_bind(s_record_duration_buttons[i], request, valid && options->can_edit && !media->command_pending &&
                (i ? options->duration_seconds < options->maximum_duration_seconds : options->duration_seconds > options->minimum_duration_seconds));
        }
        if (valid) label_format(s_record_details, "Local recording / max %llu bytes\nRetention: %us / download: %s\nContinue locally: %s", (unsigned long long)options->byte_limit,
            options->retention_seconds, options->owner_download ? "owner only" : "disabled", options->continue_locally ? "yes" : "no");
        else label_text(s_record_details, "Recording limits unavailable");
        if (job->known && job->stored_bytes_known) label_format(s_record_state, "%s / %llus / %llu bytes", media_job_name(job->state),
            (unsigned long long)(job->elapsed_ms / 1000), (unsigned long long)job->stored_bytes);
        else if (job->known) label_format(s_record_state, "%s / %llus / size pending", media_job_name(job->state),
            (unsigned long long)(job->elapsed_ms / 1000));
        else label_text(s_record_state, "No current recording status");
        label_text(s_record_reason, job->cleanup_pending ? "Cleanup pending; recording is not deleted yet." :
            job->known && *job->reason ? job->reason : options->reason);
        bool stop = job->known && job->can_stop && media_id_valid(job->id);
        text(s_record_action, stop ? "Stop and save" : "Review recording");
        request = stop ? record_job_request(APP_MEDIA_RECORD_STOP, job) : media_request(APP_MEDIA_RECORD_START_REQUEST);
        media_bind(s_record_action, request, stop || (valid && options->can_start && !media->command_pending));
        media_bind(s_record_cancel[0], record_job_request(APP_MEDIA_RECORD_CANCEL_KEEP, job), job->known && media_id_valid(job->id) && job->can_cancel_keep);
        media_bind(s_record_cancel[1], record_job_request(APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST, job), job->known && media_id_valid(job->id) && job->can_cancel_discard);
    } else if (s_view.overlay == APP_OVERLAY_RECORDINGS) {
        const app_media_record_list_view_t *list = &media->recordings;
        bool valid = list->known && list->generation && list->row_count <= APP_MEDIA_ROWS;
        label_text(s_record_state, list->pending ? "Loading recordings..." : !valid ? "Recording list unavailable" : !list->row_count ? "No recordings" : "Select a recording");
        for (size_t i = 0; i < APP_MEDIA_ROWS; ++i) {
            const app_media_record_view_t *row = &list->rows[i];
            bool present = valid && i < list->row_count && row->valid && media_id_valid(row->id);
            visible(s_record_rows[i], present);
            text(s_record_rows[i], present ? row->label : "");
            label_format(s_record_meta[i], "%s / %s", media_job_name(row->state), row->verified ? "verified" : "unverified");
            media_bind(s_record_rows[i], record_object_request(APP_MEDIA_RECORD_SELECT, row), present && !list->pending);
        }
        if (valid) label_format(s_record_page, "Page %u\n%.96s", list->page + 1, list->reason);
        else label_text(s_record_page, list->reason);
        for (unsigned i = 0; i < 2; ++i) {
            app_media_request_t request = media_request(APP_MEDIA_RECORD_LIST_PAGE);
            request.list_generation = list->generation;
            request.value.page_delta = i ? 1 : -1;
            media_bind(s_record_pages[i], request, valid && !list->pending && (i ? list->has_next : list->has_previous));
        }
    } else if (s_view.overlay == APP_OVERLAY_RECORDING_DETAIL) {
        const app_media_record_view_t *record = &media->selected_recording;
        bool valid = record->valid && media_id_valid(record->id);
        label_text(s_record_state, valid ? record->label : "Recording unavailable");
        char duration[48] = "Duration unavailable", bytes[48] = "Size unavailable";
        if (valid && record->duration_known) snprintf(duration, sizeof(duration), "Duration: %llums", (unsigned long long)record->duration_ms);
        if (valid && record->bytes_known) snprintf(bytes, sizeof(bytes), "Stored: %llu bytes", (unsigned long long)record->stored_bytes);
        label_format(s_record_details, "%s / %s\n%s\n%s", valid ? media_job_name(record->state) : "Status unknown", record->verified ? "verified" : "unverified", duration, bytes);
        label_text(s_record_reason, record->reason);
        bool stop = media->playback.known && media->playback.can_stop && media_id_valid(media->playback.id);
        text(s_record_play, stop ? "Stop playback" : "Play");
        media_bind(s_record_play, stop ? record_job_request(APP_MEDIA_PLAYBACK_STOP, &media->playback) :
            record_object_request(APP_MEDIA_RECORD_PLAY, record), stop || (valid && record->can_play && !media->command_pending));
        media_bind(s_record_delete, record_object_request(APP_MEDIA_RECORD_DELETE_REQUEST, record), valid && record->can_delete && !media->command_pending);
    }
}

static void render_media_confirmation(void)
{
    const app_media_review_t *review = &s_view.media_review;
    bool valid = review->valid &&
        (s_view.confirmation == APP_CONFIRM_RECORDING_START ?
            review->request.operation == APP_MEDIA_RECORD_START_REQUEST && record_options_valid(&review->options) :
         s_view.confirmation == APP_CONFIRM_RECORDING_DELETE ?
            review->request.operation == APP_MEDIA_RECORD_DELETE_REQUEST && review->recording.valid &&
            media_id_valid(review->request.object_id) && !memcmp(review->request.object_id.bytes, review->recording.id.bytes, 16) &&
            review->request.object_revision == review->recording.revision :
            review->request.operation == APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST && review->job.known &&
            media_id_valid(review->request.job_id) && !memcmp(review->request.job_id.bytes, review->job.id.bytes, 16) &&
            review->request.consumer_generation == review->job.consumer_generation);
    if (!valid) {
        label_text(s_confirm_title, "Review unavailable");
        label_text(s_confirm_detail, "This review is unavailable or expired. Cancel and open a fresh review.");
        text(s_confirm_accept, s_view.confirmation == APP_CONFIRM_RECORDING_START ? "Start recording" :
            s_view.confirmation == APP_CONFIRM_RECORDING_DELETE ? "Delete recording" : "Discard recording");
        media_bind(s_confirm_accept, review->request, false);
        return;
    }
    if (s_view.confirmation == APP_CONFIRM_RECORDING_START) {
        valid = valid && review->request.operation == APP_MEDIA_RECORD_START_REQUEST && record_options_valid(&review->options);
        const app_media_profile_view_t *profile = record_profile(&review->options);
        label_text(s_confirm_title, "Start microphone recording?");
        label_format(s_confirm_detail, "Local reserved storage / %.32s\nLimit: %us / %llu bytes\nRetention: %us\nDownload: %s / continue locally: %s\nMicrophone capture starts only after acceptance.",
            profile ? profile->label : "unavailable", review->options.duration_seconds, (unsigned long long)review->options.byte_limit,
            review->options.retention_seconds, review->options.owner_download ? "owner only" : "disabled", review->options.continue_locally ? "yes" : "no");
        text(s_confirm_accept, "Start recording");
    } else if (s_view.confirmation == APP_CONFIRM_RECORDING_DELETE) {
        valid = valid && review->request.operation == APP_MEDIA_RECORD_DELETE_REQUEST && review->recording.valid &&
            media_id_valid(review->request.object_id) && !memcmp(review->request.object_id.bytes, review->recording.id.bytes, 16) &&
            review->request.object_revision == review->recording.revision;
        label_text(s_confirm_title, "Delete this recording?");
        label_format(s_confirm_detail, "%.32s\nThe selected recording will be removed.\nDeletion remains pending until storage confirms cleanup.", review->recording.label);
        text(s_confirm_accept, "Delete recording");
    } else {
        valid = valid && review->request.operation == APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST && review->job.known &&
            media_id_valid(review->request.job_id) && !memcmp(review->request.job_id.bytes, review->job.id.bytes, 16) &&
            review->request.consumer_generation == review->job.consumer_generation;
        label_text(s_confirm_title, "Cancel and discard recording?");
        label_text(s_confirm_detail, "Stop this recording and discard its artifact.\nThe microphone warning stays until stop is confirmed.\nCleanup may remain pending after capture stops.");
        text(s_confirm_accept, "Discard recording");
    }
    if (!valid) label_text(s_confirm_detail, "This review is unavailable or expired. Cancel and open a fresh review.");
    media_bind(s_confirm_accept, review->request, valid);
}

static void sanitize_record_options(app_media_record_options_t *options)
{
    options->reason[sizeof(options->reason) - 1] = 0;
    for (size_t i = 0; i < APP_MEDIA_PROFILES; ++i) options->profiles[i].label[32] = 0;
}

static void sanitize_record(app_media_record_view_t *record)
{
    record->label[32] = 0;
    record->reason[64] = 0;
}

static void sanitize_media(void)
{
    app_media_view_t *media = &s_view.media;
    media->peer_label[48] = media->reason[96] = 0;
    for (size_t i = 0; i < APP_MEDIA_ROUTE_COUNT; ++i) media->routes[i].reason[64] = 0;
    sanitize_record_options(&media->recording_options);
    media->recording.reason[96] = media->playback.reason[96] = media->live_capture.reason[96] = media->recordings.reason[96] = 0;
    for (size_t i = 0; i < APP_MEDIA_ROWS; ++i) sanitize_record(&media->recordings.rows[i]);
    sanitize_record(&media->selected_recording);
    media->speech.profile[32] = media->speech.reason[96] = 0;
    sanitize_record_options(&s_view.media_review.options);
    sanitize_record(&s_view.media_review.recording);
    s_view.media_review.job.reason[96] = 0;
}
