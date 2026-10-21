// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include <stdio.h>
#include <string.h>

/* Logical geometry and colors follow design/index.html and styles.css. */
#define INK 0x182a25
#define MUTED 0x5c6c65
#define LINE 0xdae1db
#define SOFT 0xedf1eb
#define ACCENT 0x225b44
#define FOCUS 0xc5e5b5
#define WARNING 0x885018
#define SURFACE 0xffffff
#define ON_ACCENT 0xfffefe
#define MAX_BINDINGS 112
#define MAX_COLOR_BINDINGS 896

#if LV_FONT_MONTSERRAT_16
#define BODY_FONT (&lv_font_montserrat_16)
#else
#define BODY_FONT LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_14
#define META_FONT (&lv_font_montserrat_14)
#else
#define META_FONT BODY_FONT
#endif
#if LV_FONT_MONTSERRAT_22
#define TITLE_FONT (&lv_font_montserrat_22)
#else
#define TITLE_FONT BODY_FONT
#endif

typedef struct {
    app_action_t action;
    int value;
    lv_obj_t *object;
} binding_t;

static app_submit_fn s_submit;
static app_view_t s_view;
static char s_message_text[160];
static char s_target_text[96];
static binding_t s_bindings[MAX_BINDINGS];
static size_t s_binding_count;
static lv_obj_t *s_screen, *s_panels[3], *s_modes[3], *s_target, *s_status;
static lv_obj_t *s_motion, *s_gains[3], *s_pad, *s_drag, *s_mouse_controls[4];
static lv_obj_t *s_submodes[4], *s_preview, *s_progress, *s_clear;
static lv_obj_t *s_grid_page, *s_grid[APP_UI_GRID_CELLS], *s_back, *s_case, *s_symbols;
static lv_obj_t *s_voice_page, *s_voice_candidate, *s_voice_hint, *s_voice_confirm;
static lv_obj_t *s_shortcut_page, *s_shortcuts[6], *s_delete, *s_space, *s_send;
static lv_obj_t *s_audio_kinds[2], *s_audio_title, *s_audio_profile, *s_audio_power;
static lv_obj_t *s_audio_mute, *s_volume, *s_volume_text, *s_audio_note, *s_audio_capability;
static lv_obj_t *s_overlay, *s_overlay_pages[6], *s_overlay_target;
static lv_obj_t *s_target_state, *s_telemetry[2], *s_overlay_state;
static lv_obj_t *s_signal_icons[2][2], *s_signal_unknown[2][2], *s_signal_bars[2][2][4];
static lv_obj_t *s_settings_pages[7], *s_settings_title, *s_settings_back, *s_save_status;
static lv_obj_t *s_transports[2], *s_transport_status, *s_wifi_summary, *s_wifi_ssid, *s_wifi_note;
static lv_obj_t *s_bluetooth_state, *s_bluetooth_pairing, *s_pairing_buttons[3];
static lv_obj_t *s_bond_names[4], *s_bond_ids[4], *s_bond_states[4], *s_bond_rows[4];
static lv_obj_t *s_bond_empty, *s_bond_page, *s_bond_previous, *s_bond_next, *s_bond_add, *s_bond_disconnect;
static lv_obj_t *s_wifi_state, *s_wifi_detail, *s_wifi_setup, *s_wifi_forget;
static lv_obj_t *s_brightness, *s_brightness_text, *s_timeouts[5];
static lv_obj_t *s_about_identity[4], *s_about_battery, *s_about_uptime;
static lv_obj_t *s_setup_state, *s_setup_ssid, *s_setup_passcode, *s_setup_url, *s_setup_stop;
static lv_obj_t *s_confirm_title, *s_confirm_detail, *s_confirm_accept, *s_sleep_shield;
static bool s_wake_touch;
static lv_obj_t *s_target_detail, *s_target_link, *s_target_heading, *s_target_note, *s_lock_detail;
static lv_obj_t *s_hosts[2], *s_themes[2], *s_haptics, *s_haptics_text, *s_calibrate, *s_mode_choices[3];
static uint32_t s_gesture;
static uint64_t s_touch_epoch, s_touch_revision;
static bool s_touch_active;
static app_touch_surface_t s_touch_surface;
static lv_point_t s_last_point;

typedef enum { STYLE_BACKGROUND, STYLE_TEXT, STYLE_BORDER } color_property_t;
typedef struct {
    lv_obj_t *object;
    uint32_t token;
    lv_style_selector_t selector;
    color_property_t property;
} color_binding_t;

static color_binding_t s_colors[MAX_COLOR_BINDINGS];
static size_t s_color_count;
static bool s_dark_theme;

static lv_color_t themed_color(uint32_t token)
{
    if (token == ON_ACCENT) return lv_color_hex(s_dark_theme ? 0x082b40 : SURFACE);
    if (!s_dark_theme) return lv_color_hex(token);
    switch (token) {
    case INK: token = 0xecf4fb; break;
    case MUTED: token = 0xb6c4d2; break;
    case LINE: token = 0x394958; break;
    case SOFT: token = 0x111a24; break;
    case ACCENT: token = 0x8dd8ff; break;
    case FOCUS: token = 0x244b63; break;
    case WARNING: token = 0xe7bd7c; break;
    case SURFACE: token = 0x080b10; break;
    case 0x8b9891: token = 0x758594; break;
    case 0x9ca9a1: token = 0x5c6c7c; break;
    default: break;
    }
    return lv_color_hex(token);
}

static void apply_color(const color_binding_t *binding)
{
    lv_color_t color = themed_color(binding->token);
    switch (binding->property) {
    case STYLE_BACKGROUND: lv_obj_set_style_bg_color(binding->object, color, binding->selector); break;
    case STYLE_TEXT: lv_obj_set_style_text_color(binding->object, color, binding->selector); break;
    case STYLE_BORDER: lv_obj_set_style_border_color(binding->object, color, binding->selector); break;
    }
}

static void bind_color(lv_obj_t *object, uint32_t token,
                       lv_style_selector_t selector, color_property_t property)
{
    size_t index = 0;
    for (; index < s_color_count; ++index) {
        if (s_colors[index].object == object && s_colors[index].selector == selector &&
            s_colors[index].property == property) break;
    }
    LV_ASSERT(index < MAX_COLOR_BINDINGS);
    if (index == s_color_count) ++s_color_count;
    s_colors[index] = (color_binding_t){object, token, selector, property};
    apply_color(&s_colors[index]);
}

static void background(lv_obj_t *object, uint32_t token, lv_style_selector_t selector)
{
    bind_color(object, token, selector, STYLE_BACKGROUND);
}

static void foreground(lv_obj_t *object, uint32_t token, lv_style_selector_t selector)
{
    bind_color(object, token, selector, STYLE_TEXT);
}

static void border(lv_obj_t *object, uint32_t token, lv_style_selector_t selector)
{
    bind_color(object, token, selector, STYLE_BORDER);
}

static void apply_theme(bool dark)
{
    if (s_dark_theme == dark) return;
    s_dark_theme = dark;
    for (size_t i = 0; i < s_color_count; ++i) apply_color(&s_colors[i]);
}

static size_t bounded_length(const char *value, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && value[length]) ++length;
    return length;
}

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void enabled(lv_obj_t *object, bool allow)
{
    if (allow) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void label_text(lv_obj_t *object, const char *value)
{
    /* Snapshots arrive repeatedly; keep stable labels allocation-free. */
    if (strcmp(lv_label_get_text(object), value) != 0) lv_label_set_text(object, value);
}

static void text(lv_obj_t *object, const char *value)
{
    label_text(lv_obj_get_child(object, 0), value);
}

static void selected(lv_obj_t *object, bool active)
{
    background(object, active ? ACCENT : SOFT, 0);
    foreground(object, active ? ON_ACCENT : INK, 0);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    background(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int width, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    foreground(object, color, 0);
    label_text(object, value);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *heading(lv_obj_t *parent, const char *value, int x, int y, int width)
{
    lv_obj_t *object = label(parent, value, x, y, width, INK);
    lv_obj_set_style_text_font(object, TITLE_FONT, 0);
    return object;
}

static void single_line(lv_obj_t *object)
{
    /* DOT also needs a height limit: width alone still permits wrapping. */
    const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
    lv_obj_set_height(object, lv_font_get_line_height(font));
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
}

static bool submit(app_action_t action, int x, int y)
{
    app_intent_t intent = {
        .action = action, .epoch = s_gesture ? s_touch_epoch : s_view.epoch,
        .ui_revision = s_gesture ? s_touch_revision : s_view.ui_revision, .x = x, .y = y,
        .gesture = s_gesture, .surface = s_touch_surface
    };
    bool accepted = s_submit && s_submit(&intent);
    if (!accepted && s_status) {
        label_text(s_status, "Input busy. Please try again.");
    }
    return accepted;
}

static bool in_overlay(lv_obj_t *object)
{
    while (object) {
        if (object == s_overlay) return true;
        object = lv_obj_get_parent(object);
    }
    return false;
}

static void on_action(lv_event_t *event)
{
    binding_t *binding = lv_event_get_user_data(event);
    if (s_view.locked || s_view.screen_off || s_wake_touch || (s_view.overlay != APP_OVERLAY_NONE && !in_overlay(binding->object))) return;
    int value = binding->value;
    switch (binding->action) {
    case APP_MOTION: value = !s_view.motion_enabled; break;
    case APP_DRAG: value = !s_view.dragging; break;
    case APP_CASE: value = !s_view.lowercase; break;
    case APP_SYMBOLS: value = !s_view.symbols; break;
    case APP_HAPTICS: value = !s_view.haptics_enabled; break;
    case APP_AUDIO_POWER: value = !s_view.audio_active; break;
    case APP_AUDIO_MUTE: value = !s_view.microphone_muted; break;
    default: break;
    }
    app_action_t action = binding->action;
    if (action == APP_SETTINGS_PAGE && value == APP_SETTINGS_HOME && s_view.overlay == APP_OVERLAY_SETTINGS) {
        if (s_view.settings_page == APP_SETTINGS_HOME) action = APP_CLOSE;
        else if (s_view.settings_page == APP_SETTINGS_BLUETOOTH || s_view.settings_page == APP_SETTINGS_WIFI) value = APP_SETTINGS_CONNECTIVITY;
    }
    if (action == APP_BLUETOOTH_PAGE) value += (int)s_view.bluetooth_page;
    if (action == APP_SEND && s_view.sending) action = APP_STOP;
    if (action == APP_VOICE_START && s_view.voice_candidate) action = APP_VOICE_CONFIRM;
    (void)submit(action, value, 0);
}

static lv_obj_t *button(lv_obj_t *parent, const char *value, int x, int y,
                        int width, int height, app_action_t action, int argument)
{
    LV_ASSERT(s_binding_count < MAX_BINDINGS);
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_text_font(object, BODY_FONT, 0);
    background(object, FOCUS, LV_STATE_PRESSED);
    foreground(object, INK, LV_STATE_PRESSED);
    background(object, SOFT, LV_STATE_DISABLED);
    foreground(object, 0x8b9891, LV_STATE_DISABLED);
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    selected(object, false);
    lv_obj_t *caption = lv_label_create(object);
    lv_obj_set_width(caption, width - 8);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    label_text(caption, value);
    single_line(caption);
    lv_obj_center(caption);
    binding_t *binding = &s_bindings[s_binding_count++];
    *binding = (binding_t){.action = action, .value = argument, .object = object};
    lv_obj_add_event_cb(object, on_action, LV_EVENT_CLICKED, binding);
    return object;
}

static void on_touch(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_indev_t *device = lv_event_get_indev(event);
    if (!device || lv_indev_get_type(device) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(device, &point);
    lv_obj_t *target = lv_event_get_target(event);
    if (code == LV_EVENT_PRESSED) {
        ++s_gesture;
        if (!s_gesture) ++s_gesture;
        s_touch_epoch = s_view.epoch;
        s_touch_revision = s_view.ui_revision;
        s_touch_active = true;
        s_wake_touch = s_view.screen_off;
        s_touch_surface = target == s_pad ? APP_SURFACE_TOUCHPAD : APP_SURFACE_CONTROL;
        s_last_point = point;
        (void)submit(APP_TOUCH_BEGIN, point.x, point.y);
    } else if (code == LV_EVENT_PRESSING && s_touch_active &&
               s_touch_surface == APP_SURFACE_TOUCHPAD && !s_view.locked &&
               !s_view.screen_off && !s_wake_touch &&
               s_view.overlay == APP_OVERLAY_NONE && s_view.mode == INPUT_MOUSE &&
               s_touch_revision == s_view.ui_revision) {
        int dx = point.x - s_last_point.x;
        int dy = point.y - s_last_point.y;
        s_last_point = point;
        if (dx || dy) (void)submit(APP_TOUCH_MOVE, dx, dy);
    } else if (s_touch_active && (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {
        (void)submit(code == LV_EVENT_RELEASED ? APP_TOUCH_END : APP_TOUCH_CANCEL, point.x, point.y);
        s_touch_active = false;
    }
}

static void on_haptics(lv_event_t *event)
{
    (void)event;
    if (!s_view.locked && !s_view.screen_off && !s_wake_touch &&
        s_view.overlay == APP_OVERLAY_SETTINGS && s_view.settings_page == APP_SETTINGS_INPUT &&
        s_view.haptics_available) {
        (void)submit(APP_HAPTICS, !s_view.haptics_enabled, 0);
    }
}

static void on_volume(lv_event_t *event)
{
    if (!s_view.locked && !s_view.screen_off && !s_wake_touch &&
        s_view.overlay == APP_OVERLAY_NONE && s_view.audio_available) {
        (void)submit(APP_AUDIO_VOLUME, lv_slider_get_value(lv_event_get_target(event)), 0);
    }
}

static lv_obj_t *metadata(lv_obj_t *parent, const char *value, int x, int y, int width)
{
    lv_obj_t *object = label(parent, value, x, y, width, MUTED);
    lv_obj_set_style_text_font(object, META_FONT, 0);
    single_line(object);
    return object;
}

static lv_obj_t *slider(lv_obj_t *parent, int x, int y, int width,
                        int minimum, int maximum, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_slider_create(parent);
    lv_obj_remove_style_all(object);
    const lv_style_selector_t parts[] = {LV_PART_MAIN, LV_PART_INDICATOR, LV_PART_KNOB};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, parts[i]);
        lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, parts[i]);
    }
    lv_obj_set_style_pad_all(object, 8, LV_PART_KNOB);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 12);
    lv_slider_set_range(object, minimum, maximum);
    background(object, LINE, LV_PART_MAIN);
    background(object, ACCENT, LV_PART_INDICATOR);
    background(object, ACCENT, LV_PART_KNOB);
    background(object, 0x9ca9a1, LV_PART_KNOB | LV_STATE_DISABLED);
    background(object, LINE, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(object, 16);
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(object, callback, LV_EVENT_RELEASED, NULL);
    return object;
}

static void on_brightness(lv_event_t *event)
{
    if (!s_view.locked && !s_view.screen_off && !s_wake_touch &&
        s_view.overlay == APP_OVERLAY_SETTINGS && s_view.settings_page == APP_SETTINGS_DISPLAY &&
        s_view.brightness_available) {
        (void)submit(APP_BRIGHTNESS, lv_slider_get_value(lv_event_get_target(event)) * 10, 0);
    }
}

static void create_mouse(lv_obj_t *panel)
{
    heading(panel, "Point naturally", 0, 0, 296);
    metadata(panel, "Wrist motion / precise touch", 0, 28, 296);
    s_motion = button(panel, "Motion / off", 304, 0, 144, 44, APP_MOTION, 0);
    label(panel, "Touch gain", 0, 65, 184, MUTED);
    const char *gains[] = {"1x", "1/4x", "1/8x"};
    const int divisors[] = {1, 4, 8};
    for (unsigned i = 0; i < 3; ++i) {
        s_gains[i] = button(panel, gains[i], 216 + (int)i * 80, 52, 72, 44, APP_GAIN, divisors[i]);
    }
    s_pad = box(panel, 0, 108, 384, 156, SOFT);
    lv_obj_set_style_radius(s_pad, 16, 0);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *title = label(s_pad, "Touchpad", 0, 52, 384, INK);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint = metadata(s_pad, "Touch pauses motion", 0, 80, 384);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    s_mouse_controls[0] = button(panel, LV_SYMBOL_UP, 392, 108, 56, 64, APP_SCROLL, 1);
    lv_obj_t *scroll = metadata(panel, "Scroll", 392, 178, 56);
    lv_obj_set_style_text_align(scroll, LV_TEXT_ALIGN_CENTER, 0);
    s_mouse_controls[1] = button(panel, LV_SYMBOL_DOWN, 392, 200, 56, 64, APP_SCROLL, -1);
    s_mouse_controls[2] = button(panel, "Left click", 0, 272, 144, 48, APP_CLICK, 1);
    s_drag = button(panel, "Drag", 152, 272, 144, 48, APP_DRAG, 0);
    s_mouse_controls[3] = button(panel, "Right click", 304, 272, 144, 48, APP_CLICK, 2);
}

static void create_keyboard(lv_obj_t *panel)
{
    const char *names[] = {"Motion", "Touch", "Spoken", "Shortcuts"};
    for (int i = 0; i < 4; ++i) {
        s_submodes[i] = button(panel, names[i], i * 114, 0, 106, 44, APP_KEYBOARD_SUBMODE, i);
    }
    lv_obj_t *preview = box(panel, 0, 52, 448, 52, SOFT);
    lv_obj_set_style_radius(preview, 12, 0);
    s_preview = label(preview, "Ready for input...", 12, 5, 340, INK);
    single_line(s_preview);
    s_progress = metadata(preview, "0 / 64", 12, 30, 340);
    s_clear = button(preview, "Clear", 368, 4, 72, 44, APP_CLEAR, 0);
    s_grid_page = box(panel, 0, 112, 448, 152, SURFACE);
    s_back = button(s_grid_page, "Groups", 360, 0, 88, 44, APP_BACK, 0);
    s_case = button(s_grid_page, "ABC", 360, 52, 88, 44, APP_CASE, 0);
    s_symbols = button(s_grid_page, "Symbols", 360, 104, 88, 44, APP_SYMBOLS, 0);
    for (int i = 0; i < APP_UI_GRID_CELLS; ++i) {
        s_grid[i] = button(s_grid_page, "", (i % 3) * 120, (i / 3) * 52,
                            112, 44, APP_GRID, i);
    }
    s_voice_page = box(panel, 0, 112, 448, 152, SOFT);
    lv_obj_set_style_radius(s_voice_page, 12, 0);
    heading(s_voice_page, "Spoken letters", 16, 12, 416);
    s_voice_candidate = heading(s_voice_page, "A-Z", 16, 66, 82);
    s_voice_hint = label(s_voice_page, "Unavailable", 112, 48, 320, MUTED);
    s_voice_confirm = button(s_voice_page, "Start listening", 112, 100, 320, 44, APP_VOICE_START, 0);
    s_shortcut_page = box(panel, 0, 112, 448, 152, SURFACE);
    const char *shortcuts[] = {"Copy", "Paste", "Undo", "Tab", "Esc", "Enter"};
    for (int i = 0; i < 6; ++i) {
        s_shortcuts[i] = button(s_shortcut_page, shortcuts[i], (i % 3) * 152,
                                (i / 3) * 80, 144, 72, APP_SHORTCUT, i);
    }
    s_delete = button(panel, "Delete", 0, 272, 144, 48, APP_DELETE, 0);
    s_space = button(panel, "Space", 152, 272, 144, 48, APP_SPACE, 0);
    s_send = button(panel, "Send", 304, 272, 144, 48, APP_SEND, 0);
}

static void create_audio(lv_obj_t *panel)
{
    heading(panel, "Audio", 0, 0, 288);
    s_audio_capability = label(panel, "Unavailable", 320, 4, 128, WARNING);
    s_audio_kinds[0] = button(panel, "Voice / meeting", 0, 36, 220, 44, APP_AUDIO_KIND, APP_AUDIO_VOICE);
    s_audio_kinds[1] = button(panel, "Speaker", 228, 36, 220, 44, APP_AUDIO_KIND, APP_AUDIO_SPEAKER);
    lv_obj_t *hero = box(panel, 0, 88, 448, 160, SOFT);
    lv_obj_set_style_radius(hero, 16, 0);
    s_audio_title = heading(hero, "Microphone muted", 12, 16, 424);
    lv_obj_set_style_text_align(s_audio_title, LV_TEXT_ALIGN_CENTER, 0);
    s_audio_profile = label(hero, "Voice and meetings", 12, 52, 424, MUTED);
    lv_obj_set_style_text_align(s_audio_profile, LV_TEXT_ALIGN_CENTER, 0);
    s_audio_power = button(hero, "Start audio", 16, 100, 204, 44, APP_AUDIO_POWER, 0);
    s_audio_mute = button(hero, "Muted", 228, 100, 204, 44, APP_AUDIO_MUTE, 0);
    label(panel, "Volume", 0, 266, 76, INK);
    s_volume = slider(panel, 96, 272, 264, 0, 100, on_volume);
    s_volume_text = label(panel, "--", 392, 266, 56, MUTED);
    s_audio_note = metadata(panel, "Audio unavailable", 0, 303, 448);
}

static lv_obj_t *card(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *object = box(parent, 0, y, 448, height, SOFT);
    lv_obj_set_style_radius(object, 12, 0);
    return object;
}

static void create_settings(lv_obj_t *settings)
{
    s_settings_back = button(settings, LV_SYMBOL_LEFT, 0, 0, 44, 44, APP_SETTINGS_PAGE, APP_SETTINGS_HOME);
    s_settings_title = heading(settings, "Settings", 60, 8, 388);
    for (unsigned i = 0; i < 7; ++i) s_settings_pages[i] = box(settings, 0, 56, 448, 256, SURFACE);
    const char *names[] = {"Connectivity", "Display", "Input", "About"};
    for (int i = 0; i < 4; ++i) {
        button(s_settings_pages[0], names[i], (i % 2) * 228, (i / 2) * 132,
               220, 124, APP_SETTINGS_PAGE, i + 1);
    }
    s_save_status = metadata(settings, "Settings status unavailable", 0, 328, 448);
    button(settings, "Done", 0, 352, 448, 48, APP_CLOSE, 0);

    lv_obj_t *connectivity = s_settings_pages[APP_SETTINGS_CONNECTIVITY];
    lv_obj_t *transport = card(connectivity, 0, 96);
    metadata(transport, "Input transport / all modes", 12, 4, 424);
    s_transports[0] = button(transport, "Bluetooth", 8, 24, 212, 44, APP_TRANSPORT_SELECT, APP_INPUT_BLUETOOTH);
    s_transports[1] = button(transport, "USB", 228, 24, 212, 44, APP_TRANSPORT_SELECT, APP_INPUT_USB);
    s_transport_status = metadata(transport, "Bluetooth selected", 12, 76, 424);
    lv_obj_t *ble = button(connectivity, "Bluetooth hosts", 0, 104, 448, 72, APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    lv_obj_t *caption = lv_obj_get_child(ble, 0);
    lv_obj_set_align(caption, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(caption, 12, 8);
    lv_obj_set_width(caption, 424);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
    s_bluetooth_state = metadata(ble, "Unavailable", 12, 30, 424);
    s_bluetooth_pairing = metadata(ble, "Pairing unavailable", 12, 50, 424);
    lv_obj_t *wifi_nav = button(connectivity, "Wi-Fi settings", 0, 184, 448, 72, APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    caption = lv_obj_get_child(wifi_nav, 0);
    lv_obj_set_align(caption, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(caption, 12, 12);
    lv_obj_set_width(caption, 424);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
    s_wifi_summary = metadata(wifi_nav, "Unavailable", 12, 42, 424);
    lv_obj_t *wifi = s_settings_pages[APP_SETTINGS_WIFI];
    s_wifi_state = label(wifi, "Unavailable", 0, 0, 448, INK);
    single_line(s_wifi_state);
    s_wifi_ssid = label(wifi, "No network", 0, 36, 448, INK);
    lv_obj_set_height(s_wifi_ssid, 42);
    lv_label_set_long_mode(s_wifi_ssid, LV_LABEL_LONG_DOT);
    s_wifi_detail = metadata(wifi, "No saved network", 0, 96, 448);
    s_wifi_note = metadata(wifi, "", 0, 124, 448);
    s_wifi_setup = button(wifi, "Set up Wi-Fi", 0, 160, 448, 44, APP_WIFI_SETUP, 1);
    s_wifi_forget = button(wifi, "Forget network", 0, 212, 448, 44, APP_WIFI_FORGET_REQUEST, 0);

    lv_obj_t *bluetooth = s_settings_pages[APP_SETTINGS_BLUETOOTH];
    for (unsigned i = 0; i < APP_UI_BONDS_PER_PAGE; ++i) {
        lv_obj_t *row = box(bluetooth, 0, (int)i * 36, 448, 36, SURFACE);
        s_bond_rows[i] = row;
        s_bond_names[i] = label(row, "Host", 0, 0, 304, INK);
        single_line(s_bond_names[i]);
        s_bond_ids[i] = metadata(row, "Identity unavailable", 0, 20, 304);
        s_bond_states[i] = label(row, "Saved", 320, 8, 128, MUTED);
        single_line(s_bond_states[i]);
        lv_obj_set_style_text_align(s_bond_states[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    s_bond_empty = label(bluetooth, "Saved hosts unavailable", 0, 40, 448, MUTED);
    s_bond_previous = button(bluetooth, "Previous", 0, 152, 144, 44, APP_BLUETOOTH_PAGE, -1);
    s_bond_page = label(bluetooth, "1 / 1", 152, 156, 144, MUTED);
    lv_obj_set_style_text_font(s_bond_page, META_FONT, 0);
    lv_obj_set_height(s_bond_page, 36);
    lv_label_set_long_mode(s_bond_page, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_bond_page, LV_TEXT_ALIGN_CENTER, 0);
    s_bond_next = button(bluetooth, "Next", 304, 152, 144, 44, APP_BLUETOOTH_PAGE, 1);
    s_bond_add = button(bluetooth, "Add host", 0, 204, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_OPEN);
    s_pairing_buttons[0] = s_bond_add;
    s_pairing_buttons[1] = button(bluetooth, "Close pairing", 0, 204, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_CLOSE);
    s_bond_disconnect = button(bluetooth, "Disconnect", 152, 204, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    s_pairing_buttons[2] = button(bluetooth, "Forget all", 304, 204, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL);

    lv_obj_t *display = s_settings_pages[APP_SETTINGS_DISPLAY];
    label(display, "Appearance", 0, 0, 448, INK);
    s_themes[0] = button(display, "Light", 0, 24, 220, 44, APP_THEME, 0);
    s_themes[1] = button(display, "Dark", 228, 24, 220, 44, APP_THEME, 1);
    label(display, "Brightness", 0, 80, 280, INK);
    s_brightness_text = label(display, "Unavailable", 312, 80, 136, MUTED);
    lv_obj_set_style_text_align(s_brightness_text, LV_TEXT_ALIGN_RIGHT, 0);
    s_brightness = slider(display, 16, 120, 416, 1, 10, on_brightness);
    label(display, "Screen timeout", 0, 156, 448, INK);
    const char *timeouts[] = {"Off", "1 min", "5 min", "10 min", "30 min"};
    const int seconds[] = {0, 60, 300, 600, 1800};
    for (int i = 0; i < 5; ++i) {
        s_timeouts[i] = button(display, timeouts[i], i * 91, 184, 83, 44, APP_IDLE_TIMEOUT, seconds[i]);
    }
    metadata(display, "Display only; Bluetooth and Wi-Fi stay active.", 0, 238, 448);

    lv_obj_t *input = s_settings_pages[APP_SETTINGS_INPUT];
    label(input, "Host shortcut modifier", 0, 0, 448, INK);
    s_hosts[0] = button(input, "Ctrl / Windows", 0, 24, 220, 44, APP_HOST_OS, APP_HOST_CTRL);
    s_hosts[1] = button(input, "Command / Apple", 228, 24, 220, 44, APP_HOST_OS, APP_HOST_COMMAND);
    label(input, "Haptic feedback", 0, 96, 228, INK);
    s_haptics_text = label(input, "Off", 236, 96, 112, MUTED);
    single_line(s_haptics_text);
    s_haptics = lv_switch_create(input);
    lv_obj_remove_style_all(s_haptics);
    const lv_style_selector_t parts[] = {LV_PART_MAIN, LV_PART_INDICATOR, LV_PART_KNOB};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_opa(s_haptics, LV_OPA_COVER, parts[i]);
        lv_obj_set_style_radius(s_haptics, LV_RADIUS_CIRCLE, parts[i]);
    }
    lv_obj_set_style_pad_all(s_haptics, -4, LV_PART_KNOB);
    lv_obj_set_pos(s_haptics, 360, 84);
    lv_obj_set_size(s_haptics, 88, 44);
    background(s_haptics, LINE, LV_PART_MAIN);
    background(s_haptics, LINE, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_haptics, 0, 0);
    background(s_haptics, ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    background(s_haptics, ON_ACCENT, LV_PART_KNOB);
    lv_obj_add_flag(s_haptics, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_haptics, on_haptics, LV_EVENT_VALUE_CHANGED, NULL);
    s_calibrate = button(input, "Calibrate motion", 0, 144, 448, 44, APP_CALIBRATE, 0);
    label(input, "Use a US ANSI English host layout.\nHold AI to lock; double-click AI for modes.", 0, 208, 448, MUTED);

    lv_obj_t *about = s_settings_pages[APP_SETTINGS_ABOUT];
    const char *identity_names[] = {"Firmware", "Build", "Board", "Device"};
    for (unsigned i = 0; i < 4; ++i) {
        metadata(about, identity_names[i], 0, (int)i * 28 + 2, 88);
        s_about_identity[i] = label(about, "Unavailable", 96, (int)i * 28, 352, INK);
        single_line(s_about_identity[i]);
    }
    s_about_battery = label(about, "Battery unavailable", 0, 132, 448, MUTED);
    lv_obj_set_height(s_about_battery, 72);
    s_about_uptime = metadata(about, "Uptime unavailable", 0, 224, 448);
}

static void create_signals(lv_obj_t *parent, unsigned header)
{
    const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_BLUETOOTH};
    for (unsigned radio = 0; radio < 2; ++radio) {
        int x = (int)radio * 64;
        s_signal_icons[header][radio] = metadata(parent, icons[radio], x + 4, 27, 20);
        s_signal_unknown[header][radio] = metadata(parent, LV_SYMBOL_CLOSE, x + 28, 27, 28);
        for (unsigned bar = 0; bar < 4; ++bar) {
            int height = 3 + (int)bar * 3;
            lv_obj_t *object = box(parent, x + 28 + (int)bar * 6, 42 - height, 4, height, ACCENT);
            lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
            s_signal_bars[header][radio][bar] = object;
        }
    }
}

static void create_overlays(void)
{
    s_overlay = box(s_screen, 0, 0, 480, 480, SURFACE);
    s_overlay_target = label(s_overlay, "No target", 24, 12, 244, INK);
    single_line(s_overlay_target);
    s_overlay_state = metadata(s_overlay, "Pair device", 24, 36, 244);
    lv_obj_t *telemetry = box(s_overlay, 284, 8, 128, 48, SURFACE);
    s_telemetry[1] = metadata(telemetry, "Battery --", 0, 4, 128);
    lv_obj_set_style_text_align(s_telemetry[1], LV_TEXT_ALIGN_CENTER, 0);
    create_signals(telemetry, 1);
    for (int i = 0; i < 6; ++i) s_overlay_pages[i] = box(s_overlay, 16, 72, 448, 400, SURFACE);
    lv_obj_t *modes = s_overlay_pages[0];
    heading(modes, "Choose a mode", 0, 0, 448);
    label(modes, "Double-click AI to open this menu.", 0, 36, 448, MUTED);
    const char *names[] = {"Mouse", "Keyboard", "Audio"};
    for (int i = 0; i < 3; ++i) s_mode_choices[i] = button(modes, names[i], 0, 88 + i * 76, 448, 64, APP_MODE, i);
    button(modes, "Back", 0, 352, 448, 48, APP_CLOSE, 0);
    lv_obj_t *target = s_overlay_pages[1];
    s_target_heading = heading(target, "Input connection", 0, 0, 448);
    s_target_note = label(target, "One host at a time.", 0, 48, 448, MUTED);
    lv_obj_set_height(s_target_note, 64);
    s_target_detail = heading(target, "No target", 0, 124, 448);
    single_line(s_target_detail);
    s_target_link = label(target, "Pair with ESP32-INPUT", 0, 160, 448, MUTED);
    single_line(s_target_link);
    label(target, "Changing targets cancels current input.\nUnsent characters stay in preview.", 0, 216, 448, MUTED);
    button(target, "Connectivity settings", 0, 280, 448, 48, APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
    button(target, "Back", 0, 352, 448, 48, APP_CLOSE, 0);
    create_settings(s_overlay_pages[2]);
    lv_obj_t *locked = s_overlay_pages[3];
    heading(locked, LV_SYMBOL_PAUSE, 0, 48, 448);
    heading(locked, "Input locked", 0, 104, 448);
    s_lock_detail = label(locked, "Input stopped.", 0, 160, 448, MUTED);
    heading(locked, "Hold AI again to unlock.", 0, 288, 448);
    lv_obj_t *setup = s_overlay_pages[4];
    button(setup, LV_SYMBOL_LEFT, 0, 0, 44, 44, APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
    heading(setup, "Set up Wi-Fi", 60, 8, 388);
    s_setup_state = metadata(setup, "Preparing setup...", 0, 56, 448);
    metadata(setup, "1. Join this temporary Wi-Fi network", 0, 84, 448);
    s_setup_ssid = label(setup, "--", 0, 108, 448, INK);
    lv_obj_set_height(s_setup_ssid, 42);
    lv_label_set_long_mode(s_setup_ssid, LV_LABEL_LONG_DOT);
    metadata(setup, "Passcode", 0, 156, 448);
    s_setup_passcode = label(setup, "--", 0, 180, 448, INK);
    lv_obj_set_height(s_setup_passcode, 42);
    lv_label_set_long_mode(s_setup_passcode, LV_LABEL_LONG_DOT);
    metadata(setup, "2. Open this address in your browser", 0, 228, 448);
    s_setup_url = label(setup, "--", 0, 252, 448, INK);
    lv_obj_set_height(s_setup_url, 42);
    lv_label_set_long_mode(s_setup_url, LV_LABEL_LONG_DOT);
    metadata(setup, "Save your home Wi-Fi there. Setup ends automatically.", 0, 320, 448);
    s_setup_stop = button(setup, "Stop setup", 0, 352, 448, 48, APP_WIFI_SETUP, 0);
    lv_obj_t *confirm = s_overlay_pages[5];
    s_confirm_title = heading(confirm, "Confirm action", 0, 32, 448);
    lv_obj_set_height(s_confirm_title, 64);
    s_confirm_detail = label(confirm, "", 0, 128, 448, MUTED);
    lv_obj_set_height(s_confirm_detail, 156);
    button(confirm, "Cancel", 0, 352, 220, 48, APP_CONFIRM, 0);
    s_confirm_accept = button(confirm, "Confirm", 228, 352, 220, 48, APP_CONFIRM, 1);
    selected(s_confirm_accept, true);
    s_sleep_shield = box(s_screen, 0, 0, 480, 480, 0x000000);
    lv_obj_add_flag(s_sleep_shield, LV_OBJ_FLAG_PRESS_LOCK);
}

void app_ui_create(lv_display_t *display, app_submit_fn callback)
{
    s_submit = callback;
    s_binding_count = 0;
    s_color_count = 0;
    s_dark_theme = false;
    s_gesture = 0;
    s_touch_active = false;
    s_wake_touch = false;
    memset(&s_view, 0, sizeof(s_view));
    s_screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(s_screen);
    background(s_screen, SURFACE, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_screen, BODY_FONT, 0);
    foreground(s_screen, INK, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_touch, LV_EVENT_ALL, NULL);
    s_target = button(s_screen, "No target", 16, 8, 260, 48, APP_MENU, APP_OVERLAY_TARGET);
    background(s_target, SURFACE, 0);
    lv_obj_t *target_label = lv_obj_get_child(s_target, 0);
    lv_obj_set_style_text_align(target_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_align(target_label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(target_label, 8, 4);
    lv_obj_set_width(target_label, 244);
    s_target_state = metadata(s_target, "Pair device", 8, 28, 244);
    lv_obj_t *telemetry = button(s_screen, "", 284, 8, 128, 48, APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
    background(telemetry, SURFACE, 0);
    s_telemetry[0] = lv_obj_get_child(telemetry, 0);
    lv_obj_set_style_text_font(s_telemetry[0], META_FONT, 0);
    lv_obj_set_align(s_telemetry[0], LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_telemetry[0], 0, 4);
    lv_obj_set_width(s_telemetry[0], 128);
    single_line(s_telemetry[0]);
    create_signals(telemetry, 0);
    button(s_screen, LV_SYMBOL_SETTINGS, 420, 10, 44, 44, APP_MENU, APP_OVERLAY_SETTINGS);
    for (int i = 0; i < 3; ++i) s_panels[i] = box(s_screen, 16, 72, 448, 320, SURFACE);
    create_mouse(s_panels[INPUT_MOUSE]);
    create_keyboard(s_panels[INPUT_KEYBOARD]);
    create_audio(s_panels[INPUT_AUDIO]);
    s_status = metadata(s_screen, "Local control / motion off", 16, 400, 448);
    for (int i = 0; i < 3; ++i) {
        const char *names[] = {"Mouse", "Keyboard", "Audio"};
        s_modes[i] = button(s_screen, names[i], 16 + i * 152, 424, 144, 48, APP_MODE, i);
    }
    create_overlays();
    app_view_t initial = {.mode = INPUT_MOUSE, .dark_theme = true,
        .keyboard_group = -1, .gain_divisor = 1,
        .microphone_muted = true};
    app_ui_render(&initial);
}

static const char *battery_state_name(app_battery_state_t state)
{
    switch (state) {
    case APP_BATTERY_CHARGING: return "Charging";
    case APP_BATTERY_DISCHARGING: return "Discharging";
    case APP_BATTERY_FULL: return "Full";
    case APP_BATTERY_IDLE: return "Idle";
    default: return "State unknown";
    }
}

static unsigned signal_bars(int dbm)
{
    return dbm >= -55 ? 4U : dbm >= -67 ? 3U : dbm >= -78 ? 2U : 1U;
}

static void render_device(const app_view_t *view, const char *link)
{
    char battery[56], summary[160];
    if (view->battery_supported && view->battery_percent_valid && view->battery_percent <= 100) {
        const char *icon = view->battery_percent >= 90 ? LV_SYMBOL_BATTERY_FULL :
            view->battery_percent >= 60 ? LV_SYMBOL_BATTERY_3 : view->battery_percent >= 35 ?
            LV_SYMBOL_BATTERY_2 : view->battery_percent >= 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(battery, sizeof(battery), "%s %u%%%s", icon, view->battery_percent,
                 view->battery_state == APP_BATTERY_CHARGING ? " " LV_SYMBOL_CHARGE : "");
    } else snprintf(battery, sizeof(battery), LV_SYMBOL_BATTERY_EMPTY " --");
    label_text(s_telemetry[0], battery);
    label_text(s_telemetry[1], battery);
    const bool connected[] = {view->wifi_state == APP_UI_WIFI_CONNECTED, view->bluetooth_connected};
    const bool known[] = {view->wifi_rssi_valid, false};
    const int dbm[] = {view->wifi_rssi_dbm, 0};
    for (unsigned header = 0; header < 2; ++header) {
        for (unsigned radio = 0; radio < 2; ++radio) {
            foreground(s_signal_icons[header][radio], connected[radio] ? INK : MUTED, 0);
            bool measured = connected[radio] && known[radio];
            visible(s_signal_unknown[header][radio], !measured);
            label_text(s_signal_unknown[header][radio], radio == 1 ?
                       (connected[radio] ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE) :
                       connected[radio] ? "?" : LV_SYMBOL_CLOSE);
            for (unsigned bar = 0; bar < 4; ++bar) {
                visible(s_signal_bars[header][radio][bar], measured);
                background(s_signal_bars[header][radio][bar], bar < signal_bars(dbm[radio]) ? ACCENT : LINE, 0);
            }
        }
    }
    const char *titles[] = {"Settings", "Connectivity", "Display", "Input", "About", "Bluetooth hosts", "Wi-Fi"};
    unsigned page = (unsigned)view->settings_page < 7 ? (unsigned)view->settings_page : 0;
    label_text(s_settings_title, titles[page]);
    for (unsigned i = 0; i < 7; ++i) visible(s_settings_pages[i], i == page);
    const char *saved = view->settings_save_state == APP_SAVE_SAVED ? "Settings saved" :
        view->settings_save_state == APP_SAVE_PENDING ? "Saving settings..." :
        view->settings_save_state == APP_SAVE_ERROR ? "Could not save settings. Try again." :
        view->settings_save_state == APP_SAVE_DEFAULTS ? "Defaults active / not saved yet" : "Settings storage unavailable";
    label_text(s_save_status, saved);
    foreground(s_save_status, view->settings_save_state == APP_SAVE_ERROR ? WARNING : MUTED, 0);
    bool usb_selected = view->input_transport == APP_INPUT_USB;
    selected(s_transports[0], !usb_selected);
    selected(s_transports[1], usb_selected);
    for (unsigned i = 0; i < 2; ++i) {
        bool active = (i == 1) == usb_selected;
        background(s_transports[i], active ? FOCUS : SOFT, LV_STATE_DISABLED);
        foreground(s_transports[i], active ? INK : 0x8b9891, LV_STATE_DISABLED);
    }
    text(s_transports[0], view->bluetooth_available ? "Bluetooth" : "Bluetooth unavailable");
    text(s_transports[1], view->usb_available ? "USB" : "USB unavailable");
    enabled(s_transports[0], view->bluetooth_available && !view->transport_switch_pending);
    enabled(s_transports[1], view->usb_available && !view->transport_switch_pending);
    if (*view->transport_message) snprintf(summary, sizeof(summary), "%s", view->transport_message);
    else if (view->transport_switch_pending) snprintf(summary, sizeof(summary), "Switching to %s / %s active",
        view->requested_transport == APP_INPUT_USB ? "USB" : "Bluetooth", usb_selected ? "USB" : "Bluetooth");
    else snprintf(summary, sizeof(summary), "%s selected / %s", usb_selected ? "USB" : "Bluetooth", link);
    label_text(s_transport_status, summary);
    snprintf(summary, sizeof(summary), "%s / %s",
        *view->bluetooth_target_name ? view->bluetooth_target_name : "Bluetooth",
        !view->bluetooth_available ? "Unavailable" : view->bluetooth_connected ?
        (view->bluetooth_ready ? "Input ready" : "Connected") : "Not connected");
    label_text(s_bluetooth_state, summary);
    const char *pairing = view->pairing_state == APP_PAIRING_OPEN ? "Pairing open" :
        view->pairing_state == APP_PAIRING_CLOSED ? "Pairing closed" :
        view->pairing_state == APP_PAIRING_CLEARING ? "Updating pairing..." :
        view->pairing_state == APP_PAIRING_ERROR ? "Pairing error" : "Pairing unavailable";
    char bonds[48], countdown[24] = "";
    bool capacity_known = view->paired_host_capacity > 0;
    bool count_known = view->paired_host_count >= 0;
    bool pairings_full = count_known && capacity_known && (unsigned)view->paired_host_count >= view->paired_host_capacity;
    bool pairing_room = count_known && capacity_known && !pairings_full;
    if (!count_known) snprintf(bonds, sizeof(bonds), "host count unknown");
    else if (!capacity_known) snprintf(bonds, sizeof(bonds), "%d saved / limit unknown", view->paired_host_count);
    else snprintf(bonds, sizeof(bonds), "%d/%u saved%s", view->paired_host_count, view->paired_host_capacity,
                  pairings_full ? " / full" : "");
    if (view->pairing_state == APP_PAIRING_OPEN && view->pairing_seconds_left)
        snprintf(countdown, sizeof(countdown), " / %us left", view->pairing_seconds_left);
    snprintf(summary, sizeof(summary), "%s / %s%s", pairing, bonds, countdown);
    label_text(s_bluetooth_pairing, summary);
    bool pairing_available = view->pairing_state != APP_PAIRING_UNAVAILABLE && view->pairing_state != APP_PAIRING_CLEARING;
    visible(s_pairing_buttons[0], view->pairing_state != APP_PAIRING_OPEN);
    visible(s_pairing_buttons[1], view->pairing_state == APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[0], pairing_available && pairing_room && view->pairing_state != APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[1], view->pairing_state == APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[2], pairing_available && view->paired_host_count != 0);
    text(s_bond_add, !capacity_known ? "Limit unknown" : !count_known ? "Count unknown" :
         pairings_full ? "Pairings full" : "Add host");
    enabled(s_bond_add, pairing_available && pairing_room && view->pairing_state != APP_PAIRING_OPEN);
    enabled(s_bond_disconnect, view->bluetooth_connected);
    size_t host_count = view->bluetooth_host_count > APP_UI_BOND_CAPACITY ? APP_UI_BOND_CAPACITY : view->bluetooth_host_count;
    unsigned page_count = (unsigned)((host_count + APP_UI_BONDS_PER_PAGE - 1) / APP_UI_BONDS_PER_PAGE);
    if (!page_count) page_count = 1;
    unsigned host_page = view->bluetooth_page < page_count ? view->bluetooth_page : page_count - 1;
    visible(s_bond_empty, !view->bluetooth_hosts_available || !host_count);
    label_text(s_bond_empty, view->bluetooth_hosts_available ? "No saved hosts." : "Saved hosts unavailable");
    for (unsigned i = 0; i < APP_UI_BONDS_PER_PAGE; ++i) {
        size_t index = (size_t)host_page * APP_UI_BONDS_PER_PAGE + i;
        bool show = view->bluetooth_hosts_available && index < host_count;
        visible(s_bond_rows[i], show);
        if (!show) continue;
        const app_bond_view_t *host = &view->bluetooth_hosts[index];
        char name[48];
        size_t length = bounded_length(host->identity, sizeof(host->identity));
        snprintf(name, sizeof(name), "Host %s", length > 8 ? host->identity + length - 8 :
                 length ? host->identity : "(identity unavailable)");
        label_text(s_bond_names[i], *host->name ? host->name : name);
        label_text(s_bond_ids[i], *host->identity ? host->identity : "Identity unavailable");
        label_text(s_bond_states[i], host->connected ? "Connected" : "Saved");
        foreground(s_bond_states[i], host->connected ? ACCENT : MUTED, 0);
    }
    char capacity[32];
    if (!capacity_known) snprintf(capacity, sizeof(capacity), "Limit unknown");
    else if (!count_known) snprintf(capacity, sizeof(capacity), "Count unknown");
    else snprintf(capacity, sizeof(capacity), "%s%d/%u%s", pairings_full ? "Full: " : "",
                  view->paired_host_count, view->paired_host_capacity, pairings_full ? "" : " saved");
    lv_label_set_text_fmt(s_bond_page, "%u / %u\n%s", host_page + 1, page_count, capacity);
    foreground(s_bond_page, pairings_full ? WARNING : MUTED, 0);
    enabled(s_bond_previous, view->bluetooth_hosts_available && host_page > 0);
    enabled(s_bond_next, view->bluetooth_hosts_available && host_page + 1 < page_count);
    const char *wifi = view->wifi_state == APP_UI_WIFI_CONNECTED ? "Connected" :
        view->wifi_state == APP_UI_WIFI_CONNECTING ? "Connecting" :
        view->wifi_state == APP_UI_WIFI_RETRYING ? "Reconnecting" :
        view->wifi_state == APP_UI_WIFI_ERROR ? "Connection error" :
        view->wifi_state == APP_UI_WIFI_DISCONNECTED ? "Disconnected" : "Unavailable";
    snprintf(summary, sizeof(summary), "%s%s%s", wifi, *view->wifi_ssid ? " / " : "", view->wifi_ssid);
    label_text(s_wifi_summary, *view->wifi_message ? view->wifi_message : summary);
    foreground(s_wifi_summary, *view->wifi_message ? WARNING : MUTED, 0);
    label_text(s_wifi_state, wifi);
    label_text(s_wifi_ssid, *view->wifi_ssid ? view->wifi_ssid : "No network");
    label_text(s_wifi_note, *view->wifi_message ? view->wifi_message :
               view->wifi_saved_network ? "Saved network reconnects automatically." : "Use setup to save a network.");
    if (view->wifi_rssi_valid && view->wifi_state == APP_UI_WIFI_CONNECTED)
        snprintf(summary, sizeof(summary), "%d dBm / %s", view->wifi_rssi_dbm,
                 *view->wifi_ipv4 ? view->wifi_ipv4 : "IP unavailable");
    else if (view->wifi_state == APP_UI_WIFI_CONNECTED)
        snprintf(summary, sizeof(summary), "Signal unknown / %s", *view->wifi_ipv4 ? view->wifi_ipv4 : "IP unavailable");
    else snprintf(summary, sizeof(summary), "%s", view->wifi_state == APP_UI_WIFI_UNAVAILABLE ?
                  "Signal and IP unavailable" : "No active network connection");
    label_text(s_wifi_detail, summary);
    text(s_wifi_setup, view->wifi_setup_active ? "View setup" : view->wifi_operation_pending ? "Please wait..." : "Set up Wi-Fi");
    enabled(s_wifi_setup, view->wifi_state != APP_UI_WIFI_UNAVAILABLE &&
            (!view->wifi_operation_pending || view->wifi_setup_active));
    enabled(s_wifi_forget, view->wifi_state != APP_UI_WIFI_UNAVAILABLE &&
            view->wifi_saved_network && !view->wifi_operation_pending);
    if (view->brightness_available) lv_label_set_text_fmt(s_brightness_text, "%u%%", view->brightness_percent);
    else label_text(s_brightness_text, "Unavailable");
    if (!lv_slider_is_dragged(s_brightness)) {
        unsigned brightness = view->brightness_percent < 10 ? 10 : view->brightness_percent > 100 ? 100 : view->brightness_percent;
        lv_slider_set_value(s_brightness, (int)((brightness + 5) / 10), LV_ANIM_OFF);
    }
    enabled(s_brightness, view->brightness_available);
    const unsigned seconds[] = {0, 60, 300, 600, 1800};
    for (unsigned i = 0; i < 5; ++i) {
        selected(s_timeouts[i], view->idle_timeout_seconds == seconds[i]);
        enabled(s_timeouts[i], view->screen_off_available);
    }
    const char *identity[] = {view->firmware_version, view->build_id, view->board_revision, view->device_id};
    for (unsigned i = 0; i < 4; ++i) label_text(s_about_identity[i], *identity[i] ? identity[i] : "Unavailable");
    if (!view->battery_supported) snprintf(summary, sizeof(summary), "Battery unavailable\nUSB power detection unavailable");
    else if (view->battery_percent_valid && view->battery_percent <= 100)
        snprintf(summary, sizeof(summary), "Battery  %u%% / %s\nUSB power detection unavailable", view->battery_percent, battery_state_name(view->battery_state));
    else snprintf(summary, sizeof(summary), "Battery  -- / %s\nUSB power detection unavailable", battery_state_name(view->battery_state));
    label_text(s_about_battery, summary);
    lv_label_set_text_fmt(s_about_uptime, "Uptime  %lluh %02llum", (unsigned long long)(view->uptime_seconds / 3600),
                         (unsigned long long)(view->uptime_seconds / 60 % 60));
    if (!view->wifi_setup_active && *view->wifi_message) label_text(s_setup_state, view->wifi_message);
    else if (view->wifi_setup_active) lv_label_set_text_fmt(s_setup_state, "Protected setup / expires in %u:%02u", view->wifi_setup_seconds_left / 60, view->wifi_setup_seconds_left % 60);
    else label_text(s_setup_state, view->wifi_operation_pending ? "Preparing protected setup..." : "Setup ended / credentials cleared");
    bool show_credentials = view->wifi_setup_active && view->overlay == APP_OVERLAY_WIFI_SETUP && !view->locked && !view->screen_off;
    label_text(s_setup_ssid, show_credentials && *view->wifi_setup_ssid ? view->wifi_setup_ssid : "--");
    label_text(s_setup_passcode, show_credentials && *view->wifi_setup_passcode ? view->wifi_setup_passcode : "--");
    label_text(s_setup_url, show_credentials && *view->wifi_setup_url ? view->wifi_setup_url : "--");
    enabled(s_setup_stop, view->wifi_setup_active || view->wifi_operation_pending);
    const char *title = "Confirm action";
    const char *detail = "No action is pending. Return to Settings.";
    const char *accept = "Confirm";
    switch (view->confirmation) {
    case APP_CONFIRM_PAIR_OPEN:
        title = "Open Bluetooth pairing?";
        detail = "Nearby hosts can pair while pairing is open.\nChoose ESP32-INPUT in the host's\nBluetooth settings.";
        accept = "Open pairing";
        break;
    case APP_CONFIRM_PAIR_CLOSE:
        title = "Close Bluetooth pairing?";
        detail = "New hosts will no longer be able to pair.\nSaved pairings remain available.";
        accept = "Close pairing";
        break;
    case APP_CONFIRM_PAIR_FORGET_ALL:
        title = "Forget Bluetooth pairings?";
        detail = "All saved Bluetooth pairings will be removed.\nA connected Bluetooth host may disconnect.\nPair again before using Bluetooth input.";
        accept = "Forget pairings";
        break;
    case APP_CONFIRM_DISCONNECT:
        title = "Disconnect Bluetooth host?";
        detail = "The Bluetooth host will disconnect.\nIts saved pairing stays available.\nReconnect from that host's Bluetooth settings.";
        accept = "Disconnect";
        break;
    case APP_CONFIRM_WIFI_FORGET:
        title = "Forget Wi-Fi network?";
        detail = "The saved Wi-Fi network will be removed.\nWi-Fi disconnects and setup stops.\nSet up Wi-Fi again to reconnect.";
        accept = "Forget network";
        break;
    default: break;
    }
    label_text(s_confirm_title, title);
    label_text(s_confirm_detail, detail);
    text(s_confirm_accept, accept);
    enabled(s_confirm_accept, view->confirmation != APP_CONFIRM_NONE);
}

void app_ui_render(const app_view_t *view)
{
    if (!view || !s_screen) return;
    s_view = *view;
    snprintf(s_target_text, sizeof(s_target_text), "%s", view->target_name && *view->target_name ?
             view->target_name : view->connected ?
             (view->input_transport == APP_INPUT_USB ? "USB host" : "Bluetooth host") : "No target");
    snprintf(s_message_text, sizeof(s_message_text), "%s", view->message ? view->message : "");
    s_view.target_name = s_target_text;
    s_view.message = s_message_text;
    s_view.preview[APP_UI_DRAFT_CAPACITY] = '\0';
    for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) s_view.keyboard_cells[i][7] = '\0';
    s_view.wifi_ssid[32] = '\0';
    s_view.wifi_ipv4[15] = '\0';
    s_view.wifi_message[64] = '\0';
    s_view.wifi_setup_ssid[32] = '\0';
    s_view.wifi_setup_passcode[24] = '\0';
    s_view.wifi_setup_url[63] = '\0';
    s_view.firmware_version[31] = '\0';
    s_view.build_id[23] = '\0';
    s_view.board_revision[23] = '\0';
    s_view.device_id[23] = '\0';
    s_view.transport_message[64] = '\0';
    s_view.bluetooth_target_name[47] = '\0';
    for (unsigned i = 0; i < APP_UI_BOND_CAPACITY; ++i) {
        s_view.bluetooth_hosts[i].identity[23] = '\0';
        s_view.bluetooth_hosts[i].name[32] = '\0';
    }
    if (!s_view.wifi_setup_active || s_view.overlay != APP_OVERLAY_WIFI_SETUP ||
        s_view.locked || s_view.screen_off) {
        memset(s_view.wifi_setup_ssid, 0, sizeof(s_view.wifi_setup_ssid));
        memset(s_view.wifi_setup_passcode, 0, sizeof(s_view.wifi_setup_passcode));
        memset(s_view.wifi_setup_url, 0, sizeof(s_view.wifi_setup_url));
    }
    view = &s_view;
    apply_theme(view->dark_theme);
    bool usb = view->input_transport == APP_INPUT_USB;
    bool provider_available = usb ? view->usb_available : view->bluetooth_available;
    const char *link = view->transport_switch_pending ? "Switching input..." :
        !provider_available ? "Input unavailable" : view->transport_error ? "Input connection error" :
        !view->connected ? (usb ? "Connect USB" : "Pair device") :
        view->release_pending ? "Releasing input" : view->congested ? "Host busy" :
        view->ready ? "Input ready" : "Connecting input";
    text(s_target, s_target_text);
    label_text(s_target_state, link);
    label_text(s_overlay_target, s_target_text);
    label_text(s_overlay_state, link);
    render_device(view, link);
    label_text(s_target_detail, s_target_text);
    label_text(s_target_heading, usb ? "USB input" : "Bluetooth input");
    label_text(s_target_note, usb ?
        "USB is selected for input in every mode.\nConnect a host using the USB data port." :
        "Bluetooth is selected for input in every mode.\nPair from the host's Bluetooth settings.");
    label_text(s_target_link, !provider_available ? "Input unavailable on this device" :
        view->connected ? link : usb ? "Waiting for a USB host" : "Pair with ESP32-INPUT");
    const char *status = view->locked ? "Input locked / hold AI to unlock" :
        view->remote_owned ? "Remote control / touch to take local control" :
        view->sending ? "Preview first / sending without Enter" :
        view->mode == INPUT_KEYBOARD ? "Local control / preview first / no Enter" :
        view->mode == INPUT_AUDIO ? "Audio off / independent mode" :
        view->motion_enabled ? "Local control / motion on" : "Local control / motion off";
    label_text(s_status, *s_message_text ? s_message_text : status);
    /* A notification in flight can briefly report congestion. Keep controls
     * hittable so touch-down can suppress motion before the click is queued;
     * the coordinator still owns transport acceptance and fault handling. */
    bool hid_ready = view->connected && view->ready && !view->release_pending &&
                     !view->transport_error && !view->locked && provider_available && !view->transport_switch_pending;
    for (int i = 0; i < 3; ++i) {
        visible(s_panels[i], (input_mode_t)i == view->mode);
        selected(s_modes[i], (input_mode_t)i == view->mode);
        selected(s_mode_choices[i], (input_mode_t)i == view->mode);
    }
    text(s_motion, view->calibrating ? "Calibrating..." : view->motion_enabled ? "Motion / on" : "Motion / off");
    selected(s_motion, view->motion_enabled);
    enabled(s_motion, view->motion_available && hid_ready && !view->calibrating);
    const unsigned gains[] = {1, 4, 8};
    for (unsigned i = 0; i < 3; ++i) selected(s_gains[i], (view->gain_divisor ? view->gain_divisor : 1) == gains[i]);
    text(s_drag, view->dragging ? "Release" : "Drag");
    selected(s_drag, view->dragging);
    enabled(s_drag, hid_ready || view->dragging);
    for (unsigned i = 0; i < 4; ++i) enabled(s_mouse_controls[i], hid_ready);
    for (int i = 0; i < 4; ++i) selected(s_submodes[i], (app_keyboard_submode_t)i == view->keyboard_submode);
    label_text(s_preview, *view->preview ? view->preview : "Ready for input...");
    if (view->sending || view->send_total) {
        lv_label_set_text_fmt(s_progress, "Accepted %u / %u / no Enter", (unsigned)view->accepted_prefix, (unsigned)view->send_total);
    } else {
        lv_label_set_text_fmt(s_progress, "%u / 64", (unsigned)bounded_length(view->preview, sizeof(view->preview)));
    }
    enabled(s_clear, !view->sending && *view->preview);
    visible(s_grid_page, view->keyboard_submode <= APP_KEYBOARD_TOUCH);
    visible(s_voice_page, view->keyboard_submode == APP_KEYBOARD_SPOKEN);
    visible(s_shortcut_page, view->keyboard_submode == APP_KEYBOARD_SHORTCUTS);
    text(s_back, "Groups");
    enabled(s_back, view->keyboard_group >= 0 && !view->sending);
    text(s_case, view->lowercase ? "abc" : "ABC");
    text(s_symbols, view->symbols ? "ABC/123" : "Symbols");
    enabled(s_case, !view->sending);
    enabled(s_symbols, !view->sending);
    for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) {
        text(s_grid[i], view->keyboard_cells[i]);
        bool valid_cell = i < view->keyboard_cell_count && view->keyboard_cells[i][0];
        visible(s_grid[i], valid_cell);
        enabled(s_grid[i], valid_cell && !view->sending);
        bool focused = view->keyboard_submode == APP_KEYBOARD_MOTION && view->keyboard_focus == i;
        background(s_grid[i], focused ? FOCUS : SOFT, 0);
        lv_obj_set_style_border_width(s_grid[i], focused ? 2 : 0, 0);
        border(s_grid[i], ACCENT, 0);
    }
    char candidate[2] = {view->voice_candidate, '\0'};
    label_text(s_voice_candidate, view->voice_candidate ? candidate : view->listening ? "..." : "A-Z");
    label_text(s_voice_hint, !view->recognition_available ?
        "Spoken letters unavailable\nNo microphone is active." : view->voice_candidate ?
        "Confirm the candidate\nto add it to preview." : view->listening ?
        "Listening for a letter...\nNothing is sent automatically." : "Say Alpha-Zulu.\nConfirm before sending.");
    text(s_voice_confirm, view->voice_candidate ? "Confirm" : view->listening ? "Stop listening" : "Start listening");
    enabled(s_voice_confirm, view->recognition_available && !view->sending);
    const char *ctrl_names[] = {"Ctrl+C", "Ctrl+V", "Ctrl+Z"};
    const char *command_names[] = {"Cmd+C", "Cmd+V", "Cmd+Z"};
    for (unsigned i = 0; i < 6; ++i) {
        if (i < 3) text(s_shortcuts[i], view->host_os == APP_HOST_COMMAND ? command_names[i] : ctrl_names[i]);
        enabled(s_shortcuts[i], hid_ready && !view->sending);
    }
    enabled(s_delete, !view->sending && *view->preview);
    enabled(s_space, !view->sending && bounded_length(view->preview, sizeof(view->preview)) < APP_UI_DRAFT_CAPACITY);
    text(s_send, view->sending ? "Stop sending" : "Send");
    selected(s_send, true);
    enabled(s_send, view->sending || (hid_ready && *view->preview));
    bool speaker = view->audio_kind == APP_AUDIO_SPEAKER;
    label_text(s_audio_capability, view->audio_available ? "Available" : "Unavailable");
    selected(s_audio_kinds[0], !speaker);
    selected(s_audio_kinds[1], speaker);
    label_text(s_audio_title, speaker ? (view->audio_active ? "Speaker active" : "Speaker is off") :
                      !view->audio_available || view->microphone_muted ? "Microphone muted" : "Microphone active");
    label_text(s_audio_profile, speaker ? "Music playback" : "Voice and meetings");
    text(s_audio_power, view->audio_active ? "Stop audio" : "Start audio");
    lv_obj_set_pos(s_audio_power, speaker ? 122 : 16, 100);
    selected(s_audio_power, true);
    text(s_audio_mute, view->microphone_muted || !view->audio_available ? "Muted" : "Mute");
    visible(s_audio_mute, !speaker);
    enabled(s_audio_power, view->audio_available && !view->locked);
    enabled(s_audio_mute, view->audio_available && view->audio_active && !view->locked);
    enabled(s_volume, view->audio_available && !view->locked);
    lv_slider_set_value(s_volume, (int)(view->volume <= 100 ? view->volume : 100), LV_ANIM_OFF);
    if (view->audio_available) lv_label_set_text_fmt(s_volume_text, "%u%%", view->volume);
    else label_text(s_volume_text, "--");
    label_text(s_audio_note, !view->audio_available ? "Audio unavailable on this device." :
                      speaker ? "Speaker mode never uploads microphone audio." : "Independent audio mode / microphone defaults muted.");
    selected(s_themes[0], !view->dark_theme);
    selected(s_themes[1], view->dark_theme);
    selected(s_hosts[0], view->host_os == APP_HOST_CTRL);
    selected(s_hosts[1], view->host_os == APP_HOST_COMMAND);
    label_text(s_haptics_text, !view->haptics_available ? "Unavailable" : view->haptics_enabled ? "On" : "Off");
    if (view->haptics_available && view->haptics_enabled) lv_obj_add_state(s_haptics, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_haptics, LV_STATE_CHECKED);
    enabled(s_haptics, view->haptics_available);
    text(s_calibrate, view->calibrating ? "Calibrating... keep still" : "Calibrate motion");
    enabled(s_calibrate, (view->calibration_available || view->motion_available) && !view->calibrating);
    label_text(s_lock_detail, view->release_pending ?
        "Input stopped. Releasing held keys...\nRecognition stopped; microphone muted.\nUnsent characters remain in preview." :
        "Input stopped. Recognition stopped and\nmicrophone muted. Unsent characters\nremain in preview.");
    bool overlay = view->locked || view->overlay != APP_OVERLAY_NONE;
    visible(s_overlay, overlay);
    int page = view->locked ? 3 : view->overlay == APP_OVERLAY_MODES ? 0 :
               view->overlay == APP_OVERLAY_TARGET ? 1 : view->overlay == APP_OVERLAY_WIFI_SETUP ? 4 :
               view->overlay == APP_OVERLAY_CONFIRM ? 5 : 2;
    for (int i = 0; i < 6; ++i) visible(s_overlay_pages[i], overlay && i == page);
    visible(s_sleep_shield, view->screen_off);
}
