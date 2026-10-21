// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include "ui_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 480
#define HEIGHT 480
#define IDLE_MS 10000
#define FRAME_BYTES (WIDTH * HEIGHT * 2U)
/* design/ui-plan.md: fixed input pages preserve gestures; one panel is dynamic. */
#define CORE_OBJECT_LIMIT 102
#define PANEL_OBJECT_LIMIT 160

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
    {INPUT_KEYBOARD, APP_OVERLAY_KEYBOARD, 0, 0},
    {INPUT_AUDIO, APP_OVERLAY_AUDIO, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_WIFI_SETUP, 0, 0},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_OPEN},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_CLOSE},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_PAIR_FORGET_ALL},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_WIFI_FORGET},
    {INPUT_MOUSE, APP_OVERLAY_CONFIRM, 0, APP_CONFIRM_DISCONNECT},
};

static unsigned char s_draw[FRAME_BYTES];
static size_t s_flushes, s_flush_bytes;
static FILE *s_report;
static bool s_first_result = true;

static bool reject_unexpected_intent(app_intent_t *intent)
{
    (void)intent;
    assert(!"Rendering alone must never submit product intent");
    return false;
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
        .firmware_version = "resource-fixture", .uptime_seconds = 60};
    static const char *const cells[] = {"ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"};
    for (size_t i = 0; i < 9; ++i) strcpy(state.keyboard_cells[i], cells[i]);
    strcpy(state.bluetooth_target_name, "Resource test peer");
    strcpy(state.bluetooth_hosts[0].name, "Hidden saved peer");
    strcpy(state.bluetooth_hosts[0].identity, "AA:BB:CC:DD:EE:01");
    strcpy(state.wifi_ssid, "Synthetic network");
    return state;
}

static app_view_t panel_view(const panel_case_t *panel, bool dark)
{
    app_view_t state = view(panel->mode, dark);
    state.overlay = panel->overlay;
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
            strcpy(state.bluetooth_hosts[0].name, elapsed % 200 ? "Hidden peer A" : "Hidden peer B");
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
        "\"covered_target_changes\":%s,\"overlay\":%d,\"settings_page\":%d,\"confirmation\":%d,\"screen_off\":%s,\"object_limit\":%zu,"
        "\"simulated_ms\":%u,\"snapshots\":100,\"objects\":%zu,\"timers\":%zu,"
        "\"allocations\":%zu,\"reallocations\":%zu,\"frees\":%zu,\"flushes\":%zu,"
        "\"flush_bytes\":%zu,\"live_bytes_before\":%zu,\"live_bytes_after\":%zu,"
        "\"peak_live_bytes\":%zu,\"quiet\":%s,\"bounded\":%s,\"passed\":%s}",
        s_first_result ? "" : ",", (int)state.mode, state.dark_theme ? "true" : "false", hidden_changes ? "true" : "false",
        covered_target ? "true" : "false", (int)state.overlay, (int)state.settings_page,
        (int)state.confirmation, state.screen_off ? "true" : "false", limit,
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
    fprintf(s_report, "{\n  \"render_mode\":\"FULL\",\"frame_bytes\":%u,\"refresh_ms\":%u,"
                      "\"dpi\":%u,\"results\":[", FRAME_BYTES, LV_DEF_REFR_PERIOD, LV_DPI_DEF);
    bool passed = true;
    for (unsigned dark = 0; dark < 2; ++dark)
        for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode)
            for (unsigned hidden = 0; hidden < 2; ++hidden)
                if (!idle_case(view((input_mode_t)mode, dark != 0), hidden != 0, false)) passed = false;
    for (unsigned dark = 0; dark < 2; ++dark) {
        for (size_t i = 0; i < sizeof s_panels / sizeof s_panels[0]; ++i)
            if (!idle_case(panel_view(&s_panels[i], dark != 0), false, false)) passed = false;
        app_view_t sleeping = view(INPUT_MOUSE, dark != 0);
        sleeping.screen_off = true;
        if (!idle_case(sleeping, true, false)) passed = false;
        panel_case_t settings = {INPUT_MOUSE, APP_OVERLAY_SETTINGS, APP_SETTINGS_HOME, APP_CONFIRM_NONE};
        if (!idle_case(panel_view(&settings, dark != 0), false, true)) passed = false;
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
