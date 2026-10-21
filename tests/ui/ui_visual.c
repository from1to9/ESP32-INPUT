// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 480
#define HEIGHT 480
#define INTENTS 512

static unsigned char s_frame[WIDTH * HEIGHT * 3];
static unsigned char s_draw[WIDTH * HEIGHT * 2];
static app_intent_t s_intents[INTENTS];
static size_t s_intent_count;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_pointer_data;
static app_view_t s_state;
static const char *s_output;
static const char *s_prefix = "";

static bool capture(app_intent_t *intent)
{
    assert(s_intent_count < INTENTS);
    s_intents[s_intent_count++] = *intent;
    return true;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT);
            size_t offset = ((size_t)y * WIDTH + (size_t)x) * 3;
            uint16_t rgb = (uint16_t)(pixels[0] | (uint16_t)pixels[1] << 8);
            s_frame[offset] = (unsigned char)(((rgb >> 11) & 31U) * 255U / 31U);
            s_frame[offset + 1] = (unsigned char)(((rgb >> 5) & 63U) * 255U / 63U);
            s_frame[offset + 2] = (unsigned char)((rgb & 31U) * 255U / 31U);
            pixels += 2;
        }
    }
    lv_display_flush_ready(display);
}

static void read_pointer(lv_indev_t *device, lv_indev_data_t *data)
{
    (void)device;
    *data = s_pointer_data;
}

static void render(void)
{
    app_ui_render(&s_state);
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(NULL);
}

static void inspect_visible_geometry(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    if (lv_obj_check_type(object, &lv_label_class) || lv_obj_check_type(object, &lv_button_class)) {
        assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < WIDTH && area.y2 < HEIGHT);
        lv_area_t parent;
        lv_obj_get_coords(lv_obj_get_parent(object), &parent);
        assert(area.x1 >= parent.x1 && area.x2 <= parent.x2);
        assert(area.y1 >= parent.y1 && area.y2 <= parent.y2);
    }
    if (lv_obj_check_type(object, &lv_button_class)) {
        assert(area.x2 - area.x1 + 1 >= 44 && area.y2 - area.y1 + 1 >= 44);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        inspect_visible_geometry(lv_obj_get_child(object, (int32_t)i));
    }
}

static void screenshot(const char *name)
{
    render();
    inspect_visible_geometry(lv_screen_active());
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s%s.ppm", s_output, s_prefix, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P6\n480 480\n255\n") > 0);
    assert(fwrite(s_frame, sizeof(s_frame), 1, file) == 1);
    assert(fclose(file) == 0);
}

static void pointer(int x, int y, bool down)
{
    s_pointer_data.point = (lv_point_t){x, y};
    s_pointer_data.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_tick_inc(20);
    lv_indev_read(s_pointer);
    lv_timer_handler();
}

static void tap(int x, int y)
{
    pointer(x, y, true);
    pointer(x, y, false);
}

static size_t action_count(app_action_t action)
{
    size_t count = 0;
    for (size_t i = 0; i < s_intent_count; ++i) count += s_intents[i].action == action;
    return count;
}

static const app_intent_t *last_action(app_action_t action)
{
    for (size_t i = s_intent_count; i; --i) {
        if (s_intents[i - 1].action == action) return &s_intents[i - 1];
    }
    assert(!"Expected action was not emitted");
    return NULL;
}

static void clear_intents(void)
{
    s_intent_count = 0;
}

static void grid_groups(void)
{
    const char *groups[] = {"ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"};
    s_state.keyboard_group = -1;
    s_state.keyboard_cell_count = APP_UI_GRID_CELLS;
    for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) {
        snprintf(s_state.keyboard_cells[i], sizeof(s_state.keyboard_cells[i]), "%s", groups[i]);
    }
}

static lv_obj_t *find_visible_label(lv_obj_t *object, const char *value)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) &&
        strstr(lv_label_get_text(object), value)) return object;
    uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *found = find_visible_label(lv_obj_get_child(object, (int32_t)i), value);
        if (found) return found;
    }
    return NULL;
}

static bool has_visible_label(lv_obj_t *object, const char *value)
{
    return find_visible_label(object, value) != NULL;
}

static lv_obj_t *control(const char *caption)
{
    lv_obj_t *found = find_visible_label(lv_screen_active(), caption);
    assert(found);
    lv_obj_t *parent = lv_obj_get_parent(found);
    assert(lv_obj_check_type(parent, &lv_button_class));
    return parent;
}

static void tap_control(const char *caption)
{
    lv_area_t area;
    lv_obj_get_coords(control(caption), &area);
    tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
}

static void row_geometry(const char *const *captions, unsigned count)
{
    lv_area_t previous = {0};
    int first_baseline = 0;
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *object = control(captions[i]);
        lv_area_t area, caption_area;
        lv_obj_get_coords(object, &area);
        lv_obj_get_coords(lv_obj_get_child(object, 0), &caption_area);
        assert(area.y2 - area.y1 + 1 >= 44 && area.x2 - area.x1 + 1 >= 44);
        if (i) {
            assert(area.y1 == previous.y1 && area.y2 == previous.y2);
            assert(area.x2 - area.x1 == previous.x2 - previous.x1);
            assert(area.x1 - previous.x2 - 1 >= 8);
            assert(caption_area.y1 == first_baseline);
        } else first_baseline = caption_area.y1;
        assert(caption_area.x1 >= area.x1 && caption_area.x2 <= area.x2);
        assert(caption_area.y1 >= area.y1 && caption_area.y2 <= area.y2);
        previous = area;
    }
}

static void geometry_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_KEYBOARD;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    grid_groups();
    s_state.sending = false;
    render();
    const char *tabs[] = {"Motion", "Touch", "Spoken", "Shortcuts"};
    const char *grid1[] = {"ABCD", "EFGH", "IJKL"};
    const char *grid2[] = {"MNOP", "QRST", "UVWX"};
    const char *grid3[] = {"YZ01", "2345", "6789"};
    const char *actions[] = {"Delete", "Space", "Send"};
    const char *nav[] = {"Mouse", "Keyboard", "Audio"};
    const char *utilities[] = {"Groups", "ABC", "Symbols"};
    row_geometry(tabs, 4);
    row_geometry(grid1, 3);
    row_geometry(grid2, 3);
    row_geometry(grid3, 3);
    row_geometry(actions, 3);
    row_geometry(nav, 3);
    for (unsigned i = 0; i < 3; ++i) {
        lv_area_t a, b, c, utility, row;
        lv_obj_get_coords(control(grid1[i]), &a);
        lv_obj_get_coords(control(grid2[i]), &b);
        lv_obj_get_coords(control(grid3[i]), &c);
        assert(a.x1 == b.x1 && b.x1 == c.x1 && a.x2 == b.x2 && b.x2 == c.x2);
        const char *row_names[] = {"ABCD", "MNOP", "YZ01"};
        lv_obj_get_coords(control(row_names[i]), &row);
        lv_obj_get_coords(control(utilities[i]), &utility);
        assert(utility.x1 == 376 && utility.y1 == row.y1 && utility.y2 == row.y2);
    }
    for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) {
        clear_intents();
        tap(72 + (int)(i % 3) * 120, 206 + (int)(i / 3) * 52);
        assert(action_count(APP_GRID) == 1 && last_action(APP_GRID)->x == (int)i);
        assert(action_count(APP_TEXT) == 0 && action_count(APP_SEND) == 0);
    }
    lv_area_t target, status;
    lv_obj_get_coords(find_visible_label(lv_screen_active(), "HID test peer"), &target);
    lv_obj_get_coords(find_visible_label(lv_screen_active(), "Input ready"), &status);
    assert(target.y2 < status.y1 && status.y2 < 72);
    s_state = original;
    render();
    puts("PASS: keyboard rows share columns, button sizes and text baselines; header name/status do not overlap");
}

static bool contains_text(lv_obj_t *object, const char *value)
{
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), value)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        if (contains_text(lv_obj_get_child(object, (int32_t)i), value)) return true;
    }
    return false;
}

static void device_settings_checks(void)
{
    app_view_t original = s_state;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    for (int page = APP_SETTINGS_HOME; page <= APP_SETTINGS_WIFI; ++page) {
        const char *names[] = {"settings-home", "settings-connectivity", "settings-display", "settings-input", "settings-about", "settings-bluetooth", "settings-wifi"};
        s_state.settings_page = (app_settings_page_t)page;
        screenshot(names[page]);
    }
    s_state.settings_page = APP_SETTINGS_DISPLAY;
    render();
    const char *timeout_names[] = {"Off", "1 min", "5 min", "10 min", "30 min"};
    const int seconds[] = {0, 60, 300, 600, 1800};
    row_geometry(timeout_names, 5);
    for (unsigned i = 0; i < 5; ++i) {
        clear_intents();
        tap_control(timeout_names[i]);
        assert(action_count(APP_IDLE_TIMEOUT) == 1 && last_action(APP_IDLE_TIMEOUT)->x == seconds[i]);
        assert(s_state.idle_timeout_seconds == 300);
    }
    clear_intents();
    pointer(32, 254, true);
    pointer(448, 254, true);
    assert(action_count(APP_BRIGHTNESS) == 0);
    pointer(448, 254, false);
    assert(action_count(APP_BRIGHTNESS) == 1 && last_action(APP_BRIGHTNESS)->x == 100);
    assert(s_state.brightness_percent == 70);
    clear_intents();
    tap(32, 254);
    assert(action_count(APP_BRIGHTNESS) == 1 && last_action(APP_BRIGHTNESS)->x == 10);
    s_state.settings_save_state = APP_SAVE_DEFAULTS;
    screenshot("settings-defaults");
    assert(has_visible_label(lv_screen_active(), "Defaults active"));
    s_state.settings_save_state = APP_SAVE_PENDING;
    screenshot("settings-saving");
    s_state.settings_save_state = APP_SAVE_ERROR;
    screenshot("settings-save-error");
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    render();
    clear_intents();
    tap_control("Bluetooth hosts");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_BLUETOOTH);
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    render();
    tap_control("Add host");
    assert(action_count(APP_PAIRING_REQUEST) == 1 && last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_OPEN);
    assert(action_count(APP_CONFIRM) == 0 && s_state.pairing_state == APP_PAIRING_CLOSED);
    tap_control("Forget all");
    assert(last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_FORGET_ALL);
    tap_control("Disconnect");
    assert(last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_DISCONNECT);
    assert(s_state.connected);
    s_state.settings_page = APP_SETTINGS_WIFI;
    render();
    tap_control("Forget network");
    assert(action_count(APP_WIFI_FORGET_REQUEST) == 1 && s_state.wifi_saved_network);
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    s_state.pairing_state = APP_PAIRING_OPEN;
    s_state.pairing_seconds_left = 120;
    screenshot("settings-pairing-open");
    clear_intents();
    tap_control("Close pairing");
    assert(last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_CLOSE);
    s_state.overlay = APP_OVERLAY_CONFIRM;
    for (int confirmation = APP_CONFIRM_PAIR_OPEN; confirmation <= APP_CONFIRM_DISCONNECT; ++confirmation) {
        const char *names[] = {"", "confirm-pair-open", "confirm-pair-close", "confirm-pair-forget", "confirm-wifi-forget", "confirm-disconnect"};
        s_state.confirmation = (app_confirmation_t)confirmation;
        screenshot(names[confirmation]);
        clear_intents();
        tap(348, 448);
        assert(action_count(APP_CONFIRM) == 1 && last_action(APP_CONFIRM)->x == 1);
        tap_control("Cancel");
        assert(last_action(APP_CONFIRM)->x == 0);
        assert(action_count(APP_PAIRING_REQUEST) == 0 && action_count(APP_WIFI_FORGET_REQUEST) == 0);
    }
    s_state.overlay = APP_OVERLAY_WIFI_SETUP;
    s_state.wifi_setup_active = false;
    s_state.wifi_operation_pending = true;
    screenshot("wifi-setup-starting");
    s_state.wifi_setup_active = true;
    s_state.wifi_operation_pending = false;
    s_state.wifi_setup_seconds_left = 599;
    strcpy(s_state.wifi_setup_ssid, "TEST-SETUP-ABCDEFGHIJKLMNOPQRSTU");
    strcpy(s_state.wifi_setup_passcode, "TESTONLY-123456789012345");
    strcpy(s_state.wifi_setup_url, "http://192.0.2.1/");
    screenshot("wifi-setup-active");
    assert(contains_text(lv_screen_active(), "TESTONLY"));
    assert(strcmp(lv_label_get_text(find_visible_label(lv_screen_active(), "TEST-SETUP")), s_state.wifi_setup_ssid) == 0);
    assert(strcmp(lv_label_get_text(find_visible_label(lv_screen_active(), "TESTONLY")), s_state.wifi_setup_passcode) == 0);
    clear_intents();
    tap_control("Stop setup");
    assert(action_count(APP_WIFI_SETUP) == 1 && last_action(APP_WIFI_SETUP)->x == 0);
    assert(s_state.wifi_setup_active);
    s_state.locked = true;
    render();
    assert(!contains_text(lv_screen_active(), "TESTONLY"));
    s_state.locked = false;
    render();
    s_state.overlay = APP_OVERLAY_SETTINGS;
    render();
    assert(!contains_text(lv_screen_active(), "TESTONLY"));
    s_state.overlay = APP_OVERLAY_WIFI_SETUP;
    s_state.wifi_setup_active = false;
    screenshot("wifi-setup-ended");
    assert(!contains_text(lv_screen_active(), "TESTONLY"));
    s_state.wifi_operation_pending = true;
    strcpy(s_state.wifi_message, "Setup closing; retrying shutdown.");
    screenshot("wifi-setup-closing");
    assert(has_visible_label(lv_screen_active(), "Setup closing"));
    assert(!has_visible_label(lv_screen_active(), "Preparing protected"));
    assert(!contains_text(lv_screen_active(), "TESTONLY"));
    s_state.wifi_operation_pending = false;
    s_state.wifi_saved_network = true; /* Forget is useful for uncertain stored metadata. */
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_WIFI;
    strcpy(s_state.wifi_message, "Saved network uncertain; set up again or forget.");
    screenshot("settings-wifi-storage-uncertain");
    assert(has_visible_label(lv_screen_active(), "Saved network uncertain"));
    clear_intents();
    tap_control("Forget network");
    assert(action_count(APP_WIFI_FORGET_REQUEST) == 1 && s_state.wifi_saved_network);
    strcpy(s_state.wifi_message, "Saved network error; forget to clear.");
    screenshot("settings-wifi-storage-error");
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    screenshot("connectivity-wifi-storage-error");
    assert(has_visible_label(lv_screen_active(), "Saved network error; forget to clear."));
    s_state = original;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.battery_percent_valid = false;
    s_state.wifi_rssi_valid = false;
    s_state.paired_host_count = -1;
    s_state.pairing_state = APP_PAIRING_UNAVAILABLE;
    s_state.wifi_state = APP_UI_WIFI_UNAVAILABLE;
    screenshot("settings-unknown-status");
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    render();
    clear_intents();
    tap(88, 354);
    s_state.settings_page = APP_SETTINGS_WIFI;
    render();
    tap_control("Set up Wi-Fi");
    assert(action_count(APP_PAIRING_REQUEST) == 0 && action_count(APP_WIFI_SETUP) == 0);
    s_state = original;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.settings_page = APP_SETTINGS_WIFI;
    const app_ui_wifi_state_t states[] = {APP_UI_WIFI_DISCONNECTED, APP_UI_WIFI_CONNECTING,
        APP_UI_WIFI_RETRYING, APP_UI_WIFI_ERROR};
    const char *state_names[] = {"settings-wifi-disconnected", "settings-wifi-connecting",
        "settings-wifi-retrying", "settings-wifi-error"};
    for (unsigned i = 0; i < 4; ++i) {
        s_state.wifi_state = states[i];
        screenshot(state_names[i]);
    }
    s_state.wifi_state = APP_UI_WIFI_CONNECTED;
    s_state.wifi_rssi_dbm = -88;
    strcpy(s_state.wifi_ssid, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    s_state.battery_percent = 8;
    screenshot("settings-weak-signal");
    assert(has_visible_label(lv_screen_active(), "-88 dBm"));
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.pairing_state = APP_PAIRING_CLEARING;
    screenshot("settings-pairing-updating");
    assert(has_visible_label(lv_screen_active(), "Updating pairing"));
    s_state.pairing_state = APP_PAIRING_ERROR;
    screenshot("settings-pairing-error");
    s_state.settings_page = APP_SETTINGS_ABOUT;
    strcpy(s_state.firmware_version, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    strcpy(s_state.build_id, "WWWWWWWWWWWWWWWWWWWWWWW");
    strcpy(s_state.board_revision, "WWWWWWWWWWWWWWWWWWWWWWW");
    strcpy(s_state.device_id, "WWWWWWWWWWWWWWWWWWWWWWW");
    screenshot("settings-about-long");
    s_state = original;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_ABOUT;
    s_state.battery_state = APP_BATTERY_CHARGING;
    screenshot("settings-charging");
    assert(has_visible_label(lv_screen_active(), "Charging"));
    assert(has_visible_label(lv_screen_active(), "USB power detection unavailable"));
    s_state.battery_state = APP_BATTERY_FULL;
    screenshot("settings-battery-full");
    s_state.battery_supported = false;
    s_state.battery_percent_valid = false;
    screenshot("settings-no-battery");
    assert(has_visible_label(lv_screen_active(), "Battery unavailable"));
    s_state = original;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    render();
    assert(has_visible_label(lv_screen_active(), "Connected"));
    assert(has_visible_label(lv_screen_active(), "AA:BB:CC:DD:EE:00"));
    clear_intents();
    tap_control("Next");
    assert(action_count(APP_BLUETOOTH_PAGE) == 1 && last_action(APP_BLUETOOTH_PAGE)->x == 1);
    assert(s_state.bluetooth_page == 0);
    s_state.bluetooth_page = 1;
    ++s_state.ui_revision;
    screenshot("settings-bluetooth-next");
    assert(has_visible_label(lv_screen_active(), "AA:BB:CC:DD:EE:04"));
    assert(!has_visible_label(lv_screen_active(), "AA:BB:CC:DD:EE:00"));
    clear_intents();
    tap_control("Next");
    assert(action_count(APP_BLUETOOTH_PAGE) == 0);
    tap_control("Previous");
    assert(last_action(APP_BLUETOOTH_PAGE)->x == 0);
    clear_intents();
    tap(38, 94);
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_CONNECTIVITY);
    s_state.bluetooth_host_count = APP_UI_BOND_CAPACITY;
    s_state.paired_host_count = APP_UI_BOND_CAPACITY;
    s_state.paired_host_capacity = APP_UI_BOND_CAPACITY;
    s_state.bluetooth_page = 7;
    for (unsigned i = 0; i < APP_UI_BOND_CAPACITY; ++i) {
        snprintf(s_state.bluetooth_hosts[i].identity, sizeof(s_state.bluetooth_hosts[i].identity), "AA:BB:CC:DD:EE:%02X", i);
    }
    screenshot("settings-bluetooth-last");
    assert(has_visible_label(lv_screen_active(), "AA:BB:CC:DD:EE:1F"));
    assert(has_visible_label(lv_screen_active(), "8 / 8"));
    s_state.bluetooth_host_count = 0;
    s_state.paired_host_count = 0;
    screenshot("settings-bluetooth-empty");
    assert(has_visible_label(lv_screen_active(), "No saved hosts"));
    s_state.bluetooth_hosts_available = false;
    screenshot("settings-bluetooth-unknown");
    assert(has_visible_label(lv_screen_active(), "Saved hosts unavailable"));
    s_state = original;
    s_state.target_name = "Host AA:BB:CC:DD:EE:FF";
    screenshot("target-identity-header");
    s_state.overlay = APP_OVERLAY_TARGET;
    screenshot("target-identity-detail");
    assert(has_visible_label(lv_screen_active(), "Host AA:BB:CC:DD:EE:FF"));
    s_state = original;
    s_state.mode = INPUT_MOUSE;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.screen_off = true;
    screenshot("screen-off");
    for (unsigned i = 0; i < sizeof(s_frame); ++i) assert(s_frame[i] == 0);
    clear_intents();
    pointer(80, 366, true);
    assert(action_count(APP_TOUCH_BEGIN) == 1);
    s_state.screen_off = false;
    ++s_state.ui_revision;
    render();
    pointer(80, 366, false);
    assert(action_count(APP_CLICK) == 0);
    assert(action_count(APP_TOUCH_END) + action_count(APP_TOUCH_CANCEL) == 1);
    tap(80, 366);
    assert(action_count(APP_CLICK) == 1);
    s_state = original;
    render();
    clear_intents();
    puts("PASS: settings intents await coordinator state; confirmations, defaults/persistence, brightness bounds, setup secrets and consumed wake gesture");
}

static void pairing_capacity_checks(void)
{
    app_view_t original = s_state;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    s_state.pairing_state = APP_PAIRING_CLOSED;
    s_state.paired_host_count = 15;
    s_state.paired_host_capacity = 15;
    s_state.bluetooth_host_count = 15;
    s_state.bluetooth_page = 0;
    for (unsigned i = 0; i < 15; ++i)
        snprintf(s_state.bluetooth_hosts[i].identity, sizeof(s_state.bluetooth_hosts[i].identity), "AA:BB:CC:DD:EE:%02X", i);
    screenshot("bluetooth-capacity-full");
    assert(has_visible_label(lv_screen_active(), "Full: 15/15"));
    clear_intents();
    tap_control("Pairings full");
    assert(action_count(APP_PAIRING_REQUEST) == 0);
    assert(s_state.paired_host_count == 15); /* No implicit eviction. */
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    screenshot("bluetooth-capacity-summary");
    assert(has_visible_label(lv_screen_active(), "15/15 saved / full"));
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    s_state.paired_host_count = 16;
    render();
    clear_intents();
    tap_control("Pairings full");
    assert(action_count(APP_PAIRING_REQUEST) == 0);
    s_state.paired_host_count = 14;
    s_state.bluetooth_host_count = 14;
    render();
    clear_intents();
    tap_control("Add host");
    assert(action_count(APP_PAIRING_REQUEST) == 1 && last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_OPEN);
    s_state.paired_host_capacity = 0;
    screenshot("bluetooth-capacity-unknown");
    clear_intents();
    tap(88, 354);
    assert(action_count(APP_PAIRING_REQUEST) == 0);
    assert(!has_visible_label(lv_screen_active(), "Pairings full"));
    s_state.paired_host_capacity = 15;
    s_state.paired_host_count = -1;
    screenshot("bluetooth-count-unknown");
    clear_intents();
    tap(88, 354);
    assert(action_count(APP_PAIRING_REQUEST) == 0);
    assert(!has_visible_label(lv_screen_active(), "Pairings full"));
    s_state.pairing_state = APP_PAIRING_OPEN;
    render();
    clear_intents();
    tap_control("Close pairing");
    assert(action_count(APP_PAIRING_REQUEST) == 1 && last_action(APP_PAIRING_REQUEST)->x == APP_PAIR_CLOSE);
    s_state = original;
    render();
    clear_intents();
    puts("PASS: actual pairing count/capacity, full/over-capacity admission, unknown fail-closed state and close-pairing escape");
}

static void transport_and_grid_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_KEYBOARD;
    s_state.keyboard_submode = APP_KEYBOARD_MOTION;
    s_state.overlay = APP_OVERLAY_NONE;
    grid_groups();
    s_state.keyboard_focus = 8;
    screenshot("keyboard-ninth-focus");
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    s_state.keyboard_group = 8;
    s_state.keyboard_cell_count = 4;
    memset(s_state.keyboard_cells, 0, sizeof(s_state.keyboard_cells));
    for (unsigned i = 0; i < 4; ++i) snprintf(s_state.keyboard_cells[i], sizeof(s_state.keyboard_cells[i]), "%c", '6' + (int)i);
    screenshot("keyboard-four-characters");
    for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) {
        clear_intents();
        tap(72 + (int)(i % 3) * 120, 206 + (int)(i / 3) * 52);
        assert(action_count(APP_GRID) == (i < 4 ? 1U : 0U));
        if (i < 4) assert(last_action(APP_GRID)->x == (int)i);
        assert(action_count(APP_SEND) == 0 && action_count(APP_TEXT) == 0);
    }
    /* A hole inside a counted range remains non-actionable. */
    s_state.keyboard_cells[2][0] = '\0';
    render();
    clear_intents();
    tap(312, 206);
    assert(action_count(APP_GRID) == 0);
    s_state = original;
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.usb_available = false;
    screenshot("transport-usb-unavailable");
    clear_intents();
    tap_control("USB unavailable");
    assert(action_count(APP_TRANSPORT_SELECT) == 0);
    s_state.usb_available = true;
    s_state.usb_connected = false;
    s_state.usb_ready = false;
    render();
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        s_state.mode = (input_mode_t)mode;
        render();
        clear_intents();
        tap_control("USB");
        assert(action_count(APP_TRANSPORT_SELECT) == 1 && last_action(APP_TRANSPORT_SELECT)->x == APP_INPUT_USB);
        assert(s_state.input_transport == APP_INPUT_BLUETOOTH);
        assert(action_count(APP_MODE) == 0 && action_count(APP_CLICK) == 0 && action_count(APP_AUDIO_POWER) == 0);
    }
    s_state.transport_switch_pending = true;
    s_state.requested_transport = APP_INPUT_USB;
    screenshot("transport-switching-usb");
    assert(has_visible_label(lv_screen_active(), "Switching to USB"));
    clear_intents();
    tap_control("USB");
    tap_control("Bluetooth");
    assert(action_count(APP_TRANSPORT_SELECT) == 0);
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.mode = INPUT_MOUSE;
    render();
    clear_intents();
    tap_control("Left click");
    assert(action_count(APP_CLICK) == 0);
    s_state.transport_switch_pending = false;
    s_state.input_transport = APP_INPUT_USB;
    s_state.connected = false;
    s_state.ready = false;
    s_state.target_name = "USB host";
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    screenshot("transport-usb-disconnected");
    assert(has_visible_label(lv_screen_active(), "Connect USB"));
    s_state.usb_connected = true;
    s_state.connected = true;
    screenshot("transport-usb-waiting");
    assert(has_visible_label(lv_screen_active(), "Connecting input"));
    s_state.usb_ready = true;
    s_state.ready = true;
    screenshot("transport-usb-ready");
    assert(has_visible_label(lv_screen_active(), "USB selected / Input ready"));
    s_state.overlay = APP_OVERLAY_TARGET;
    screenshot("target-usb");
    assert(has_visible_label(lv_screen_active(), "USB input"));
    assert(!has_visible_label(lv_screen_active(), "Pair with ESP32-INPUT"));
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.mode = INPUT_MOUSE;
    screenshot("usb-mouse");
    clear_intents();
    tap_control("Left click");
    assert(action_count(APP_CLICK) == 1);
    assert(action_count(APP_TRANSPORT_SELECT) == 0);
    s_state.mode = INPUT_KEYBOARD;
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    grid_groups();
    strcpy(s_state.preview, "USB draft");
    screenshot("usb-keyboard");
    clear_intents();
    tap_control("Send");
    assert(action_count(APP_SEND) == 1 && action_count(APP_KEY) == 0);
    s_state.mode = INPUT_AUDIO;
    s_state.audio_available = false;
    screenshot("usb-audio-unavailable");
    clear_intents();
    tap_control("Start audio");
    assert(action_count(APP_AUDIO_POWER) == 0);
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.bluetooth_connected = false;
    s_state.bluetooth_ready = false;
    screenshot("transport-usb-bluetooth-offline");
    assert(has_visible_label(lv_screen_active(), "USB selected / Input ready"));
    assert(has_visible_label(lv_screen_active(), "Not connected"));
    s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    render();
    clear_intents();
    tap_control("Disconnect");
    assert(action_count(APP_PAIRING_REQUEST) == 0);
    s_state.settings_page = APP_SETTINGS_CONNECTIVITY;
    s_state.transport_switch_pending = true;
    s_state.requested_transport = APP_INPUT_BLUETOOTH;
    screenshot("transport-switching-bluetooth");
    s_state.transport_switch_pending = false;
    s_state.transport_error = true;
    strcpy(s_state.transport_message, "USB connection failed. Reconnect the host.");
    screenshot("transport-usb-error");
    assert(has_visible_label(lv_screen_active(), "Input connection error"));
    s_state.transport_error = false;
    s_state.usb_available = false;
    screenshot("transport-selected-unavailable");
    assert(has_visible_label(lv_screen_active(), "Input unavailable"));
    s_state = original;
    render();
    clear_intents();
    puts("PASS: nine-cell hit map and four-character stage; independent Bluetooth/USB selection, pending/unavailable states and truthful audio capability");
}

static void text_boundary_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_KEYBOARD;
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    s_state.overlay = APP_OVERLAY_NONE;
    memset(s_state.preview, 'W', APP_UI_DRAFT_CAPACITY);
    s_state.preview[APP_UI_DRAFT_CAPACITY] = '\0';
    s_state.sending = false;
    s_state.send_total = 0;
    screenshot("keyboard-full-preview");
    lv_obj_t *preview = find_visible_label(lv_screen_active(), "WWWW");
    lv_obj_t *counter = find_visible_label(lv_screen_active(), "64 / 64");
    lv_obj_t *clear = find_visible_label(lv_screen_active(), "Clear");
    assert(preview && counter && clear);
    lv_area_t preview_area, counter_area, clear_area, container_area;
    lv_obj_get_coords(preview, &preview_area);
    lv_obj_get_coords(counter, &counter_area);
    lv_obj_get_coords(lv_obj_get_parent(clear), &clear_area);
    lv_obj_get_content_coords(lv_obj_get_parent(preview), &container_area);
    assert(preview_area.y2 < counter_area.y1);
    assert(preview_area.x2 < clear_area.x1);
    assert(counter_area.y2 <= container_area.y2);
    const lv_font_t *font = lv_obj_get_style_text_font(preview, LV_PART_MAIN);
    assert(lv_obj_get_height(preview) <= lv_font_get_line_height(font));
    assert(strstr(lv_label_get_text(preview), "..."));
    clear_intents();
    tap(370, 366);
    assert(action_count(APP_SEND) == 1 && action_count(APP_KEY) == 0);
    assert(strlen(s_state.preview) == APP_UI_DRAFT_CAPACITY);

    s_state.message = "Host stalled; pending input expired. Releases have priority.";
    screenshot("keyboard-stalled-footer");
    lv_obj_t *footer = find_visible_label(lv_screen_active(), "Host stalled");
    lv_obj_t *mouse_tab = find_visible_label(lv_screen_active(), "Mouse");
    assert(footer && mouse_tab);
    lv_area_t footer_area, nav_area;
    lv_obj_get_coords(footer, &footer_area);
    lv_obj_get_coords(lv_obj_get_parent(mouse_tab), &nav_area);
    assert(footer_area.y2 < nav_area.y1);
    font = lv_obj_get_style_text_font(footer, LV_PART_MAIN);
    assert(lv_obj_get_height(footer) <= lv_font_get_line_height(font));
    /* The smaller metadata font may fit this exact status without truncation;
     * its measured one-line bounds, not mandatory ellipsis, are the contract. */
    s_state.message = "Host stalled; pending input expired. Releases have priority. Reconnect from Bluetooth settings before sending again.";
    render();
    assert(strstr(lv_label_get_text(footer), "..."));

    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_INPUT;
    s_state.haptics_available = false;
    s_state.haptics_enabled = false;
    screenshot("settings-no-haptics");
    lv_obj_t *unavailable = find_visible_label(lv_screen_active(), "Unavailable");
    assert(unavailable);
    font = lv_obj_get_style_text_font(unavailable, LV_PART_MAIN);
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(unavailable), font,
        lv_obj_get_style_text_letter_space(unavailable, LV_PART_MAIN),
        lv_obj_get_style_text_line_space(unavailable, LV_PART_MAIN),
        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(text_size.x <= lv_obj_get_content_width(unavailable));
    assert(lv_obj_get_height(unavailable) <= lv_font_get_line_height(font));
    lv_obj_t *settings = lv_obj_get_parent(unavailable);
    lv_obj_t *haptics = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(settings); ++i) {
        lv_obj_t *child = lv_obj_get_child(settings, (int32_t)i);
        if (lv_obj_check_type(child, &lv_switch_class)) haptics = child;
    }
    assert(haptics);
    lv_area_t status_area, switch_area;
    lv_obj_get_coords(unavailable, &status_area);
    lv_obj_get_coords(haptics, &switch_area);
    assert(status_area.x2 < switch_area.x1);
    assert(status_area.y1 >= switch_area.y1 && status_area.y2 <= switch_area.y2);
    clear_intents();
    tap(420, 234);
    assert(action_count(APP_HAPTICS) == 0);
    s_state = original;
    render();
    clear_intents();
    puts("PASS: 64 wide characters stay above the counter; footer stays above navigation; unavailable haptics fits fully");
}

static void congestion_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.congested = true;
    s_state.motion_enabled = true;
    render();
    clear_intents();
    tap(80, 366);
    assert(action_count(APP_CLICK) == 1 && last_action(APP_CLICK)->x == 1);
    assert(action_count(APP_TOUCH_BEGIN) == 1 && action_count(APP_TOUCH_END) == 1);
    assert(s_intents[0].action == APP_TOUCH_BEGIN);
    assert(last_action(APP_CLICK)->gesture == last_action(APP_TOUCH_BEGIN)->gesture);
    assert(has_visible_label(lv_screen_active(), "Host busy"));
    clear_intents();
    tap(390, 86);
    assert(action_count(APP_MOTION) == 1 && last_action(APP_MOTION)->x == 0);
    assert(has_visible_label(lv_screen_active(), "Motion / on"));
    clear_intents();
    tap(440, 190);
    assert(action_count(APP_SCROLL) == 1 && last_action(APP_SCROLL)->x == 1);

    /* Busy can clear between down and up. The first press still owns the
     * click; the UI must not require a second tap after transport drains. */
    clear_intents();
    pointer(380, 366, true);
    s_state.congested = false;
    render();
    pointer(380, 366, false);
    assert(action_count(APP_CLICK) == 1 && last_action(APP_CLICK)->x == 2);

    /* Active cancellation controls remain operable even when normal input
     * is unavailable. Emitting an intent does not acknowledge its outcome. */
    s_state.congested = true;
    s_state.connected = false;
    s_state.ready = false;
    s_state.release_pending = true;
    s_state.transport_error = true;
    s_state.dragging = true;
    render();
    clear_intents();
    tap(80, 366);
    assert(action_count(APP_CLICK) == 0);
    tap(240, 366);
    assert(action_count(APP_DRAG) == 1 && last_action(APP_DRAG)->x == 0);
    assert(has_visible_label(lv_screen_active(), "Release"));
    assert(has_visible_label(lv_screen_active(), "Input connection error"));
    s_state.mode = INPUT_KEYBOARD;
    s_state.sending = true;
    render();
    clear_intents();
    tap(370, 366);
    assert(action_count(APP_STOP) == 1 && action_count(APP_SEND) == 0);
    assert(has_visible_label(lv_screen_active(), "Stop sending"));
    s_state = original;
    render();
    clear_intents();
    puts("PASS: busy controls preserve first touch; active Release/Stop remain available without claiming success");
}

static void interaction_checks(void)
{
    /* The LVGL pointer driver performs actual hit testing, including gaps and
     * modal ownership. No direct callback calls or mocked LVGL are involved. */
    s_state.mode = INPUT_MOUSE;
    s_state.overlay = APP_OVERLAY_NONE;
    render();
    clear_intents();
    tap(80, 450);
    assert(action_count(APP_MODE) == 1 && last_action(APP_MODE)->x == INPUT_MOUSE);
    assert(action_count(APP_TOUCH_BEGIN) == 1 && action_count(APP_TOUCH_END) == 1);
    assert(last_action(APP_MODE)->gesture == last_action(APP_TOUCH_BEGIN)->gesture);
    clear_intents();
    uint32_t object_count = lv_obj_get_child_count(lv_screen_active());
    pointer(80, 200, true);
    uint32_t gesture = last_action(APP_TOUCH_BEGIN)->gesture;
    bool original_theme = s_state.dark_theme;
    s_state.dark_theme = !original_theme;
    render();
    assert(lv_obj_get_child_count(lv_screen_active()) == object_count);
    pointer(84, 194, true);
    pointer(88, 196, true);
    pointer(88, 196, false);
    assert(action_count(APP_TOUCH_MOVE) == 2);
    const app_intent_t *move = last_action(APP_TOUCH_MOVE);
    assert(move->x == 4 && move->y == 2 && move->surface == APP_SURFACE_TOUCHPAD);
    assert(move->gesture == gesture);
    s_state.dark_theme = original_theme;
    render();
    assert(action_count(APP_CLICK) == 0);
    clear_intents();
    /* Blank spacing between pad and scroll must not route to either. */
    tap(404, 220);
    assert(action_count(APP_SCROLL) == 0 && action_count(APP_TOUCH_MOVE) == 0);
    clear_intents();
    tap(440, 190);
    assert(action_count(APP_SCROLL) == 1 && last_action(APP_SCROLL)->x == 1);
    clear_intents();
    tap(240, 366);
    assert(action_count(APP_DRAG) == 1 && last_action(APP_DRAG)->x == 1);
    assert(!s_state.dragging); /* Only a coordinator update changes the view. */
    s_state.dragging = true;
    render();
    clear_intents();
    tap(240, 366);
    assert(action_count(APP_DRAG) == 1 && last_action(APP_DRAG)->x == 0);
    s_state.dragging = false;
    clear_intents();
    pointer(80, 200, true);
    s_state.overlay = APP_OVERLAY_TARGET;
    ++s_state.ui_revision;
    render();
    pointer(86, 202, true);
    pointer(86, 202, false);
    assert(action_count(APP_TOUCH_MOVE) == 0);
    assert(action_count(APP_TOUCH_END) + action_count(APP_TOUCH_CANCEL) == 1);
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.mode = INPUT_KEYBOARD;
    render();
    clear_intents();
    tap(80, 206);
    assert(action_count(APP_GRID) == 1 && last_action(APP_GRID)->x == 0);
    assert(action_count(APP_MOVE) == 0 && action_count(APP_TEXT) == 0);
    assert(last_action(APP_GRID)->ui_revision == s_state.ui_revision);
    /* A render between pointer down and release must preserve the press-time
     * context, allowing the coordinator to reject the stale character. */
    clear_intents();
    pointer(230, 240, true);
    uint64_t old_revision = s_state.ui_revision;
    ++s_state.ui_revision;
    render();
    pointer(230, 240, false);
    assert(action_count(APP_GRID) == 1);
    assert(last_action(APP_GRID)->ui_revision == old_revision);
    s_state.keyboard_submode = APP_KEYBOARD_SHORTCUTS;
    render();
    clear_intents();
    tap(390, 310);
    assert(action_count(APP_SHORTCUT) == 1 && last_action(APP_SHORTCUT)->x == APP_SHORTCUT_ENTER);
    assert(action_count(APP_SEND) == 0);
    strcpy(s_state.preview, "Example");
    s_state.sending = true;
    render();
    clear_intents();
    tap(370, 366);
    assert(action_count(APP_STOP) == 1 && action_count(APP_SEND) == 0);
    s_state.sending = false;
    s_state.keyboard_submode = APP_KEYBOARD_SPOKEN;
    render();
    clear_intents();
    tap(300, 290);
    assert(action_count(APP_VOICE_START) == 0 && action_count(APP_VOICE_CONFIRM) == 0);
    s_state.overlay = APP_OVERLAY_MODES;
    render();
    clear_intents();
    tap(80, 450); /* Bottom nav is covered by the modal. */
    assert(action_count(APP_MODE) == 0);
    tap(240, 264);
    assert(action_count(APP_MODE) == 1 && last_action(APP_MODE)->x == INPUT_KEYBOARD);
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_DISPLAY;
    render();
    clear_intents();
    tap(350, 174);
    assert(action_count(APP_THEME) == 1 && last_action(APP_THEME)->x == 1);
    assert(s_state.dark_theme == original_theme);
    clear_intents();
    tap(100, 174);
    assert(action_count(APP_THEME) == 1 && last_action(APP_THEME)->x == 0);
    s_state.settings_page = APP_SETTINGS_INPUT;
    render();
    clear_intents();
    tap(420, 234);
    assert(action_count(APP_HAPTICS) == 1 && last_action(APP_HAPTICS)->x == 0);
    tap(350, 174);
    assert(action_count(APP_HOST_OS) == 1 && last_action(APP_HOST_OS)->x == APP_HOST_COMMAND);
    s_state.locked = true;
    render();
    clear_intents();
    tap(420, 234);
    tap(350, 174);
    tap(80, 450);
    assert(action_count(APP_MODE) == 0 && action_count(APP_HAPTICS) == 0 && action_count(APP_CLOSE) == 0);
    s_state.locked = false;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.mode = INPUT_AUDIO;
    render();
    clear_intents();
    tap(140, 280);
    tap(350, 280);
    assert(action_count(APP_AUDIO_POWER) == 0 && action_count(APP_AUDIO_MUTE) == 0);
    clear_intents();
    s_state.mode = INPUT_MOUSE;
    s_state.connected = false;
    s_state.ready = false;
    render();
    tap(80, 366);
    assert(action_count(APP_CLICK) == 0);
    puts("PASS: real LVGL pointer boundaries, relative pad deltas, modal/lock focus, stale press revision, in-place theme changes, explicit Enter/Stop and unavailable capability gates");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_output = argv[1];
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    app_ui_create(display, capture);
    for (unsigned theme = 0; theme < 2; ++theme) {
        s_prefix = theme ? "dark-" : "";
        s_state = (app_view_t){.epoch = 11, .ui_revision = 1, .mode = INPUT_MOUSE,
            .dark_theme = theme != 0,
            .connected = true, .ready = true, .motion_available = true,
            .bluetooth_available = true, .bluetooth_connected = true, .bluetooth_ready = true,
            .bluetooth_target_name = "HID test peer",
            .haptics_available = true, .haptics_enabled = true, .gain_divisor = 1,
            .microphone_muted = true, .volume = 0, .target_name = "HID test peer",
            .battery_supported = true, .battery_percent_valid = true, .battery_percent = 73,
            .battery_state = APP_BATTERY_DISCHARGING,
            .pairing_state = APP_PAIRING_CLOSED, .paired_host_count = 6, .paired_host_capacity = 15,
            .bluetooth_hosts_available = true, .bluetooth_host_count = 6,
            .wifi_state = APP_UI_WIFI_CONNECTED, .wifi_rssi_valid = true, .wifi_rssi_dbm = -50,
            .wifi_saved_network = true, .wifi_ssid = "Test network", .wifi_ipv4 = "192.0.2.15",
            .brightness_available = true, .brightness_percent = 70, .screen_off_available = true,
            .idle_timeout_seconds = 300, .settings_save_state = APP_SAVE_SAVED,
            .firmware_version = "test-build", .build_id = "fixture-only", .board_revision = "Test board",
            .device_id = "test-device", .uptime_seconds = 3782};
        for (unsigned i = 0; i < 6; ++i) {
            snprintf(s_state.bluetooth_hosts[i].identity, sizeof(s_state.bluetooth_hosts[i].identity), "AA:BB:CC:DD:EE:%02X", i);
            s_state.bluetooth_hosts[i].connected = i == 0;
        }
        strcpy(s_state.bluetooth_hosts[1].name, "Named test host");
        grid_groups();
        screenshot("mouse");
        s_state.congested = true;
        s_state.motion_enabled = true;
        screenshot("mouse-busy");
        s_state.congested = false;
        s_state.motion_enabled = false;
        s_state.gain_divisor = 8;
        s_state.dragging = true;
        screenshot("mouse-drag");
        s_state.dragging = false;
        s_state.mode = INPUT_KEYBOARD;
        screenshot("keyboard-motion");
        s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
        s_state.keyboard_group = 0;
        strcpy(s_state.preview, "Hello");
        s_state.keyboard_cell_count = 4;
        memset(s_state.keyboard_cells, 0, sizeof(s_state.keyboard_cells));
        for (int i = 0; i < 4; ++i) snprintf(s_state.keyboard_cells[i], sizeof(s_state.keyboard_cells[i]), "%c", 'A' + i);
        screenshot("keyboard-touch");
        s_state.sending = true;
        s_state.accepted_prefix = 2;
        s_state.send_total = 7;
        screenshot("keyboard-sending");
        s_state.sending = false;
        s_state.accepted_prefix = 0;
        s_state.send_total = 0;
        s_state.keyboard_submode = APP_KEYBOARD_SPOKEN;
        screenshot("keyboard-spoken");
        s_state.keyboard_submode = APP_KEYBOARD_SHORTCUTS;
        s_state.host_os = APP_HOST_COMMAND;
        screenshot("keyboard-shortcuts");
        s_state.mode = INPUT_AUDIO;
        screenshot("audio-voice");
        s_state.audio_kind = APP_AUDIO_SPEAKER;
        screenshot("audio-speaker");
        s_state.mode = INPUT_MOUSE;
        s_state.overlay = APP_OVERLAY_MODES;
        screenshot("mode-chooser");
        s_state.overlay = APP_OVERLAY_TARGET;
        screenshot("target");
        s_state.overlay = APP_OVERLAY_SETTINGS;
        s_state.settings_page = APP_SETTINGS_HOME;
        screenshot("settings");
        s_state.settings_page = APP_SETTINGS_INPUT;
        s_state.haptics_enabled = false;
        s_state.motion_available = false;
        s_state.calibration_available = true;
        screenshot("settings-calibration-retry");
        s_state.calibrating = true;
        screenshot("settings-calibrating");
        s_state.haptics_enabled = true;
        s_state.motion_available = true;
        s_state.calibrating = false;
        s_state.locked = true;
        screenshot("locked");
        s_state.release_pending = true;
        screenshot("locked-release-pending");
        s_state.release_pending = false;
        s_state.locked = false;
        s_state.overlay = APP_OVERLAY_NONE;
        s_state.connected = false;
        s_state.ready = false;
        s_state.target_name = NULL;
        s_state.bluetooth_connected = false;
        screenshot("mouse-disconnected");
        s_state.bluetooth_connected = true;
        s_state.connected = true;
        s_state.ready = true;
        s_state.target_name = "HID test peer";
        s_state.keyboard_submode = APP_KEYBOARD_MOTION;
        s_state.host_os = APP_HOST_CTRL;
        grid_groups();
        geometry_checks();
        device_settings_checks();
        pairing_capacity_checks();
        transport_and_grid_checks();
        text_boundary_checks();
        congestion_checks();
        interaction_checks();
    }
    lv_deinit();
    return 0;
}
