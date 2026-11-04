// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include "ui_memory.h"
#include "ui_private_probe.h"
#include "ui_extension_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 480
#define HEIGHT 480
#define IDLE_MS 10000
#define FRAME_BYTES (WIDTH * HEIGHT * 2U)
/* design/ui-plan.md: fixed input pages preserve gestures; one panel is dynamic. */
#define CORE_OBJECT_LIMIT 120 /* Navigation plus the direct Manage devices entry. */
#define PANEL_OBJECT_LIMIT 162 /* Two explicit Manage devices core objects extend every panel. */

typedef struct {
    input_mode_t mode;
    app_overlay_t overlay;
    app_settings_page_t page;
    app_confirmation_t confirmation;
} panel_case_t;

static const panel_case_t s_panels[] = {
    {INPUT_MOUSE, APP_OVERLAY_MODES, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_TARGET, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_HOME, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_CONNECTIVITY, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_DISPLAY, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_INPUT, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_ABOUT, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_BLUETOOTH, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_WIFI, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_MOTION, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_MOTION_ADVANCED, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_GESTURES, 0},
    {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_NAVIGATION, 0},
    {INPUT_KEYBOARD, APP_OVERLAY_KEYBOARD, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_AUDIO, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_BLUETOOTH_DEVICES, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_BLUETOOTH_DEVICES, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_AUDIO_DEVICE, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_BLUETOOTH_INPUT_OPTIONS, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_WIFI_SETUP, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_OPEN},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_CLOSE},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_FORGET_ALL},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_WIFI_FORGET},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_DISCONNECT},
    {INPUT_MOUSE, APP_OVERLAY_WIFI_NETWORKS, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_WIFI_NETWORK, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_LOCAL_EDITOR, 0, 0},
    {INPUT_KEYBOARD, APP_OVERLAY_LOCAL_EDITOR, 0, 0},
    {INPUT_KEYBOARD, APP_OVERLAY_COMMON_KEYS, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_RECORDING_STORAGE, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_RECORDING_STORAGE_PREPARE},
    {INPUT_AUDIO, APP_OVERLAY_MEDIA_OPTIONS, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_RECORDING_START, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_RECORDINGS, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_RECORDING_DETAIL, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_RECORDING_START},
    {INPUT_AUDIO, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_RECORDING_DELETE},
    {INPUT_AUDIO, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_RECORDING_CANCEL_DISCARD},
    {INPUT_MOUSE, APP_OVERLAY_API_CONNECTION, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_API_REVIEW, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_CONTROLLERS, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_CONTROLLER_ACCESS, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_WIFI_PROFILES, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_BLUETOOTH_TARGET, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_BLUETOOTH_PAIRING, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_BLUETOOTH_FORGET_ONE},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_REMOTE},
};

static unsigned char s_draw[FRAME_BYTES];
static size_t s_flushes, s_flush_bytes;
static FILE *s_report;
static bool s_first_result = true;
static uint64_t s_fixture_editor_session = 1000;

static bool reject_unexpected_intent(app_intent_t *intent)
{
    (void)intent;
    assert(!"Rendering alone must never submit product intent");
    return false;
}

static app_local_edit_result_t reject_unexpected_private(const app_local_edit_request_t *request)
{
    (void)request;
    assert(!"Rendering alone must never submit a private draft");
    return APP_LOCAL_EDIT_UNAVAILABLE;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(pixels);
    assert(lv_display_get_render_mode(display) == LV_DISPLAY_RENDER_MODE_FULL);
    assert(area->x1 == 0 && area->y1 == 0 && area->x2 == WIDTH - 1 && area->y2 == HEIGHT - 1);
    ++s_flushes;
    s_flush_bytes += FRAME_BYTES;
    lv_display_flush_ready(display);
}

static void advance(unsigned milliseconds)
{
    assert(milliseconds % 10 == 0);
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static size_t object_count(lv_obj_t *object)
{
    size_t count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += object_count(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static size_t timer_count(void)
{
    size_t count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) ++count;
    return count;
}

static app_view_t view(input_mode_t mode, bool dark)
{
    app_view_t state = {.mode = mode, .dark_theme = dark, .epoch = 11, .ui_revision = 1,
        .navigation_enabled = true, .navigation_available = mode == INPUT_MOUSE,
        .connected = true, .ready = true, .bluetooth_available = true,
        .bluetooth_connected = true, .bluetooth_ready = true, .target_name = "Resource test peer",
        .motion_available = true, .gain_divisor = 1, .microphone_muted = true,
        .keyboard_submode = APP_KEYBOARD_TOUCH, .keyboard_group = -1,
        .keyboard_cell_count = 9, .battery_supported = true, .battery_percent_valid = true,
        .battery_percent = 73, .battery_state = APP_BATTERY_DISCHARGING,
        .paired_host_count = 1, .paired_host_capacity = 15,
        .bluetooth_hosts_available = true, .bluetooth_host_count = 1,
        .wifi_state = APP_UI_WIFI_CONNECTED, .wifi_rssi_valid = true, .wifi_rssi_dbm = -50,
        .settings_save_state = APP_SAVE_SAVED, .brightness_available = true,
        .brightness_percent = 70, .screen_off_available = true, .idle_timeout_seconds = 300,
        .firmware_version = "resource-fixture", .uptime_seconds = 60,
        .developer = "from1to9", .repository_url = "https://github.com/from1to9/ESP32-INPUT",
        .git_commit = "0123456789abcdef0123456789abcdef01234567", .git_state = APP_GIT_CLEAN};
    static const char *const cells[] = {"ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"};
    for (size_t i = 0; i < 9; ++i) strcpy(state.keyboard_cells[i], cells[i]);
    strcpy(state.bluetooth_target_name, "Resource test peer");
    strcpy(state.bluetooth_hosts[0].name, "Hidden saved peer");
    strcpy(state.bluetooth_hosts[0].identity, "AA:BB:CC:DD:EE:01");
    strcpy(state.wifi_ssid, "Synthetic network");
    ui_fixture_native_networks(&state);
    return state;
}

static app_view_t panel_view(const panel_case_t *panel, bool dark)
{
    app_view_t state = view(panel->mode, dark);
    state.overlay = panel->overlay;
    state.bluetooth_audio_devices = panel->mode == INPUT_AUDIO &&
        (panel->overlay == APP_OVERLAY_BLUETOOTH_DEVICES || panel->overlay == APP_OVERLAY_AUDIO_DEVICE);
    state.settings_page = panel->page;
    state.confirmation = panel->confirmation;
    state.paired_host_count = 15;
    state.bluetooth_host_count = 15;
    for (unsigned i = 0; i < 15; ++i) {
        snprintf(state.bluetooth_hosts[i].identity, sizeof state.bluetooth_hosts[i].identity,
                 "AA:BB:CC:DD:EE:%02X", i);
        strcpy(state.bluetooth_hosts[i].name, "Synthetic saved host");
    }
    if (panel->overlay == APP_OVERLAY_WIFI_SETUP) {
        state.wifi_setup_active = true;
        state.wifi_setup_seconds_left = 600;
        strcpy(state.wifi_setup_ssid, "TEST-SETUP-ABCDEFGHIJKLMNOPQRSTU");
        strcpy(state.wifi_setup_passcode, "TESTONLY-123456789012345");
        strcpy(state.wifi_setup_url, "http://192.0.2.1/");
    }
    if (panel->overlay == APP_OVERLAY_LOCAL_EDITOR) {
        state.local_editor = (app_local_editor_view_t){.session = ++s_fixture_editor_session,
            .purpose = panel->mode == INPUT_MOUSE ? APP_LOCAL_EDIT_WIFI_PASSPHRASE : APP_LOCAL_EDIT_BLUETOOTH_NAME,
            .seconds_left = 300, .submit_available = true, .wifi_scan_generation = 47,
            .wifi_record_index = 17, .wifi_remember = true};
        strcpy(state.local_editor.bluetooth_initial, "Synthetic name");
    }
    state.motion_settings = (app_motion_settings_view_t){
        .available = true, .editable = true, .revision = 17,
        .gain_x_centi = 400, .gain_y_centi = 400, .deadzone_mdps = 500,
        .filter_ms = 30, .freeze_ms = 150, .reference = APP_REFERENCE_READY
    };
    state.recording_storage = (app_recording_storage_view_t){
        .state = APP_RECORD_STORE_UNPREPARED, .backend = APP_RECORD_BACKEND_INTERNAL_RESERVED,
        .generation = 31, .revision = 47, .reserved_region_verified = true, .can_prepare = true
    };
    ui_fixture_media(&state);
    ui_fixture_connections(&state);
    ui_fixture_remote_review(&state);
    strcpy(state.recording_storage.reason, "The dedicated internal recording area is not prepared.");
    return state;
}

static bool idle_case(app_view_t state, bool hidden_changes, bool covered_target)
{
    if (covered_target)
        assert(state.overlay == APP_OVERLAY_SETTINGS && state.settings_page == APP_SETTINGS_HOME);
    app_ui_render(&state);
    lv_obj_update_layout(lv_screen_active());
    advance(1000);
    size_t objects = object_count(lv_screen_active());
    size_t timers = timer_count();
    ui_memory_stats_t before = ui_memory_stats();
    size_t flush_before = s_flushes, bytes_before = s_flush_bytes;
    for (unsigned elapsed = 0; elapsed < IDLE_MS; elapsed += 100) {
        if (hidden_changes) {
            state.uptime_seconds += 61;
            strcpy(state.firmware_version, elapsed % 200 ? "hidden-version-a" : "hidden-version-b");
            strcpy(state.developer, elapsed % 200 ? "Hidden developer A" : "Hidden developer B with more");
            strcpy(state.repository_url, elapsed % 200 ? "https://example.com/owner/a" : "https://example.com/longer-owner/longer-repository");
            strcpy(state.git_commit, elapsed % 200 ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
            state.git_state = elapsed % 200 ? APP_GIT_CLEAN : APP_GIT_DIRTY;
            strcpy(state.bluetooth_hosts[0].name, elapsed % 200 ? "Hidden peer A" : "Hidden peer B");
            strcpy(state.wifi_scan.rows[0].label, elapsed % 200 ? "Hidden Wi-Fi A" : "Hidden Wi-Fi B");
            strcpy(state.bluetooth_name.effective, elapsed % 200 ? "Hidden name A" : "Hidden name B");
            strcpy(state.media.recording_options.reason, elapsed % 200 ? "Hidden recording A" : "Hidden recording B");
            state.media.recordings.generation += 1;
            if (state.mode != INPUT_AUDIO) {
                state.media.connection = elapsed % 200 ? APP_MEDIA_CONNECTION_USB : APP_MEDIA_CONNECTION_BLUETOOTH;
                state.media.volume_percent = elapsed % 200 ? 20 : 40;
                strcpy(state.media.reason, elapsed % 200 ? "Hidden audio A" : "Hidden audio B");
            }
        }
        if (covered_target)
            state.target_name = elapsed % 200 ? "Covered target A" : "Covered target B";
        app_ui_render(&state);
        advance(100);
    }
    ui_memory_stats_t after = ui_memory_stats();
    size_t allocations = after.allocations - before.allocations;
    size_t reallocations = after.reallocations - before.reallocations;
    size_t frees = after.frees - before.frees;
    size_t flushes = s_flushes - flush_before;
    bool quiet = allocations == 0 && reallocations == 0 && frees == 0 && flushes == 0;
    bool bounded = objects == object_count(lv_screen_active()) && timers == timer_count() &&
        before.live_bytes == after.live_bytes && before.live_blocks == after.live_blocks;
    size_t limit = state.overlay == APP_OVERLAY_NONE ? CORE_OBJECT_LIMIT : PANEL_OBJECT_LIMIT;
    bool passed = quiet && bounded && objects <= limit && after.failures == 0;
    fprintf(s_report, "%s\n    {\"mode\":%d,\"dark\":%s,\"hidden_changes\":%s,"
        "\"covered_target_changes\":%s,\"overlay\":%d,\"settings_page\":%d,\"confirmation\":%d,\"screen_off\":%s,"
        "\"motion_enabled\":%s,\"motion_reference\":%d,\"motion_recenter_available\":%s,\"object_limit\":%zu,"
        "\"simulated_ms\":%u,\"snapshots\":100,\"objects\":%zu,\"timers\":%zu,"
        "\"allocations\":%zu,\"reallocations\":%zu,\"frees\":%zu,\"flushes\":%zu,"
        "\"flush_bytes\":%zu,\"live_bytes_before\":%zu,\"live_bytes_after\":%zu,"
        "\"peak_live_bytes\":%zu,\"quiet\":%s,\"bounded\":%s,\"passed\":%s}",
        s_first_result ? "" : ",", (int)state.mode, state.dark_theme ? "true" : "false", hidden_changes ? "true" : "false",
        covered_target ? "true" : "false", (int)state.overlay, (int)state.settings_page,
        (int)state.confirmation, state.screen_off ? "true" : "false", state.motion_enabled ? "true" : "false",
        (int)state.motion_settings.reference, state.motion_recenter_available ? "true" : "false", limit,
        IDLE_MS, objects, timers, allocations, reallocations, frees, flushes, s_flush_bytes - bytes_before,
        before.live_bytes, after.live_bytes, after.peak_live_bytes, quiet ? "true" : "false",
        bounded ? "true" : "false", passed ? "true" : "false");
    s_first_result = false;
    return passed;
}

static bool navigation_case(bool dark)
{
    app_view_t core = view(INPUT_MOUSE, dark);
    uint64_t revision = 100;
    const size_t count = sizeof s_panels / sizeof s_panels[0];
    for (unsigned warmup = 0; warmup < 2; ++warmup) {
        for (size_t i = 0; i < count; ++i) {
            app_view_t state = panel_view(&s_panels[i], dark);
            state.ui_revision = ++revision;
            app_ui_render(&state);
            advance(100);
            core.ui_revision = ++revision;
            app_ui_render(&core);
            advance(100);
        }
    }
    ui_memory_stats_t before = ui_memory_stats();
    size_t core_objects = object_count(lv_screen_active()), core_timers = timer_count();
    size_t max_objects = core_objects, first_growth = 0;
    bool bounded = true;
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        app_view_t state = panel_view(&s_panels[cycle % count], dark);
        state.ui_revision = ++revision;
        app_ui_render(&state);
        advance(100);
        size_t objects = object_count(lv_screen_active());
        if (objects > max_objects) max_objects = objects;
        if (objects > PANEL_OBJECT_LIMIT) bounded = false;
        core.ui_revision = ++revision;
        app_ui_render(&core);
        advance(100);
        ui_memory_stats_t current = ui_memory_stats();
        assert(ui_private_buffers_cleared());
        if (current.live_bytes != before.live_bytes || current.live_blocks != before.live_blocks ||
            object_count(lv_screen_active()) != core_objects || timer_count() != core_timers) {
            bounded = false;
            if (!first_growth) first_growth = cycle + 1;
        }
    }
    ui_memory_stats_t after = ui_memory_stats();
    bool passed = bounded && core_objects <= CORE_OBJECT_LIMIT && after.failures == 0;
    fprintf(s_report, "%s\n    {\"dark\":%s,\"open_close_cycles\":1000,\"panel_variants\":%zu,"
        "\"core_objects\":%zu,\"peak_objects\":%zu,\"core_timers\":%zu,"
        "\"live_bytes_before\":%zu,\"live_bytes_after\":%zu,\"live_blocks_before\":%zu,"
        "\"live_blocks_after\":%zu,\"peak_live_bytes\":%zu,\"first_growth_cycle\":%zu,\"passed\":%s}",
        dark ? "," : "", dark ? "true" : "false", count, core_objects, max_objects, core_timers,
        before.live_bytes, after.live_bytes, before.live_blocks, after.live_blocks,
        after.peak_live_bytes, first_growth, passed ? "true" : "false");
    return passed;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_report = fopen(argv[1], "wb");
    assert(s_report);
    ui_memory_check_observer();
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw, NULL, sizeof s_draw, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    app_ui_create(display, reject_unexpected_intent);
    app_ui_set_local_edit_submit(reject_unexpected_private);
    fprintf(s_report, "{\n  \"render_mode\":\"FULL\",\"frame_bytes\":%u,\"refresh_ms\":%u,"
                      "\"dpi\":%u,\"results\":[", FRAME_BYTES, LV_DEF_REFR_PERIOD, LV_DPI_DEF);
    bool passed = true;
    for (unsigned dark = 0; dark < 2; ++dark)
        for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode)
            for (unsigned hidden = 0; hidden < 2; ++hidden)
                if (!idle_case(view((input_mode_t)mode, dark != 0), hidden != 0, false)) passed = false;
    for (unsigned dark = 0; dark < 2; ++dark) {
        for (unsigned scenario = 0; scenario < 9; ++scenario) {
            app_view_t audio = view(INPUT_AUDIO, dark != 0);
            ui_fixture_media(&audio);
            if (scenario >= 1 && scenario <= 4) {
                app_media_job_view_t job = {.known = true, .id = {{31}}, .consumer_generation = 47,
                    .state = scenario == 4 ? APP_MEDIA_JOB_STOPPING : APP_MEDIA_JOB_RUNNING, .can_stop = scenario != 4};
                if (scenario != 2) audio.media.live_capture = job;
                if (scenario >= 2) { audio.media.playback = job; audio.media.playback.id.bytes[0] = 33; }
                audio.media.microphone.can_start = audio.media.speaker.can_start = false;
                audio.media.state = scenario == 4 ? APP_MEDIA_STOPPING : APP_MEDIA_RUNNING;
                audio.capture_indicator_required = scenario != 2;
                audio.capture_indicator_generation = 59;
                audio.capture_state = scenario == 4 ? APP_CAPTURE_STOPPING : APP_CAPTURE_ACTIVE;
            }
            if (scenario == 5) {
                audio.media.playback.known = true; audio.media.playback.state = APP_MEDIA_JOB_ERROR;
                audio.media.speaker.can_start = false;
                strcpy(audio.media.playback.reason, "USB host stopped sending audio.");
            }
            if (scenario >= 6) {
                audio.media.command_pending = scenario == 6;
                audio.media.microphone.can_start = audio.media.speaker.can_start = false;
                audio.media.microphone.start_block = audio.media.speaker.start_block = scenario == 6 ?
                    APP_MEDIA_START_PENDING : scenario == 7 ? APP_MEDIA_START_HOST_STREAM_REQUIRED : APP_MEDIA_START_UNSUPPORTED;
            }
            if (!idle_case(audio, true, false)) passed = false;
        }
        const app_git_state_t git_states[] = {APP_GIT_DIRTY, APP_GIT_UNKNOWN, (app_git_state_t)99};
        for (unsigned i = 0; i < sizeof(git_states) / sizeof(git_states[0]); ++i) {
            panel_case_t about = {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_ABOUT, APP_CONFIRM_NONE};
            app_view_t metadata = panel_view(&about, dark != 0);
            metadata.git_state = git_states[i];
            if (i == 2) {
                memset(metadata.developer, 'D', sizeof(metadata.developer));
                memset(metadata.repository_url, 'R', sizeof(metadata.repository_url));
                memset(metadata.git_commit, 'a', sizeof(metadata.git_commit));
            }
            if (!idle_case(metadata, false, false)) passed = false;
        }
        for (unsigned reference = APP_REFERENCE_UNKNOWN; reference <= APP_REFERENCE_UNSTABLE; ++reference) {
            app_view_t grip = view(INPUT_MOUSE, dark != 0);
            grip.motion_reason = APP_MOTION_READY;
            grip.motion_enabled = true;
            grip.motion_recenter_available = reference >= APP_REFERENCE_ACQUIRING;
            grip.motion_settings.reference = (app_motion_reference_t)reference;
            if (!idle_case(grip, false, false)) passed = false;
        }
        for (size_t i = 0; i < sizeof s_panels / sizeof s_panels[0]; ++i)
            if (!idle_case(panel_view(&s_panels[i], dark != 0), false, false)) passed = false;
        app_view_t sleeping = view(INPUT_MOUSE, dark != 0);
        sleeping.screen_off = true;
        if (!idle_case(sleeping, true, false)) passed = false;
        panel_case_t settings = {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_HOME, APP_CONFIRM_NONE};
        if (!idle_case(panel_view(&settings, dark != 0), false, true)) passed = false;
        app_view_t capture = view(INPUT_MOUSE, dark != 0);
        capture.capture_indicator_required = true;
        capture.capture_indicator_generation = 91;
        capture.capture_state = APP_CAPTURE_UNCERTAIN;
        if (!idle_case(capture, false, false)) passed = false;
        capture.overlay = APP_OVERLAY_SETTINGS;
        capture.settings_page = APP_SETTINGS_MOTION;
        if (!idle_case(capture, false, false)) passed = false;
        capture.locked = true;
        if (!idle_case(capture, false, false)) passed = false;
    }
    fprintf(s_report, "\n  ],\"navigation\":[");
    if (!navigation_case(false)) passed = false;
    if (!navigation_case(true)) passed = false;
    app_view_t changed = view(INPUT_KEYBOARD, false);
    app_ui_render(&changed);
    advance(1000);
    size_t before = s_flushes;
    strcpy(changed.preview, "Visible change");
    app_ui_render(&changed);
    advance(100);
    bool positive_control = s_flushes > before;
    passed = passed && positive_control;
    size_t visible_flushes = s_flushes - before;
    lv_deinit();
    ui_memory_stats_t final = ui_memory_stats();
    passed = passed && final.live_bytes == 0 && final.live_blocks == 0;
    fprintf(s_report, "\n  ],\"visible_change_flushes\":%zu,\"final_live_bytes\":%zu,"
            "\"final_live_blocks\":%zu,\"passed\":%s}\n",
            visible_flushes, final.live_bytes, final.live_blocks, passed ? "true" : "false");
    assert(fclose(s_report) == 0);
    printf("%s: FULL-render idle/hidden-state resource checks; measurements in %s\n",
           passed ? "PASS" : "FAIL", argv[1]);
    return passed ? 0 : 1;
}
