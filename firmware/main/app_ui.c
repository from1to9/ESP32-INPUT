// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
/* LVGL owner data only; never DMA, ISR, or cache-disabled access. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define UI_TASK_BSS EXT_RAM_BSS_ATTR
#else
#define UI_TASK_BSS
#endif
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Logical geometry follows design/ui-plan.md; all navigation is coordinator-owned. */
#define INK 0x182a25
#define MUTED 0x5c6c65
#define LINE 0xdae1db
#define SOFT 0xedf1eb
#define ACCENT 0x225b44
#define FOCUS 0xc5e5b5
#define WARNING 0x885018
#define ERROR 0xa12e35
#define SURFACE 0xffffff
#define ON_ACCENT 0xfffefe
#define MAX_BINDINGS 64
#define MAX_COLOR_BINDINGS 512
#define COMMON_KEY_BUTTONS 15

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
    uint32_t wifi_generation;
    uint16_t wifi_record;
    app_motion_field_t motion_field;
    bool motion_delta;
} binding_t;

static app_submit_fn s_submit;
/* Only the LVGL owner reads/writes this snapshot; never ISR or cache-disabled code. */
static UI_TASK_BSS app_view_t s_view;
static char s_message_text[160];
static char s_target_text[96];
static UI_TASK_BSS binding_t s_bindings[MAX_BINDINGS];
static size_t s_binding_count;
static lv_obj_t *s_screen, *s_panels[3], *s_mode_title, *s_context, *s_status;
static lv_obj_t *s_critical, *s_keyboard_choice, *s_host_keys, *s_audio_choice, *s_gain;
static lv_obj_t *s_navigation_home, *s_navigation_back, *s_navigation, *s_reset_grip;
static lv_obj_t *s_motion, *s_pad, *s_drag, *s_mouse_controls[4];
static lv_obj_t *s_pad_hint;
static lv_obj_t *s_submodes[4], *s_preview, *s_progress, *s_clear;
static lv_obj_t *s_grid_page, *s_grid[APP_UI_GRID_CELLS], *s_back, *s_case, *s_symbols;
static lv_obj_t *s_voice_page, *s_voice_candidate, *s_voice_hint, *s_voice_confirm;
static lv_obj_t *s_shortcut_page, *s_shortcuts[6], *s_delete, *s_space, *s_send;
static lv_obj_t *s_audio_power[2], *s_audio_state[2];
static lv_obj_t *s_audio_mute, *s_volume, *s_volume_text, *s_audio_note;
static lv_obj_t *s_mic_level, *s_mic_level_label;
static char s_mic_level_text[32];
static lv_obj_t *s_overlay, *s_overlay_target, *s_overlay_critical;
static lv_obj_t *s_telemetry, *s_overlay_state;
static lv_obj_t *s_settings_title, *s_settings_back, *s_save_status;
static lv_obj_t *s_transports[2], *s_transport_status, *s_wifi_summary, *s_wifi_ssid, *s_wifi_note;
static lv_obj_t *s_bluetooth_state, *s_bluetooth_pairing, *s_pairing_buttons[3];
static lv_obj_t *s_bond_names[4], *s_bond_ids[4], *s_bond_states[4], *s_bond_rows[4];
static lv_obj_t *s_bond_empty, *s_bond_page, *s_bond_previous, *s_bond_next, *s_bond_add, *s_bond_disconnect;
static lv_obj_t *s_wifi_state, *s_wifi_detail, *s_wifi_setup, *s_wifi_forget;
static lv_obj_t *s_brightness, *s_brightness_text, *s_timeouts[5];
static lv_obj_t *s_about_identity[6], *s_about_repository, *s_about_battery, *s_about_uptime;
static lv_obj_t *s_setup_state, *s_setup_ssid, *s_setup_passcode, *s_setup_url, *s_setup_stop;
static lv_obj_t *s_confirm_title, *s_confirm_detail, *s_confirm_accept, *s_sleep_shield, *s_lock_shield;
static bool s_wake_touch;
static lv_obj_t *s_target_detail, *s_target_link, *s_target_heading, *s_target_note, *s_lock_detail;
static lv_obj_t *s_hosts[2], *s_themes[2], *s_haptics, *s_haptics_text, *s_calibrate, *s_mode_choices[3];
static size_t s_core_bindings, s_core_colors;
static app_overlay_t s_built_overlay;
static app_settings_page_t s_built_settings;
static uint32_t s_gesture;
static bool s_touch_bluetooth_connected, s_touch_bluetooth_paused;
static uint64_t s_touch_epoch, s_touch_revision;
static uint64_t s_touch_settings_revision;
static uint64_t s_touch_recording_storage_generation, s_touch_recording_storage_revision;
static bool s_touch_active;
static bool s_touch_local_blocked;
static lv_obj_t *s_touch_target;
static lv_indev_t *s_touch_device;
static app_touch_surface_t s_touch_surface;
static lv_point_t s_last_point;
static uint32_t s_last_touch_heartbeat;
enum { WHEEL_STEP_PX = 24, WHEEL_TAP_SLOP_PX = 8 };
static struct {
    lv_area_t bounds;
    int32_t anchor_y;
    bool tracking, tap, wait_all_up;
} s_wheel;
static app_local_edit_submit_fn s_local_submit;
static char s_wifi_password[APP_UI_WIFI_PASSWORD_CAPACITY];
static char s_bluetooth_name[APP_UI_BLUETOOTH_NAME_CAPACITY];
static uint64_t s_editor_session, s_touch_editor_session;
static app_local_edit_purpose_t s_editor_purpose;
static bool s_editor_consumed, s_editor_lowercase, s_editor_symbols;
static int s_editor_group = -1;
static const char *s_editor_feedback;
static uint32_t s_built_scan_generation;
static unsigned s_built_scan_page;
static uint64_t s_built_editor_session;
static app_local_edit_purpose_t s_built_editor_purpose;
static lv_obj_t *s_scan_rows[4], *s_scan_meta[4], *s_scan_state, *s_scan_empty;
static lv_obj_t *s_scan_previous, *s_scan_next, *s_scan_page, *s_scan_refresh;
static lv_obj_t *s_network_name, *s_network_detail, *s_network_note, *s_network_join[2];
static lv_obj_t *s_wifi_find, *s_wifi_saved, *s_wifi_backend, *s_name_edit, *s_tap_click;
static lv_obj_t *s_motion_detail, *s_common_context, *s_common_modifiers, *s_common_keys[COMMON_KEY_BUTTONS];
static lv_obj_t *s_motion_values[5], *s_motion_sliders[5], *s_motion_nudges[5][2];
static lv_obj_t *s_motion_presets[2][3], *s_motion_invert[2], *s_motion_reference;
static lv_obj_t *s_gesture_help, *s_gesture_drag, *s_gesture_release;
static lv_obj_t *s_record_store_state, *s_record_store_profile, *s_record_store_capacity;
static lv_obj_t *s_record_store_time, *s_record_store_reason, *s_record_store_prepare;
static lv_obj_t *s_editor_preview, *s_editor_context, *s_editor_source, *s_editor_status, *s_editor_grid[9];
static lv_obj_t *s_editor_back, *s_editor_case, *s_editor_symbol, *s_editor_delete;
static lv_obj_t *s_editor_space, *s_editor_apply;

static void render_editor(void);
static void media_press(lv_obj_t *target);
static void media_action(lv_obj_t *target, int level);
static void render_media_core(void);
static void render_speech(void);
static void connection_press(lv_obj_t *target);
static void connection_action(lv_obj_t *target);
static void remote_review_press(lv_obj_t *target);
static void remote_review_action(lv_obj_t *target, bool confirm);

static void secure_clear(void *buffer, size_t capacity)
{
    volatile unsigned char *bytes = buffer;
    while (capacity--) *bytes++ = 0;
}

static void clear_editor(void)
{
    secure_clear(s_wifi_password, sizeof(s_wifi_password));
    secure_clear(s_bluetooth_name, sizeof(s_bluetooth_name));
    s_editor_consumed = true;
    s_editor_feedback = NULL;
    s_editor_group = -1;
    s_editor_lowercase = false;
    s_editor_symbols = false;
}

typedef enum { STYLE_BACKGROUND, STYLE_TEXT, STYLE_BORDER } color_property_t;
typedef struct {
    lv_obj_t *object;
    uint32_t token;
    lv_style_selector_t selector;
    color_property_t property;
} color_binding_t;

static UI_TASK_BSS color_binding_t s_colors[MAX_COLOR_BINDINGS];
static size_t s_color_count;
static bool s_dark_theme;
static bool s_keyboard_header_compact;
static app_ui_capture_indicator_t s_capture_indicator;
static uint64_t s_capture_indicator_revision;

static bool capture_warning_required(const app_view_t *view)
{
    return view->capture_indicator_required || view->listening ||
        (view->audio_active && view->audio_kind == APP_AUDIO_VOICE && !view->microphone_muted);
}

static bool recording_storage_can_prepare(const app_view_t *view)
{
    const app_recording_storage_view_t *store = &view->recording_storage;
    return store->state == APP_RECORD_STORE_UNPREPARED &&
        store->backend == APP_RECORD_BACKEND_INTERNAL_RESERVED &&
        store->generation != 0 && store->reserved_region_verified && store->can_prepare;
}

static const char *capture_warning(const app_view_t *view, bool compact)
{
    if (!view->capture_indicator_required) return compact ? "Mic active\nStop + lock" : "Microphone active";
    switch (view->capture_state) {
    case APP_CAPTURE_STARTING: return compact ? "Mic starting\nStop + lock" : "Microphone starting";
    case APP_CAPTURE_ACTIVE: return compact ? "Mic active\nStop + lock" : "Microphone active";
    case APP_CAPTURE_STOPPING: return compact ? "Mic stopping\nStop + lock" : "Stopping microphone; waiting for confirmation";
    case APP_CAPTURE_UNCERTAIN: return compact ? "Mic uncertain\nStop + lock" : "Microphone stop unconfirmed";
    default: return compact ? "Mic unknown\nStop + lock" : "Microphone status unknown; stop required";
    }
}

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
    case ERROR: token = 0xffb4ab; break;
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
    if (!object) return;
    size_t index = 0;
    for (; index < s_color_count; ++index) {
        if (s_colors[index].object == object && s_colors[index].selector == selector &&
            s_colors[index].property == property) break;
    }
    if (index < s_color_count && s_colors[index].token == token) return;
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

static bool about_text_valid(const char *value, size_t capacity)
{
    size_t length = bounded_length(value, capacity);
    if (!length || length == capacity) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] > 0x7e) return false;
    return value[0] != ' ' && value[length - 1] != ' ';
}

static bool about_hex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static bool about_commit_valid(const char value[41])
{
    if (bounded_length(value, 41) != 40) return false;
    for (unsigned i = 0; i < 40; ++i) if (!about_hex(value[i])) return false;
    return true;
}

static bool about_repository_valid(const char value[96])
{
    if (!about_text_valid(value, 96) || strncmp(value, "https://", 8)) return false;
    size_t i = 8;
    bool dot = false;
    unsigned label_length = 0;
    for (; value[i] && value[i] != '/' && value[i] != ':'; ++i) {
        char c = value[i];
        if (c == '.') {
            if (!label_length || value[i - 1] == '-') return false;
            dot = true; label_length = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || (c == '-' && label_length))) return false;
            if (++label_length > 63) return false;
        }
    }
    if (!label_length || !dot || value[i - 1] == '-') return false;
    if (value[i] == ':') {
        unsigned port = 0, digits = 0;
        for (++i; value[i] && value[i] != '/'; ++i) {
            if (value[i] < '0' || value[i] > '9' || ++digits > 5) return false;
            port = port * 10 + (unsigned)(value[i] - '0');
        }
        if (!digits || !port || port > 65535) return false;
    }
    if (value[i] != '/' || !value[i + 1]) return false;
    for (++i; value[i]; ++i) {
        char c = value[i];
        if (c == '%') {
            if (!value[i + 1] || !value[i + 2] || !about_hex(value[i + 1]) || !about_hex(value[i + 2])) return false;
            i += 2;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || strchr("-._~!$&'()*+,;=:@/", c))) return false;
    }
    return true;
}

static void visible(lv_obj_t *object, bool show)
{
    if (!object) return;
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

/* Keep each widget's normal availability when a temporary update policy
 * masks it. Static buttons and live capability-driven controls share this path. */
#define UI_BROWSE_CONTROL LV_OBJ_FLAG_USER_1
#define UI_SAFETY_CONTROL LV_OBJ_FLAG_USER_2
#define UI_UPDATE_MASKED LV_OBJ_FLAG_USER_3

static void enabled(lv_obj_t *object, bool allow)
{
    if (!object) return;
    if (s_view.local_access != APP_LOCAL_NORMAL) {
        bool blocked = !lv_obj_has_flag(object, UI_SAFETY_CONTROL) &&
            !(s_view.local_access == APP_LOCAL_BROWSE && lv_obj_has_flag(object, UI_BROWSE_CONTROL));
        lv_obj_set_flag(object, UI_UPDATE_MASKED, allow && blocked);
        if (blocked) allow = false;
    }
    /* LVGL suppresses RELEASED for a disabled active object. Keep the real
     * gesture open until physical release, which then emits PRESS_LOST even
     * for a disabled widget. This yields one CANCEL and no stale CLICKED. */
    if (!allow && s_touch_active && object == s_touch_target &&
        !lv_obj_has_state(object, LV_STATE_DISABLED))
        lv_indev_wait_release(s_touch_device);
    if (allow) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

typedef struct {
    size_t capacity;
    char value[];
} label_source_t;

static void release_label_source(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target(event);
    label_source_t *source = lv_obj_get_user_data(object);
    if (source) memset(source->value, 0, source->capacity);
    lv_free(source);
    lv_obj_set_user_data(object, NULL);
}

static void label_text(lv_obj_t *object, const char *value)
{
    if (!object) return;
    /* LVGL DOT mode mutates its text buffer. Compare the original source,
     * not lv_label_get_text(), or long labels redraw every snapshot. Each
     * label owns a bounded source copy, freed with the label, with no idle
     * allocation and no dependency on private LVGL structures. */
    label_source_t *source = lv_obj_get_user_data(object);
    if (source && strcmp(source->value, value) == 0) return;
    size_t length = strlen(value) + 1;
    if (!source || source->capacity < length) {
        bool first = source == NULL;
        label_source_t *resized = lv_realloc(source, sizeof(*source) + length);
        LV_ASSERT_MALLOC(resized);
        if (!resized) return;
        source = resized;
        source->capacity = length;
        lv_obj_set_user_data(object, source);
        if (first) lv_obj_add_event_cb(object, release_label_source, LV_EVENT_DELETE, NULL);
    }
    memset(source->value, 0, source->capacity);
    memcpy(source->value, value, length);
    lv_label_set_text(object, value);
}

/* Formatting uses stack storage; unchanged snapshots never reallocate labels. */
static void label_format(lv_obj_t *object, const char *format, ...)
{
    if (!object) return;
    char value[192];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(value, sizeof(value), format, arguments);
    va_end(arguments);
    label_text(object, value);
}

static void slider_value(lv_obj_t *object, int value)
{
    if (object && !lv_slider_is_dragged(object) && lv_slider_get_value(object) != value)
        lv_slider_set_value(object, value, LV_ANIM_OFF);
}

static void storage_bytes(char *output, size_t capacity, uint64_t bytes)
{
    if (bytes < 1024) snprintf(output, capacity, "%llu bytes", (unsigned long long)bytes);
    else snprintf(output, capacity, "%llu.%02llu KiB", (unsigned long long)(bytes / 1024),
                  (unsigned long long)((bytes % 1024) * 100 / 1024));
}

static void text(lv_obj_t *object, const char *value)
{
    if (!object) return;
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

static bool submit_identity(app_action_t action, int x, int y, uint32_t generation, uint16_t record)
{
    app_intent_t intent = {
        .action = action, .epoch = s_gesture ? s_touch_epoch : s_view.epoch,
        .ui_revision = s_gesture ? s_touch_revision : s_view.ui_revision, .x = x, .y = y,
        .gesture = s_gesture, .surface = s_touch_surface,
        .wifi_scan_generation = generation, .wifi_record_index = record
    };
    if (action == APP_RECORDING_STORAGE_PREPARE_REQUEST ||
        (action == APP_CONFIRM && s_view.confirmation == APP_CONFIRM_RECORDING_STORAGE_PREPARE)) {
        intent.recording_storage_generation = s_touch_recording_storage_generation;
        intent.recording_storage_revision = s_touch_recording_storage_revision;
    }
    bool accepted = s_submit && s_submit(&intent);
    if (!accepted && s_status) {
        label_text(s_status, "Input busy. Please try again.");
    }
    return accepted;
}

static bool submit(app_action_t action, int x, int y)
{
    return submit_identity(action, x, y, 0, 0);
}

static bool in_overlay(lv_obj_t *object)
{
    while (object) {
        if (object == s_overlay) return true;
        object = lv_obj_get_parent(object);
    }
    return false;
}

static bool motion_settings_valid(const app_motion_settings_view_t *settings)
{
    return settings->available && settings->gain_x_centi >= 25 && settings->gain_x_centi <= 10000 &&
        settings->gain_y_centi >= 25 && settings->gain_y_centi <= 10000 &&
        settings->deadzone_mdps <= 10000 && settings->filter_ms <= 1000 && settings->freeze_ms <= 1000;
}

static int motion_setting(app_motion_field_t field)
{
    const app_motion_settings_view_t *settings = &s_view.motion_settings;
    switch (field) {
    case APP_MOTION_FIELD_GAIN_X: return settings->gain_x_centi;
    case APP_MOTION_FIELD_GAIN_Y: return settings->gain_y_centi;
    case APP_MOTION_FIELD_DEADZONE: return settings->deadzone_mdps;
    case APP_MOTION_FIELD_FILTER: return settings->filter_ms;
    case APP_MOTION_FIELD_FREEZE: return settings->freeze_ms;
    case APP_MOTION_FIELD_INVERT_X: return settings->invert_x;
    case APP_MOTION_FIELD_INVERT_Y: return settings->invert_y;
    default: return -1;
    }
}

static void submit_motion(app_motion_field_t field, int value)
{
    if (!motion_settings_valid(&s_view.motion_settings) || !s_view.motion_settings.editable ||
        s_view.locked || s_view.screen_off || s_wake_touch || s_touch_local_blocked ||
        s_view.overlay != APP_OVERLAY_SETTINGS ||
        (s_view.settings_page != APP_SETTINGS_MOTION && s_view.settings_page != APP_SETTINGS_MOTION_ADVANCED) ||
        s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
        s_touch_settings_revision != s_view.motion_settings.revision) return;
    int minimum = field <= APP_MOTION_FIELD_GAIN_Y ? 25 : 0;
    int maximum = field <= APP_MOTION_FIELD_DEADZONE ? 10000 :
                  field <= APP_MOTION_FIELD_FREEZE ? 1000 : 1;
    if ((unsigned)field > APP_MOTION_FIELD_INVERT_Y || value < minimum || value > maximum) return;
    app_intent_t intent = {
        .action = APP_MOTION_CONFIG, .epoch = s_touch_epoch, .ui_revision = s_touch_revision,
        .gesture = s_gesture, .surface = APP_SURFACE_CONTROL,
        .motion_patch = {.field = field, .expected_settings_revision = s_touch_settings_revision}
    };
    if (field <= APP_MOTION_FIELD_GAIN_Y) intent.motion_patch.value.gain_centi = (uint16_t)value;
    else if (field == APP_MOTION_FIELD_DEADZONE) intent.motion_patch.value.deadzone_mdps = (uint16_t)value;
    else if (field <= APP_MOTION_FIELD_FREEZE) intent.motion_patch.value.milliseconds = (uint16_t)value;
    else intent.motion_patch.value.inverted = value != 0;
    if ((!s_submit || !s_submit(&intent)) && s_status) label_text(s_status, "Input busy. Please try again.");
}

static void on_action(lv_event_t *event)
{
    binding_t *binding = lv_event_get_user_data(event);
    if (s_view.locked || s_view.screen_off || s_wake_touch || (s_view.overlay != APP_OVERLAY_NONE && !in_overlay(binding->object))) return;
    if (binding->action == APP_SCROLL && !s_wheel.tap) return;
    if ((binding->action == APP_NAV_HOME || binding->action == APP_NAV_BACK) &&
        (s_view.mode != INPUT_MOUSE || !s_view.navigation_enabled ||
         !s_view.navigation_available || s_touch_local_blocked ||
         s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision)) return;
    if (binding->action == APP_CONFIRM && s_view.confirmation == APP_CONFIRM_REMOTE) {
        remote_review_action(binding->object, binding->value != 0); return;
    }
    if (binding->action == APP_API_COMMISSIONING || binding->action == APP_WIFI_PROFILE_CONTROL ||
        binding->action == APP_BLUETOOTH_TARGET_CONTROL || binding->action == APP_API_INPUT_ALLOW_SET) { connection_action(binding->object); return; }
    if (binding->action == APP_MEDIA_CONTROL || binding->object == s_voice_confirm ||
        (binding->action == APP_CONFIRM && binding->value == 1 &&
         ((s_view.confirmation >= APP_CONFIRM_RECORDING_START && s_view.confirmation <= APP_CONFIRM_RECORDING_CANCEL_DISCARD) ||
          s_view.confirmation == APP_CONFIRM_AUDIO_PAIR))) {
        media_action(binding->object, -1);
        return;
    }
    int value = binding->value;
    if (binding->object == s_bond_disconnect) {
        if (s_touch_local_blocked || s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
            s_touch_bluetooth_connected != s_view.bluetooth_connected ||
            s_touch_bluetooth_paused != s_view.bluetooth_reconnect_paused) return;
        value = s_touch_bluetooth_connected ? APP_PAIR_DISCONNECT : APP_PAIR_RESUME;
    }
    if (binding->action == APP_RECORDING_STORAGE_PREPARE_REQUEST ||
        (binding->action == APP_CONFIRM && value == 1 && s_view.confirmation == APP_CONFIRM_RECORDING_STORAGE_PREPARE)) {
        if (!recording_storage_can_prepare(&s_view) || s_touch_local_blocked ||
            s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
            s_touch_recording_storage_generation != s_view.recording_storage.generation ||
            s_touch_recording_storage_revision != s_view.recording_storage.revision) return;
    }
    if (binding->action == APP_MOTION_CONFIG) {
        if (binding->motion_field >= APP_MOTION_FIELD_INVERT_X) value = !motion_setting(binding->motion_field);
        else if (binding->motion_delta) {
            value += motion_setting(binding->motion_field);
            int minimum = binding->motion_field <= APP_MOTION_FIELD_GAIN_Y ? 25 : 0;
            int maximum = binding->motion_field <= APP_MOTION_FIELD_DEADZONE ? 10000 : 1000;
            if (value < minimum) value = minimum;
            if (value > maximum) value = maximum;
        }
        submit_motion(binding->motion_field, value);
        return;
    }
    switch (binding->action) {
    case APP_MOTION: value = !s_view.motion_enabled; break;
    case APP_DRAG: value = !s_view.dragging; break;
    case APP_GAIN: value = s_view.gain_divisor == 1 ? 4 : s_view.gain_divisor == 4 ? 8 : 1; break;
    case APP_CASE: value = !s_view.lowercase; break;
    case APP_SYMBOLS: value = !s_view.symbols; break;
    case APP_HAPTICS: value = !s_view.haptics_enabled; break;
    case APP_TAP_TO_CLICK: value = !s_view.tap_to_click_enabled; break;
    case APP_NAVIGATION: value = !s_view.navigation_enabled; break;
    case APP_AUDIO_POWER: value = !s_view.audio_active; break;
    case APP_AUDIO_MUTE: value = !s_view.microphone_muted; break;
    default: break;
    }
    app_action_t action = binding->action;
    if (binding->object == s_keyboard_choice) {
        if (s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision) return;
        if (s_view.mode == INPUT_MOUSE) {
            if (s_touch_local_blocked || !s_view.motion_recenter_available) return;
            action = APP_RECENTER;
            value = 0;
        }
    }
    if (action == APP_SETTINGS_PAGE && value == APP_SETTINGS_HOME && s_view.overlay == APP_OVERLAY_SETTINGS) {
        if (s_view.settings_page == APP_SETTINGS_HOME) { action = APP_MENU; value = APP_OVERLAY_MODES; }
        else if (s_view.settings_page == APP_SETTINGS_BLUETOOTH || s_view.settings_page == APP_SETTINGS_WIFI) value = APP_SETTINGS_CONNECTIVITY;
        else if (s_view.settings_page == APP_SETTINGS_MOTION || s_view.settings_page == APP_SETTINGS_GESTURES ||
                 s_view.settings_page == APP_SETTINGS_NAVIGATION) value = APP_SETTINGS_INPUT;
        else if (s_view.settings_page == APP_SETTINGS_MOTION_ADVANCED) value = APP_SETTINGS_MOTION;
    }
    if ((binding->object == s_critical || binding->object == s_overlay_critical) &&
        capture_warning_required(&s_view))
        action = APP_LOCK;
    if (action == APP_BLUETOOTH_PAGE) value += (int)s_view.bluetooth_page;
    if (action == APP_WIFI_SCAN_PAGE) value += (int)s_built_scan_page;
    if (binding->object == s_scan_refresh) value = s_view.wifi_scan.can_cancel ? 0 : 1;
    if (action == APP_SEND && s_view.sending) action = APP_STOP;
    if (action == APP_VOICE_START && s_view.voice_candidate) action = APP_VOICE_CONFIRM;
    bool accepted = submit_identity(action, value, 0, binding->wifi_generation, binding->wifi_record);
    if (accepted && s_view.overlay == APP_OVERLAY_LOCAL_EDITOR &&
        (action == APP_CLOSE || action == APP_MENU || action == APP_SETTINGS_PAGE || action == APP_LOCK)) {
        clear_editor();
        render_editor();
    }
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
    if (app_local_action_allowed(APP_LOCAL_PAUSED, action)) lv_obj_add_flag(object, UI_SAFETY_CONTROL);
    else if (app_local_action_allowed(APP_LOCAL_BROWSE, action)) lv_obj_add_flag(object, UI_BROWSE_CONTROL);
    enabled(object, true);
    lv_obj_add_event_cb(object, on_action, LV_EVENT_CLICKED, binding);
    return object;
}

static lv_obj_t *critical_control(lv_obj_t *parent)
{
    lv_obj_t *object = button(parent, "Status", 280, 8, 116, 48, APP_MENU, APP_OVERLAY_TARGET);
    lv_obj_set_style_border_width(object, 1, 0);
    border(object, WARNING, 0);
    lv_obj_t *caption = lv_obj_get_child(object, 0);
    lv_obj_set_style_text_font(caption, META_FONT, 0);
    lv_obj_set_height(caption, 36);
    lv_obj_center(caption);
    return object;
}

static void critical_state(lv_obj_t *object, bool alert, bool capture, bool provider_available)
{
    visible(object, alert);
    if (!alert) return;
    const app_view_t *view = &s_view;
    bool confirmation = view->overlay == APP_OVERLAY_CONFIRM;
    lv_obj_set_flag(object, UI_SAFETY_CONTROL, capture);
    enabled(object, !confirmation || capture || view->remote_owned);
    const char *caption = capture ? capture_warning(view, true) :
        view->remote_owned ? "Remote\nTake local" :
        view->local_access == APP_LOCAL_BROWSE ? "Boot check\nBrowse only" :
        view->local_access == APP_LOCAL_PAUSED ? "Maintenance\nInput paused" :
        view->release_pending ? "Releasing\nDetails" :
        view->transport_error ? "Input error\nDetails" :
        view->transport_switch_pending ? "Switching\nDetails" :
        !provider_available ? "Unavailable\nDetails" :
        !view->connected ? "Disconnected\nDetails" : "Connecting\nDetails";
    if (confirmation && !capture && !view->remote_owned)
        caption = view->transport_error ? "Input error" : view->release_pending ? "Releasing" : "Input not ready";
    text(object, caption);
    bool error = view->transport_error || (capture && view->capture_state == APP_CAPTURE_UNCERTAIN);
    foreground(object, error ? ERROR : WARNING, 0);
    border(object, error ? ERROR : WARNING, 0);
}

/* The existing arrows own taps. A gap press belongs to the mouse panel.
 * LVGL's press lock retains that original target throughout the contact. */
static void begin_wheel(lv_obj_t *target, lv_point_t point)
{
    s_wheel.tracking = s_wheel.tap = false;
    bool arrow = target == s_mouse_controls[0] || target == s_mouse_controls[1];
    if (s_wheel.wait_all_up || (!arrow && target != s_panels[INPUT_MOUSE]) || s_view.mode != INPUT_MOUSE ||
        s_view.overlay != APP_OVERLAY_NONE || s_wake_touch || s_touch_local_blocked ||
        lv_obj_has_state(s_mouse_controls[0], LV_STATE_DISABLED)) return;
    lv_area_t lower;
    lv_obj_get_coords(s_mouse_controls[0], &s_wheel.bounds);
    lv_obj_get_coords(s_mouse_controls[1], &lower);
    s_wheel.bounds.y2 = lower.y2;
    if (point.x < s_wheel.bounds.x1 || point.x > s_wheel.bounds.x2 ||
        point.y < s_wheel.bounds.y1 || point.y > s_wheel.bounds.y2) return;
    s_wheel.tracking = true;
    s_wheel.tap = arrow;
    s_wheel.anchor_y = point.y;
}

static void track_wheel(lv_point_t point)
{
    if (point.x < s_wheel.bounds.x1 || point.x > s_wheel.bounds.x2 ||
        point.y < s_wheel.bounds.y1 || point.y > s_wheel.bounds.y2) {
        s_wheel.tracking = s_wheel.tap = false;
        return;
    }
    if (LV_ABS(point.x - s_last_point.x) >= WHEEL_TAP_SLOP_PX ||
        LV_ABS(point.y - s_last_point.y) >= WHEEL_TAP_SLOP_PX ||
        (s_wheel.tap && !lv_obj_hit_test(s_touch_target, &point))) s_wheel.tap = false;
    int steps = (s_wheel.anchor_y - point.y) / WHEEL_STEP_PX;
    if (steps) {
        /* Consume this sample even on queue rejection. A stationary finger
         * must never replay a rejected wheel report. */
        s_wheel.anchor_y -= steps * WHEEL_STEP_PX;
        (void)submit(APP_SCROLL, steps, 0);
    }
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
        s_touch_bluetooth_connected = s_view.bluetooth_connected;
        s_touch_bluetooth_paused = s_view.bluetooth_reconnect_paused;
        s_touch_epoch = s_view.epoch;
        s_touch_revision = s_view.ui_revision;
        s_touch_settings_revision = s_view.motion_settings.revision;
        s_touch_recording_storage_generation = s_view.recording_storage.generation;
        s_touch_recording_storage_revision = s_view.recording_storage.revision;
        s_touch_editor_session = s_view.local_editor.session;
        media_press(target);
        connection_press(target);
        remote_review_press(target);
        s_touch_active = true;
        s_touch_target = target;
        s_touch_device = device;
        s_wake_touch = s_view.screen_off;
        s_touch_local_blocked = s_view.remote_owned || s_view.locked;
        s_touch_surface = target == s_pad ? APP_SURFACE_TOUCHPAD : APP_SURFACE_CONTROL;
        s_last_point = point;
        s_last_touch_heartbeat = lv_tick_get();
        begin_wheel(target, point);
        if (!submit(APP_TOUCH_BEGIN, point.x, point.y)) s_wheel.tracking = s_wheel.tap = false;
    } else if (code == LV_EVENT_PRESSING && s_touch_active && s_wheel.tracking) {
        track_wheel(point);
    } else if (code == LV_EVENT_PRESSING && s_touch_active &&
               s_touch_surface == APP_SURFACE_TOUCHPAD && !s_view.locked &&
               !s_view.screen_off && !s_wake_touch &&
               s_view.overlay == APP_OVERLAY_NONE && s_view.mode == INPUT_MOUSE &&
               s_touch_revision == s_view.ui_revision) {
        int dx = point.x - s_last_point.x;
        int dy = point.y - s_last_point.y;
        s_last_point = point;
        /* A stationary held finger is still a live physical contact. The
         * coordinator/recognizer needs bounded freshness for safe drag holds. */
        if (dx || dy || lv_tick_elaps(s_last_touch_heartbeat) >= 50) {
            (void)submit(APP_TOUCH_MOVE, dx, dy);
            s_last_touch_heartbeat = lv_tick_get();
        }
    } else if (s_touch_active && (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {
        if (s_wheel.tracking && code == LV_EVENT_RELEASED) track_wheel(point);
        if (code == LV_EVENT_PRESS_LOST) s_wheel.tap = false;
        s_wheel.tracking = false;
        (void)submit(code == LV_EVENT_RELEASED ? APP_TOUCH_END : APP_TOUCH_CANCEL, point.x, point.y);
        s_touch_active = false;
        s_touch_target = NULL;
        s_touch_device = NULL;
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
    media_action(lv_event_get_target(event), lv_slider_get_value(lv_event_get_target(event)));
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

static void on_motion_slider(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target(event);
    for (unsigned i = 0; i < 5; ++i) {
        if (object == s_motion_sliders[i]) {
            submit_motion((app_motion_field_t)i, lv_slider_get_value(object));
            return;
        }
    }
}

static lv_obj_t *motion_button(lv_obj_t *parent, const char *caption, int x, int y,
                              int width, app_motion_field_t field, int value, bool delta)
{
    lv_obj_t *object = button(parent, caption, x, y, width, 44, APP_MOTION_CONFIG, value);
    s_bindings[s_binding_count - 1].motion_field = field;
    s_bindings[s_binding_count - 1].motion_delta = delta;
    return object;
}

static void motion_adjustment(lv_obj_t *parent, app_motion_field_t field,
                              const char *caption, int y, int step)
{
    label(parent, caption, 0, y, 260, INK);
    s_motion_values[field] = label(parent, "Unavailable", 264, y, 184, MUTED);
    lv_obj_set_style_text_align(s_motion_values[field], LV_TEXT_ALIGN_RIGHT, 0);
    single_line(s_motion_values[field]);
    s_motion_nudges[field][0] = motion_button(parent, "-", 0, y + 22, 56, field, -step, true);
    s_motion_nudges[field][1] = motion_button(parent, "+", 392, y + 22, 56, field, step, true);
    s_motion_sliders[field] = slider(parent, 84, y + 38, 280,
        field <= APP_MOTION_FIELD_GAIN_Y ? 25 : 0,
        field <= APP_MOTION_FIELD_DEADZONE ? 10000 : 1000, on_motion_slider);
}

static const char *motion_reason(const app_view_t *view)
{
    switch (view->motion_reason) {
    case APP_MOTION_READY:
        if (!view->motion_enabled) return "Enable Motion to set your grip.";
        switch (view->motion_settings.reference) {
        case APP_REFERENCE_READY: return "Grip ready. Touch pauses motion.";
        case APP_REFERENCE_ACQUIRING: return "Hold still to set your grip.";
        case APP_REFERENCE_UNSTABLE: return "Hold still to restore your grip.";
        case APP_REFERENCE_UNAVAILABLE: return "Grip unavailable. Open Status.";
        default: return "Grip status unavailable. Open Status.";
        }
    case APP_MOTION_SENSOR_UNAVAILABLE: return "Motion sensor unavailable.";
    case APP_MOTION_CALIBRATION_REQUIRED: return "Calibrate sensor in Input settings.";
    case APP_MOTION_CALIBRATING: return "Keep the device still to calibrate sensor.";
    case APP_MOTION_PROVIDER_UNAVAILABLE: return "Selected input provider unavailable.";
    case APP_MOTION_DISCONNECTED: return view->input_transport == APP_INPUT_USB ?
        "Connect a USB data host." : "Connect a Bluetooth input host.";
    case APP_MOTION_INPUT_NOT_READY: return "Host connected; input is not ready.";
    case APP_MOTION_RELEASING: return "Waiting for held input to release.";
    case APP_MOTION_TRANSPORT_SWITCHING: return "Switching input connection.";
    case APP_MOTION_TRANSPORT_ERROR: return "Input connection error. Open Status.";
    default: return "Motion status unavailable. Open Status.";
    }
}

static const char *motion_caption(const app_view_t *view)
{
    if (view->calibrating) return "Calibrating...";
    if (!view->motion_enabled) return "Motion / off";
    if (view->motion_reason != APP_MOTION_READY) return "Motion / paused";
    switch (view->motion_settings.reference) {
    case APP_REFERENCE_READY: return "Motion / on";
    case APP_REFERENCE_ACQUIRING: case APP_REFERENCE_UNSTABLE: return "Motion / waiting";
    default: return "Motion / unavailable";
    }
}

static bool name_bytes_valid(const char *name, size_t length)
{
    if (!length || length >= APP_UI_BLUETOOTH_NAME_CAPACITY) return false;
    for (size_t i = 0; i < length;) {
        unsigned char first = (unsigned char)name[i++];
        uint32_t code = first;
        unsigned trailing = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { trailing = 1; code &= 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { trailing = 2; code &= 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { trailing = 3; code &= 7; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (trailing > length - i) return false;
        while (trailing--) {
            unsigned char byte = (unsigned char)name[i++];
            if ((byte & 0xc0) != 0x80) return false;
            code = (code << 6) | (byte & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
            code < 0x20 || (code >= 0x7f && code <= 0x9f)) return false;
    }
    return true;
}

static bool name_valid(const char *name, size_t length)
{
    return name_bytes_valid(name, length) && name[0] != ' ' && name[length - 1] != ' ';
}

/* The bundled font is ASCII-oriented. Escaping unsupported UTF-8 bytes is a
 * display operation only; the name draft retains its original bytes. */
static void name_label(char *output, size_t capacity, const char *name)
{
    size_t out = 0;
    for (size_t i = 0; i < APP_UI_BLUETOOTH_NAME_CAPACITY - 1 && name[i]; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (byte >= 0x20 && byte < 0x7f) {
            if (out + 1 >= capacity) break;
            output[out++] = (char)byte;
        } else {
            if (out + 4 >= capacity) break;
            (void)snprintf(output + out, capacity - out, "\\x%02X", byte);
            out += 4;
        }
    }
    output[out] = '\0';
}

static char *editor_buffer(void)
{
    return s_editor_purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE ? s_wifi_password : s_bluetooth_name;
}

static size_t editor_capacity(void)
{
    return s_editor_purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE ? sizeof(s_wifi_password) : sizeof(s_bluetooth_name);
}

static bool editor_live(void)
{
    return !s_view.locked && !s_view.screen_off && s_view.overlay == APP_OVERLAY_LOCAL_EDITOR &&
        s_editor_session && s_editor_session == s_view.local_editor.session &&
        s_editor_purpose == s_view.local_editor.purpose && s_view.local_editor.seconds_left &&
        !s_editor_consumed;
}

static const char *editor_group(unsigned index)
{
    static const char *const letters[] = {"ABC", "DEF", "GHI", "JKL", "MNO", "PQR", "STU", "VWX", "YZ"};
    static const char *const symbols[] = {"12345", "67890", ".,!?", "@#$%", "^&*()", "-_=+", "[]{}", ";:'\"", "/\\|<>`~"};
    return index < 9 ? (s_editor_symbols ? symbols[index] : letters[index]) : "";
}

static char editor_character(char value)
{
    return s_editor_lowercase && value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static void editor_append(char character)
{
    char *buffer = editor_buffer();
    size_t length = bounded_length(buffer, editor_capacity());
    if (length + 1 >= editor_capacity()) { s_editor_feedback = "Byte limit reached."; return; }
    buffer[length] = character;
    buffer[length + 1] = '\0';
}

static void on_editor_action(lv_event_t *event)
{
    if (!editor_live() || s_wake_touch || s_touch_local_blocked || s_view.remote_owned || s_touch_editor_session != s_editor_session ||
        s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision) return;
    unsigned operation = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    char *buffer = editor_buffer();
    size_t length = bounded_length(buffer, editor_capacity());
    s_editor_feedback = NULL;
    if (operation < 9) {
        if (s_editor_group < 0) s_editor_group = (int)operation;
        else {
            const char *group = editor_group((unsigned)s_editor_group);
            if (operation < strlen(group)) editor_append(editor_character(group[operation]));
            s_editor_group = -1;
        }
    } else if (operation == 9) s_editor_group = -1;
    else if (operation == 10) s_editor_lowercase = !s_editor_lowercase;
    else if (operation == 11) { s_editor_symbols = !s_editor_symbols; s_editor_group = -1; }
    else if (operation == 12 && length) {
        size_t end = length - 1;
        if (s_editor_purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME)
            while (end && ((unsigned char)buffer[end] & 0xc0) == 0x80) --end;
        secure_clear(buffer + end, editor_capacity() - end);
    } else if (operation == 13) editor_append(' ');
    else if (operation == 14) {
        bool valid = s_editor_purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE ?
            length >= 8 && length <= 63 : name_valid(buffer, length);
        if (!valid || !s_view.local_editor.submit_available || !s_local_submit) return;
        app_local_edit_request_t request = {
            .purpose = s_editor_purpose, .epoch = s_touch_epoch,
            .ui_revision = s_touch_revision, .editor_session = s_editor_session,
            .gesture = s_gesture
        };
        if (s_editor_purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE) {
            request.value.wifi.scan_generation = s_view.local_editor.wifi_scan_generation;
            request.value.wifi.record_index = s_view.local_editor.wifi_record_index;
            request.value.wifi.remember = s_view.local_editor.wifi_remember;
            request.value.wifi.length = length;
            memcpy(request.value.wifi.password, buffer, length + 1);
        } else {
            request.value.bluetooth.length = length;
            memcpy(request.value.bluetooth.name, buffer, length + 1);
        }
        app_local_edit_result_t result = s_local_submit(&request);
        secure_clear(&request, sizeof(request));
        if (result == APP_LOCAL_EDIT_ACCEPTED || result == APP_LOCAL_EDIT_STALE) {
            clear_editor();
            s_editor_feedback = result == APP_LOCAL_EDIT_ACCEPTED ? "Submitted / waiting for result." : "Editor expired. Open a new editor.";
        } else s_editor_feedback = result == APP_LOCAL_EDIT_BUSY ? "Input busy. Try again." :
            result == APP_LOCAL_EDIT_INVALID ? "Entry rejected. Check the value." : "Submission unavailable.";
    }
    render_editor();
}

static lv_obj_t *editor_button(lv_obj_t *parent, const char *caption, int x, int y,
                               int width, int height, unsigned operation)
{
    lv_obj_t *object = button(parent, caption, x, y, width, height, APP_CLOSE, 0);
    lv_obj_remove_event_cb(object, on_action);
    lv_obj_add_event_cb(object, on_editor_action, LV_EVENT_CLICKED, (void *)(uintptr_t)operation);
    return object;
}

static void render_editor(void)
{
    if (!s_editor_preview) return;
    bool wifi = s_editor_purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE;
    bool live = editor_live();
    size_t length = bounded_length(editor_buffer(), editor_capacity());
    char preview[APP_UI_BLUETOOTH_NAME_CAPACITY * 4];
    if (wifi) {
        size_t masks = length < 24 ? length : 24;
        memset(preview, '*', masks);
        preview[masks] = '\0';
        if (length > masks) memcpy(preview + masks, "...", 4);
    } else name_label(preview, sizeof(preview), s_bluetooth_name);
    label_text(s_editor_preview, *preview ? preview : wifi ? "Password hidden" : "Enter device name");
    if (wifi) label_text(s_editor_source, s_view.wifi_selection.network.label);
    else {
        char effective[APP_UI_BLUETOOTH_NAME_CAPACITY * 4];
        name_label(effective, sizeof(effective), s_view.bluetooth_name.effective);
        label_format(s_editor_source, "Effective device name: %s",
            s_view.bluetooth_name.effective_valid ? effective : "unavailable");
    }
    label_format(s_editor_context, wifi ? "%u / 63 bytes / %s" : "%u / 18 bytes / %s", (unsigned)length,
        wifi ? s_view.local_editor.wifi_remember ? "Remember and join" : "Join once" : "This device / not the host");
    label_text(s_editor_status, s_editor_feedback ? s_editor_feedback : *s_view.local_editor.message ?
        s_view.local_editor.message : !live ? "Editor unavailable / draft cleared" :
        !s_local_submit || !s_view.local_editor.submit_available ? "Submission unavailable. Close and retry." :
        wifi ? "8-63 ASCII bytes / never sent as keyboard input" : "1-18 UTF-8 bytes / no outer spaces");
    for (unsigned i = 0; i < 9; ++i) {
        char cell[10] = {0};
        const char *group = editor_group(s_editor_group < 0 ? i : (unsigned)s_editor_group);
        if (s_editor_group < 0) {
            for (size_t j = 0; j < sizeof(cell) - 1 && group[j]; ++j) cell[j] = editor_character(group[j]);
        } else if (i < strlen(group)) cell[0] = editor_character(group[i]);
        text(s_editor_grid[i], cell);
        visible(s_editor_grid[i], *cell != '\0');
        enabled(s_editor_grid[i], live && *cell);
    }
    text(s_editor_case, s_editor_lowercase ? "abc" : "ABC");
    text(s_editor_symbol, s_editor_symbols ? "Letters" : "Symbols");
    text(s_editor_apply, wifi ? "Join" : "Apply");
    enabled(s_editor_back, live && s_editor_group >= 0);
    enabled(s_editor_case, live && !s_editor_symbols);
    enabled(s_editor_symbol, live);
    enabled(s_editor_delete, live && length);
    enabled(s_editor_space, live && length + 1 < editor_capacity());
    enabled(s_editor_apply, live && s_local_submit && s_view.local_editor.submit_available &&
        (wifi ? length >= 8 && length <= 63 : name_valid(s_bluetooth_name, length)));
}

static void reconcile_editor(const app_view_t *view)
{
    const app_local_editor_view_t *editor = &view->local_editor;
    bool valid = !view->locked && !view->screen_off && view->overlay == APP_OVERLAY_LOCAL_EDITOR &&
        editor->session && editor->seconds_left && (editor->purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE ||
        editor->purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME);
    if (!valid) {
        clear_editor();
        return;
    }
    if (s_editor_session == editor->session && s_editor_purpose == editor->purpose) return;
    clear_editor();
    s_editor_session = editor->session;
    s_editor_purpose = editor->purpose;
    s_editor_consumed = false;
    if (editor->purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME) {
        size_t length = bounded_length(editor->bluetooth_initial, sizeof(editor->bluetooth_initial));
        /* Preserve a bounded UTF-8 draft for repair; outer spaces still block
         * Apply until the user removes them. Never truncate malformed input. */
        if (name_bytes_valid(editor->bluetooth_initial, length)) memcpy(s_bluetooth_name, editor->bluetooth_initial, length + 1);
    }
}

#include "app_ui_media.inc"
#include "app_ui_remote_review.inc"
#include "app_ui_connections.inc"

static void create_mouse(lv_obj_t *panel)
{
    s_pad = box(panel, 0, 0, 396, 292, SOFT);
    lv_obj_set_style_radius(s_pad, 16, 0);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *title = heading(s_pad, "Touchpad", 16, 112, 364);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    s_pad_hint = label(s_pad, "Motion status unavailable", 16, 148, 364, MUTED);
    lv_obj_set_style_text_font(s_pad_hint, META_FONT, 0);
    lv_obj_set_height(s_pad_hint, 80);
    lv_obj_set_style_text_align(s_pad_hint, LV_TEXT_ALIGN_CENTER, 0);
    s_mouse_controls[0] = button(panel, LV_SYMBOL_UP, 404, 0, 52, 142, APP_SCROLL, 1);
    s_mouse_controls[1] = button(panel, LV_SYMBOL_DOWN, 404, 150, 52, 142, APP_SCROLL, -1);
    s_mouse_controls[2] = button(panel, "Left click", 0, 304, 144, 48, APP_CLICK, 1);
    s_mouse_controls[3] = button(panel, "Right click", 156, 304, 144, 48, APP_CLICK, 2);
    s_drag = button(panel, "Drag", 312, 304, 144, 48, APP_DRAG, 0);
    s_motion = button(panel, "Motion / off", 0, 360, 224, 48, APP_MOTION, 0);
    s_gain = button(panel, "Fine gain / 1x", 232, 360, 224, 48, APP_GAIN, 0);
}

static void create_keyboard(lv_obj_t *panel)
{
    lv_obj_t *preview = box(panel, 0, 0, 456, 64, SOFT);
    lv_obj_set_style_radius(preview, 12, 0);
    s_preview = label(preview, "Ready for input...", 12, 8, 332, INK);
    single_line(s_preview);
    s_progress = metadata(preview, "0 / 64", 12, 38, 332);
    s_clear = button(preview, "Clear", 360, 8, 88, 48, APP_CLEAR, 0);
    s_grid_page = box(panel, 0, 76, 456, 244, SURFACE);
    s_back = button(s_grid_page, "Groups", 360, 0, 96, 76, APP_BACK, 0);
    s_case = button(s_grid_page, "ABC", 360, 84, 96, 76, APP_CASE, 0);
    s_symbols = button(s_grid_page, "Symbols", 360, 168, 96, 76, APP_SYMBOLS, 0);
    for (int i = 0; i < APP_UI_GRID_CELLS; ++i) {
        s_grid[i] = button(s_grid_page, "", (i % 3) * 120, (i / 3) * 84,
                            112, 76, APP_GRID, i);
        lv_obj_set_style_border_width(s_grid[i], 2, 0);
        border(s_grid[i], SOFT, 0);
    }
    s_voice_page = box(panel, 0, 76, 456, 244, SOFT);
    lv_obj_set_style_radius(s_voice_page, 12, 0);
    heading(s_voice_page, "Spoken letters", 16, 24, 424);
    s_voice_candidate = heading(s_voice_page, "A-Z", 16, 100, 82);
    s_voice_hint = label(s_voice_page, "Unavailable", 112, 82, 328, MUTED);
    s_voice_confirm = button(s_voice_page, "Start listening", 112, 176, 208, 48, APP_MEDIA_CONTROL, 0);
    s_voice_reject = button(s_voice_page, "Reject", 328, 176, 112, 48, APP_MEDIA_CONTROL, 0);
    s_shortcut_page = box(panel, 0, 76, 456, 244, SURFACE);
    const char *shortcuts[] = {"Copy", "Paste", "Undo", "Tab", "Esc", "Enter"};
    for (int i = 0; i < 6; ++i) {
        s_shortcuts[i] = button(s_shortcut_page, shortcuts[i], (i % 3) * 156,
                                (i / 3) * 128, 144, 116, APP_SHORTCUT, i);
    }
    s_status = metadata(panel, "Local control / preview first / no Enter", 0, 328, 456);
    s_delete = button(panel, "Delete draft", 0, 360, 144, 48, APP_DELETE, 0);
    s_space = button(panel, "Space", 156, 360, 144, 48, APP_SPACE, 0);
    s_send = button(panel, "Send", 312, 360, 144, 48, APP_SEND, 0);
}

static void create_audio(lv_obj_t *panel)
{
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *surface = box(panel, 0, (int)i * 180, 456, 168, SOFT);
        lv_obj_set_style_radius(surface, 16, 0);
        heading(surface, i ? "Speaker" : "Mic", 16, 12, 268);
        s_audio_state[i] = label(surface, "Status unknown", 16, 48, 424, MUTED);
        lv_obj_set_height(s_audio_state[i], 44);
        lv_label_set_long_mode(s_audio_state[i], LV_LABEL_LONG_DOT);
        s_audio_power[i] = button(surface, i ? "Start speaker" : "Start mic", 304, 16, 136, 48, APP_MEDIA_CONTROL, 0);
        lv_obj_set_style_border_width(s_audio_power[i], 1, 0);
        border(s_audio_power[i], LINE, 0);
        if (!i) {
            s_audio_mute = button(surface, "Mute mic", 16, 104, 224, 48, APP_MEDIA_CONTROL, 0);
            lv_obj_set_style_border_width(s_audio_mute, 1, 0);
            border(s_audio_mute, LINE, 0);
            s_mic_level_label = lv_label_create(surface);
            lv_obj_set_pos(s_mic_level_label, 256, 106);
            lv_obj_set_size(s_mic_level_label, 184, 20);
            lv_label_set_long_mode(s_mic_level_label, LV_LABEL_LONG_CLIP);
            lv_obj_remove_flag(s_mic_level_label, LV_OBJ_FLAG_CLICKABLE);
            s_mic_level_text[0] = '\0';
            lv_label_set_text_static(s_mic_level_label, s_mic_level_text);
            foreground(s_mic_level_label, MUTED, 0);
            s_mic_level = lv_bar_create(surface);
            lv_obj_remove_style_all(s_mic_level);
            lv_obj_set_pos(s_mic_level, 256, 136);
            lv_obj_set_size(s_mic_level, 184, 12);
            lv_obj_remove_flag(s_mic_level, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_bar_set_range(s_mic_level, 0, 100);
            lv_obj_set_style_bg_opa(s_mic_level, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(s_mic_level, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(s_mic_level, 6, 0);
            lv_obj_set_style_radius(s_mic_level, 6, LV_PART_INDICATOR);
            background(s_mic_level, LINE, 0);
            background(s_mic_level, ACCENT, LV_PART_INDICATOR);
        }
        else {
            label(surface, "Volume", 16, 94, 268, INK);
            s_volume_text = label(surface, "--", 376, 94, 64, MUTED);
            s_volume = slider(surface, 24, 132, 408, 0, 100, on_volume);
        }
    }
    s_audio_note = label(panel, "Audio unavailable", 0, 360, 456, MUTED);
    lv_obj_set_height(s_audio_note, 48);
    lv_label_set_long_mode(s_audio_note, LV_LABEL_LONG_DOT);
}

static lv_obj_t *card(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *object = box(parent, 0, y, 448, height, SOFT);
    lv_obj_set_style_radius(object, 12, 0);
    return object;
}

static void create_settings(lv_obj_t *settings, app_settings_page_t page)
{
    s_settings_back = button(settings, "Back", 0, 360, 456, 48, APP_SETTINGS_PAGE, APP_SETTINGS_HOME);
    s_save_status = metadata(settings, "Settings status unavailable", 0, 336, 456);
    if (page == APP_SETTINGS_HOME) {
        const char *names[] = {"Connectivity", "Display", "Input", "About"};
        for (int i = 0; i < 4; ++i)
            button(settings, names[i], 0, i * 80, 456, 68, APP_SETTINGS_PAGE, i + 1);
        return;
    }
    if (page == APP_SETTINGS_CONNECTIVITY) {
        lv_obj_t *connectivity = settings;
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
        return;
    }
    if (page == APP_SETTINGS_WIFI) {
        lv_obj_t *wifi = settings;
        s_wifi_state = label(wifi, "Unavailable", 0, 0, 448, INK);
        single_line(s_wifi_state);
        s_wifi_ssid = label(wifi, "No network", 0, 28, 448, INK);
        lv_obj_set_height(s_wifi_ssid, 36);
        lv_label_set_long_mode(s_wifi_ssid, LV_LABEL_LONG_DOT);
        s_wifi_detail = metadata(wifi, "No active network", 0, 72, 448);
        lv_obj_t *saved_nav = button(wifi, "Saved networks", 0, 96, 448, 44, APP_MENU, APP_OVERLAY_WIFI_PROFILES);
        s_wifi_saved = lv_obj_get_child(saved_nav, 0);
        lv_obj_set_height(s_wifi_saved, 36);
        lv_label_set_long_mode(s_wifi_saved, LV_LABEL_LONG_DOT);
        lv_obj_t *api_nav = button(wifi, "Web/API", 0, 144, 448, 44, APP_MENU, APP_OVERLAY_API_CONNECTION);
        s_wifi_backend = lv_obj_get_child(api_nav, 0);
        lv_obj_set_height(s_wifi_backend, 40);
        lv_label_set_long_mode(s_wifi_backend, LV_LABEL_LONG_DOT);
        s_wifi_note = label(wifi, "", 0, 192, 448, MUTED);
        lv_obj_set_style_text_font(s_wifi_note, META_FONT, 0);
        lv_obj_set_height(s_wifi_note, 32);
        s_wifi_find = button(wifi, "Find networks", 0, 232, 448, 44, APP_WIFI_SCAN, 1);
        s_wifi_setup = button(wifi, "Recovery hotspot", 0, 284, 220, 44, APP_WIFI_SETUP, 1);
        s_wifi_forget = button(wifi, "Forget network", 228, 284, 220, 44, APP_WIFI_FORGET_REQUEST, 0);
        return;
    }
    if (page == APP_SETTINGS_BLUETOOTH) {
        lv_obj_t *bluetooth = settings;
        for (unsigned i = 0; i < APP_UI_BONDS_PER_PAGE; ++i) {
            lv_obj_t *row = button(bluetooth, "Host", 0, (int)i * 56, 456, 52, APP_BLUETOOTH_TARGET_CONTROL, 0);
            s_bond_rows[i] = row;
            s_bond_names[i] = lv_obj_get_child(row, 0);
            lv_obj_set_align(s_bond_names[i], LV_ALIGN_TOP_LEFT);
            lv_obj_set_pos(s_bond_names[i], 8, 0);
            lv_obj_set_width(s_bond_names[i], 296);
            lv_obj_set_style_text_align(s_bond_names[i], LV_TEXT_ALIGN_LEFT, 0);
            single_line(s_bond_names[i]);
            s_bond_ids[i] = metadata(row, "Identity unavailable", 0, 20, 304);
            s_bond_states[i] = label(row, "Saved", 320, 8, 128, MUTED);
            single_line(s_bond_states[i]);
            lv_obj_set_style_text_align(s_bond_states[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
        s_bond_empty = label(bluetooth, "Saved hosts unavailable", 0, 40, 448, MUTED);
        s_bond_previous = button(bluetooth, "Previous", 0, 228, 144, 44, APP_BLUETOOTH_PAGE, -1);
        s_bond_page = label(bluetooth, "1 / 1", 156, 232, 144, MUTED);
        lv_obj_set_style_text_font(s_bond_page, META_FONT, 0);
        lv_obj_set_height(s_bond_page, 36);
        lv_label_set_long_mode(s_bond_page, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_bond_page, LV_TEXT_ALIGN_CENTER, 0);
        s_bond_next = button(bluetooth, "Next", 312, 228, 144, 44, APP_BLUETOOTH_PAGE, 1);
        s_bond_add = button(bluetooth, "Add host", 0, 280, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_OPEN);
        s_pairing_buttons[0] = s_bond_add;
        s_pairing_buttons[1] = button(bluetooth, "Close pairing", 0, 280, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_CLOSE);
        s_bond_disconnect = button(bluetooth, "Disconnect", 156, 280, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
        s_pairing_buttons[2] = button(bluetooth, "Forget all", 312, 280, 144, 44, APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL);
        lv_obj_set_width(s_settings_back, 224);
        lv_obj_set_width(lv_obj_get_child(s_settings_back, 0), 216);
        s_name_edit = button(bluetooth, "Device name", 232, 360, 224, 48, APP_BLUETOOTH_NAME_EDIT, 0);
        return;
    }
    if (page == APP_SETTINGS_DISPLAY) {
        lv_obj_t *display = settings;
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
        return;
    }
    if (page == APP_SETTINGS_INPUT) {
        lv_obj_t *input = settings;
        label(input, "Host shortcut modifier", 0, 0, 448, INK);
        s_hosts[0] = button(input, "Ctrl / Windows", 0, 24, 220, 44, APP_HOST_OS, APP_HOST_CTRL);
        s_hosts[1] = button(input, "Command / Apple", 228, 24, 220, 44, APP_HOST_OS, APP_HOST_COMMAND);
        label(input, "Haptic feedback", 0, 88, 228, INK);
        s_haptics_text = label(input, "Off", 236, 88, 112, MUTED);
        single_line(s_haptics_text);
        s_haptics = lv_switch_create(input);
        lv_obj_remove_style_all(s_haptics);
        const lv_style_selector_t parts[] = {LV_PART_MAIN, LV_PART_INDICATOR, LV_PART_KNOB};
        for (unsigned i = 0; i < 3; ++i) {
            lv_obj_set_style_bg_opa(s_haptics, LV_OPA_COVER, parts[i]);
            lv_obj_set_style_radius(s_haptics, LV_RADIUS_CIRCLE, parts[i]);
        }
        lv_obj_set_style_pad_all(s_haptics, -4, LV_PART_KNOB);
        lv_obj_set_pos(s_haptics, 360, 76);
        lv_obj_set_size(s_haptics, 88, 44);
        background(s_haptics, LINE, LV_PART_MAIN);
        background(s_haptics, LINE, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(s_haptics, 0, 0);
        background(s_haptics, ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        background(s_haptics, ON_ACCENT, LV_PART_KNOB);
        lv_obj_add_flag(s_haptics, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(s_haptics, on_haptics, LV_EVENT_VALUE_CHANGED, NULL);
        s_motion_detail = label(input, "Motion status unavailable", 0, 112, 348, MUTED);
        lv_obj_set_style_text_font(s_motion_detail, META_FONT, 0);
        single_line(s_motion_detail);
        s_calibrate = button(input, "Calibrate sensor", 0, 132, 220, 44, APP_CALIBRATE, 0);
        s_reset_grip = button(input, "Reset grip", 228, 132, 220, 44, APP_RECENTER, 0);
        s_tap_click = button(input, "Tap to click / unavailable", 0, 184, 448, 44, APP_TAP_TO_CLICK, 0);
        button(input, "Motion settings", 0, 236, 220, 44, APP_SETTINGS_PAGE, APP_SETTINGS_MOTION);
        button(input, "Gesture help", 228, 236, 220, 44, APP_SETTINGS_PAGE, APP_SETTINGS_GESTURES);
        s_navigation = button(input, "Navigation / off", 0, 288, 448, 44,
            APP_SETTINGS_PAGE, APP_SETTINGS_NAVIGATION);
        return;
    }
    if (page == APP_SETTINGS_NAVIGATION) {
        s_navigation = button(settings, "Navigation / off", 0, 0, 456, 48,
            APP_NAVIGATION, 0);
        heading(settings, "Home / setup", 0, 64, 456);
        label(settings, "Enable AssistiveTouch: Settings > Accessibility > Touch. "
            "In AssistiveTouch > Devices, select this device and map Button 3 to Home.", 0, 100, 448, MUTED);
        label(settings, "During button setup, close this panel and tap Home on Mouse.", 0, 190, 448, MUTED);
        heading(settings, "Back / Command+[", 0, 252, 456);
        label(settings, "Only in apps that support this shortcut. "
            "No system-wide Back or touchscreen swipe.", 0, 288, 448, MUTED);
        return;
    }
    if (page == APP_SETTINGS_MOTION) {
        s_motion_reference = label(settings, "Auto grip unavailable", 0, 0, 448, MUTED);
        lv_obj_set_height(s_motion_reference, 36);
        lv_obj_set_style_text_font(s_motion_reference, META_FONT, 0);
        const char *presets[] = {"Slow", "Default", "Fast"};
        const int values[] = {200, 400, 800};
        for (unsigned axis = 0; axis < 2; ++axis) {
            int y = axis ? 160 : 40;
            motion_adjustment(settings, (app_motion_field_t)axis, axis ? "Vertical sensitivity" : "Horizontal sensitivity", y, 25);
            for (unsigned preset = 0; preset < 3; ++preset)
                s_motion_presets[axis][preset] = motion_button(settings, presets[preset], (int)preset * 152,
                    y + 70, 144, (app_motion_field_t)axis, values[preset], false);
        }
        button(settings, "Smoothing and direction", 0, 284, 448, 44,
               APP_SETTINGS_PAGE, APP_SETTINGS_MOTION_ADVANCED);
        return;
    }
    if (page == APP_SETTINGS_MOTION_ADVANCED) {
        motion_adjustment(settings, APP_MOTION_FIELD_DEADZONE, "Ignore small movements", 0, 100);
        motion_adjustment(settings, APP_MOTION_FIELD_FILTER, "Smoothing", 92, 10);
        motion_adjustment(settings, APP_MOTION_FIELD_FREEZE, "Pause after touch", 184, 25);
        s_motion_invert[0] = motion_button(settings, "Reverse horizontal", 0, 280, 220, APP_MOTION_FIELD_INVERT_X, 0, false);
        s_motion_invert[1] = motion_button(settings, "Reverse vertical", 228, 280, 220, APP_MOTION_FIELD_INVERT_Y, 0, false);
        return;
    }
    if (page == APP_SETTINGS_GESTURES) {
        s_gesture_drag = label(settings, "Drag status unavailable", 0, 0, 448, INK);
        lv_obj_set_height(s_gesture_drag, 36);
        s_gesture_help = label(settings, "Gesture information unavailable", 0, 48, 448, MUTED);
        lv_obj_set_height(s_gesture_help, 140);
        s_tap_click = button(settings, "Tap to click / unavailable", 0, 200, 448, 44, APP_TAP_TO_CLICK, 0);
        s_gesture_release = button(settings, "Cancel drag", 0, 256, 448, 44, APP_RELEASE, 0);
        metadata(settings, "Touch pauses Motion until release and a short pause.", 0, 308, 448);
        return;
    }
    if (page == APP_SETTINGS_ABOUT) {
        lv_obj_t *about = settings;
        const char *identity_names[] = {"Firmware", "Build artifact ID", "Board", "Device", "Developer", "Git commit"};
        for (unsigned i = 0; i < 6; ++i) {
            metadata(about, identity_names[i], 0, (int)i * 24 + 2, 128);
            s_about_identity[i] = label(about, "Unavailable", 136, (int)i * 24, 312, INK);
            single_line(s_about_identity[i]);
        }
        metadata(about, "Repository", 0, 148, 448);
        s_about_repository = label(about, "Unavailable", 0, 168, 448, INK);
        lv_obj_set_style_text_font(s_about_repository, META_FONT, 0);
        lv_label_set_long_mode(s_about_repository, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(s_about_repository, 68);
        s_about_battery = label(about, "Battery unavailable", 0, 248, 448, MUTED);
        lv_obj_set_height(s_about_battery, 40);
        s_about_uptime = metadata(about, "Uptime unavailable", 0, 296, 448);
        return;
    }
}

static void clear_panel_bindings(void)
{
    clear_media_panel();
    clear_connection_panel();
    clear_remote_review_panel();
    memset(s_scan_rows, 0, sizeof(s_scan_rows));
    memset(s_scan_meta, 0, sizeof(s_scan_meta));
    memset(s_network_join, 0, sizeof(s_network_join));
    memset(s_common_keys, 0, sizeof(s_common_keys));
    memset(s_editor_grid, 0, sizeof(s_editor_grid));
    s_scan_state = s_scan_empty = s_scan_previous = s_scan_next = s_scan_page = s_scan_refresh = NULL;
    s_network_name = s_network_detail = s_network_note = NULL;
    s_wifi_find = s_wifi_saved = s_wifi_backend = s_name_edit = s_tap_click = NULL;
    s_motion_detail = s_common_context = s_common_modifiers = NULL;
    s_navigation = s_reset_grip = NULL;
    memset(s_motion_values, 0, sizeof(s_motion_values));
    memset(s_motion_sliders, 0, sizeof(s_motion_sliders));
    memset(s_motion_nudges, 0, sizeof(s_motion_nudges));
    memset(s_motion_presets, 0, sizeof(s_motion_presets));
    memset(s_motion_invert, 0, sizeof(s_motion_invert));
    s_motion_reference = s_gesture_help = s_gesture_drag = s_gesture_release = NULL;
    s_record_store_state = s_record_store_profile = s_record_store_capacity = NULL;
    s_record_store_time = s_record_store_reason = s_record_store_prepare = NULL;
    s_editor_preview = s_editor_context = s_editor_source = s_editor_status = NULL;
    s_editor_back = s_editor_case = s_editor_symbol = s_editor_delete = s_editor_space = s_editor_apply = NULL;
    memset(&s_submodes, 0, sizeof(s_submodes));
    memset(&s_overlay_target, 0, sizeof(s_overlay_target));
    s_overlay_critical = NULL;
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    memset(&s_overlay_state, 0, sizeof(s_overlay_state));
    memset(&s_settings_title, 0, sizeof(s_settings_title));
    memset(&s_settings_back, 0, sizeof(s_settings_back));
    memset(&s_save_status, 0, sizeof(s_save_status));
    memset(&s_transports, 0, sizeof(s_transports));
    memset(&s_transport_status, 0, sizeof(s_transport_status));
    memset(&s_wifi_summary, 0, sizeof(s_wifi_summary));
    memset(&s_wifi_ssid, 0, sizeof(s_wifi_ssid));
    memset(&s_wifi_note, 0, sizeof(s_wifi_note));
    memset(&s_bluetooth_state, 0, sizeof(s_bluetooth_state));
    memset(&s_bluetooth_pairing, 0, sizeof(s_bluetooth_pairing));
    memset(&s_pairing_buttons, 0, sizeof(s_pairing_buttons));
    memset(&s_bond_names, 0, sizeof(s_bond_names));
    memset(&s_bond_ids, 0, sizeof(s_bond_ids));
    memset(&s_bond_states, 0, sizeof(s_bond_states));
    memset(&s_bond_rows, 0, sizeof(s_bond_rows));
    memset(&s_bond_empty, 0, sizeof(s_bond_empty));
    memset(&s_bond_page, 0, sizeof(s_bond_page));
    memset(&s_bond_previous, 0, sizeof(s_bond_previous));
    memset(&s_bond_next, 0, sizeof(s_bond_next));
    memset(&s_bond_add, 0, sizeof(s_bond_add));
    memset(&s_bond_disconnect, 0, sizeof(s_bond_disconnect));
    memset(&s_wifi_state, 0, sizeof(s_wifi_state));
    memset(&s_wifi_detail, 0, sizeof(s_wifi_detail));
    memset(&s_wifi_setup, 0, sizeof(s_wifi_setup));
    memset(&s_wifi_forget, 0, sizeof(s_wifi_forget));
    memset(&s_brightness, 0, sizeof(s_brightness));
    memset(&s_brightness_text, 0, sizeof(s_brightness_text));
    memset(&s_timeouts, 0, sizeof(s_timeouts));
    memset(&s_about_identity, 0, sizeof(s_about_identity));
    s_about_repository = NULL;
    memset(&s_about_battery, 0, sizeof(s_about_battery));
    memset(&s_about_uptime, 0, sizeof(s_about_uptime));
    memset(&s_setup_state, 0, sizeof(s_setup_state));
    memset(&s_setup_ssid, 0, sizeof(s_setup_ssid));
    memset(&s_setup_passcode, 0, sizeof(s_setup_passcode));
    memset(&s_setup_url, 0, sizeof(s_setup_url));
    memset(&s_setup_stop, 0, sizeof(s_setup_stop));
    memset(&s_confirm_title, 0, sizeof(s_confirm_title));
    memset(&s_confirm_detail, 0, sizeof(s_confirm_detail));
    memset(&s_confirm_accept, 0, sizeof(s_confirm_accept));
    memset(&s_target_detail, 0, sizeof(s_target_detail));
    memset(&s_target_link, 0, sizeof(s_target_link));
    memset(&s_target_heading, 0, sizeof(s_target_heading));
    memset(&s_target_note, 0, sizeof(s_target_note));
    memset(&s_hosts, 0, sizeof(s_hosts));
    memset(&s_themes, 0, sizeof(s_themes));
    memset(&s_haptics, 0, sizeof(s_haptics));
    memset(&s_haptics_text, 0, sizeof(s_haptics_text));
    memset(&s_calibrate, 0, sizeof(s_calibrate));
    memset(&s_mode_choices, 0, sizeof(s_mode_choices));
}

static void binding_identity(lv_obj_t *object, uint32_t generation, uint16_t record)
{
    for (size_t i = s_core_bindings; i < s_binding_count; ++i) {
        if (s_bindings[i].object != object) continue;
        s_bindings[i].wifi_generation = generation;
        s_bindings[i].wifi_record = record;
        return;
    }
}

static void create_extension(lv_obj_t *body, lv_obj_t *title, app_overlay_t kind)
{
    if (kind == APP_OVERLAY_WIFI_NETWORKS) {
        label_text(title, "Nearby Wi-Fi");
        s_scan_state = label(body, "Scan unavailable", 0, 0, 456, MUTED);
        lv_obj_set_style_text_font(s_scan_state, META_FONT, 0);
        lv_obj_set_height(s_scan_state, 36);
        lv_label_set_long_mode(s_scan_state, LV_LABEL_LONG_DOT);
        for (unsigned i = 0; i < 4; ++i) {
            s_scan_rows[i] = button(body, "Network", 0, 44 + (int)i * 60, 456, 52, APP_WIFI_NETWORK_SELECT, 0);
            lv_obj_t *caption = lv_obj_get_child(s_scan_rows[i], 0);
            lv_obj_set_align(caption, LV_ALIGN_TOP_LEFT);
            lv_obj_set_pos(caption, 8, 4);
            lv_obj_set_width(caption, 440);
            lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
            s_scan_meta[i] = metadata(s_scan_rows[i], "", 8, 29, 440);
            binding_identity(s_scan_rows[i], s_view.wifi_scan.generation, s_view.wifi_scan.rows[i].record_index);
        }
        s_scan_empty = label(body, "No scan results", 0, 112, 456, MUTED);
        lv_obj_set_height(s_scan_empty, 120);
        s_scan_previous = button(body, "Previous", 0, 292, 144, 44, APP_WIFI_SCAN_PAGE, -1);
        s_scan_page = metadata(body, "", 156, 306, 144);
        lv_obj_set_style_text_align(s_scan_page, LV_TEXT_ALIGN_CENTER, 0);
        s_scan_next = button(body, "Next", 312, 292, 144, 44, APP_WIFI_SCAN_PAGE, 1);
        s_scan_refresh = button(body, "Refresh", 0, 360, 224, 48, APP_WIFI_SCAN, 1);
        binding_identity(s_scan_previous, s_view.wifi_scan.generation, 0);
        binding_identity(s_scan_next, s_view.wifi_scan.generation, 0);
        binding_identity(s_scan_refresh, s_view.wifi_scan.generation, 0);
        button(body, "Wi-Fi settings", 232, 360, 224, 48, APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    } else if (kind == APP_OVERLAY_WIFI_NETWORK) {
        label_text(title, "Wi-Fi network");
        s_network_name = heading(body, "Network unavailable", 0, 0, 456);
        lv_obj_set_style_text_font(s_network_name, BODY_FONT, 0);
        lv_obj_set_height(s_network_name, 116);
        lv_label_set_long_mode(s_network_name, LV_LABEL_LONG_DOT);
        s_network_detail = label(body, "", 0, 128, 456, MUTED);
        lv_obj_set_height(s_network_detail, 64);
        s_network_note = label(body, "", 0, 204, 456, MUTED);
        lv_obj_set_height(s_network_note, 84);
        s_network_join[0] = button(body, "Join once", 0, 300, 224, 48, APP_WIFI_CREDENTIAL_OPEN, 0);
        s_network_join[1] = button(body, "Remember + join", 232, 300, 224, 48, APP_WIFI_CREDENTIAL_OPEN, 1);
        for (unsigned i = 0; i < 2; ++i)
            binding_identity(s_network_join[i], s_view.wifi_selection.generation, s_view.wifi_selection.network.record_index);
        button(body, "Back to networks", 0, 360, 456, 48, APP_MENU, APP_OVERLAY_WIFI_NETWORKS);
    } else if (kind == APP_OVERLAY_LOCAL_EDITOR) {
        bool wifi = s_view.local_editor.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE;
        label_text(title, wifi ? "Wi-Fi password" : "Device name");
        lv_obj_t *preview = box(body, 0, 0, 456, 64, SOFT);
        s_editor_source = metadata(preview, "", 12, 4, 432);
        s_editor_preview = label(preview, "", 12, 24, 432, INK);
        single_line(s_editor_preview);
        s_editor_context = metadata(preview, "", 12, 46, 432);
        for (unsigned i = 0; i < 9; ++i)
            s_editor_grid[i] = editor_button(body, "", (int)(i % 3) * 120, 76 + (int)(i / 3) * 84, 112, 76, i);
        s_editor_back = editor_button(body, "Groups", 360, 76, 96, 76, 9);
        s_editor_case = editor_button(body, "ABC", 360, 160, 96, 76, 10);
        s_editor_symbol = editor_button(body, "Symbols", 360, 244, 96, 76, 11);
        s_editor_status = metadata(body, "", 0, 328, 456);
        s_editor_delete = editor_button(body, "Delete", 0, 360, 144, 48, 12);
        s_editor_space = editor_button(body, "Space", 156, 360, 144, 48, 13);
        s_editor_apply = editor_button(body, wifi ? "Join" : "Apply", 312, 360, 144, 48, 14);
        selected(s_editor_apply, true);
    } else if (kind == APP_OVERLAY_COMMON_KEYS) {
        label_text(title, "Live host keys");
        s_common_context = metadata(body, "", 0, 0, 456);
        s_common_modifiers = metadata(body, "", 0, 24, 456);
        const char *shortcuts[] = {"Copy", "Paste", "Undo"};
        const app_shortcut_t actions[] = {APP_SHORTCUT_COPY, APP_SHORTCUT_PASTE, APP_SHORTCUT_UNDO};
        for (unsigned i = 0; i < 3; ++i)
            s_common_keys[i] = button(body, shortcuts[i], (int)i * 156, 48, 144, 52, APP_SHORTCUT, actions[i]);
        const char *names[] = {"Backspace", "Delete fwd", "Enter", "Esc", "Tab", "Home", "Up", "End", "Left", "Down", "Right"};
        const int usages[] = {0x2a, 0x4c, 0x28, 0x29, 0x2b, 0x4a, 0x52, 0x4d, 0x50, 0x51, 0x4f};
        for (unsigned i = 0; i < 11; ++i) {
            unsigned cell = i < 5 ? i + 3 : i + 4;
            s_common_keys[i + 3] = button(body, names[i], (int)(cell % 3) * 156, 48 + (int)(cell / 3) * 60, 144, 52, APP_KEY, usages[i]);
        }
        s_common_keys[14] = button(body, "Select all", 312, 168, 144, 52, APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL);
        button(body, "Back to keyboard", 0, 360, 456, 48, APP_CLOSE, 0);
    }
}

static void create_overlay(app_overlay_t kind, app_settings_page_t page)
{
    s_overlay = box(s_screen, 0, 0, 480, 480, SURFACE);
    lv_obj_t *title = heading(s_overlay, "Menu", 12, 16, 256);
    single_line(title);
    s_overlay_critical = critical_control(s_overlay);
    button(s_overlay, "Close", 404, 8, 64, 48,
           kind == APP_OVERLAY_CONFIRM ? APP_CONFIRM : APP_CLOSE, 0);
    lv_obj_t *body = box(s_overlay, 12, 64, 456, 408, SURFACE);
    if (kind == APP_OVERLAY_MODES) {
        lv_obj_t *summary = box(body, 0, 0, 456, 104, SOFT);
        lv_obj_set_style_radius(summary, 12, 0);
        s_overlay_target = heading(summary, "No target", 12, 8, 432);
        single_line(s_overlay_target);
        s_overlay_state = metadata(summary, "Input unavailable", 12, 40, 432);
        s_telemetry = metadata(summary, "Battery -- / Wi-Fi unavailable", 12, 72, 432);
        const char *names[] = {"Mouse", "Keyboard", "Audio"};
        for (int i = 0; i < 3; ++i)
            s_mode_choices[i] = button(body, names[i], i * 156, 116, 144, 104, APP_MODE, i);
        button(body, "Status", 0, 232, 224, 64, APP_MENU, APP_OVERLAY_TARGET);
        button(body, "Settings", 232, 232, 224, 64, APP_MENU, APP_OVERLAY_SETTINGS);
        button(body, "Lock input", 0, 308, 456, 64, APP_LOCK, 0);
        metadata(body, "AI: single acts / double opens Menu / hold locks", 0, 384, 456);
    } else if (kind == APP_OVERLAY_TARGET) {
        label_text(title, "Status");
        s_target_heading = heading(body, "Input connection", 0, 0, 456);
        s_target_detail = heading(body, "No target", 0, 40, 456);
        single_line(s_target_detail);
        s_target_link = label(body, "Input unavailable", 0, 76, 456, MUTED);
        lv_obj_set_style_text_font(s_target_link, META_FONT, 0);
        lv_obj_set_height(s_target_link, 60);
        s_target_note = label(body, "", 0, 144, 456, MUTED);
        lv_obj_set_height(s_target_note, 48);
        s_telemetry = label(body, "Battery --", 0, 204, 456, MUTED);
        lv_obj_set_height(s_telemetry, 36);
        s_overlay_state = label(body, "", 0, 244, 456, MUTED);
        lv_obj_set_height(s_overlay_state, 44);
        button(body, "Connections", 0, 300, 224, 48, APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
        button(body, "Input settings", 232, 300, 224, 48, APP_SETTINGS_PAGE, APP_SETTINGS_INPUT);
        button(body, "Back to Menu", 0, 360, 456, 48, APP_MENU, APP_OVERLAY_MODES);
    } else if (kind == APP_OVERLAY_SETTINGS) {
        s_settings_title = title;
        create_settings(body, page);
    } else if (kind == APP_OVERLAY_KEYBOARD) {
        label_text(title, "Keyboard entry");
        const char *names[] = {"Motion", "Touch", "Spoken letters", "Shortcuts"};
        for (int i = 0; i < 4; ++i)
            s_submodes[i] = button(body, names[i], 0, i * 72, 456, 64, APP_KEYBOARD_SUBMODE, i);
        button(body, "Common keys", 0, 288, 456, 64, APP_MENU, APP_OVERLAY_COMMON_KEYS);
    } else if (kind == APP_OVERLAY_AUDIO) {
        label_text(title, "Audio");
        heading(body, "Mic and Speaker", 0, 12, 456);
        label(body, "Use the Audio screen to start or stop either one.", 0, 64, 456, MUTED);
        button(body, "Audio connection", 0, 144, 456, 56, APP_MENU, APP_OVERLAY_MEDIA_OPTIONS);
        button(body, "Back to Audio", 0, 360, 456, 48, APP_CLOSE, 0);
    } else if (kind >= APP_OVERLAY_MEDIA_OPTIONS && kind <= APP_OVERLAY_RECORDING_START) {
        create_media_panel(body, title, kind);
    } else if (kind >= APP_OVERLAY_API_CONNECTION && kind <= APP_OVERLAY_BLUETOOTH_PAIRING) {
        create_connection_panel(body, title, kind);
    } else if (kind == APP_OVERLAY_RECORDING_STORAGE) {
        label_text(title, "Recording storage");
        s_record_store_state = heading(body, "Storage status unknown", 0, 0, 456);
        lv_obj_set_height(s_record_store_state, 48);
        s_record_store_profile = label(body, "Recording profile unavailable", 0, 56, 456, MUTED);
        lv_obj_set_height(s_record_store_profile, 40);
        s_record_store_capacity = label(body, "Capacity unavailable", 0, 104, 456, INK);
        lv_obj_set_height(s_record_store_capacity, 48);
        s_record_store_time = label(body, "Remaining time unavailable", 0, 168, 456, INK);
        lv_obj_set_height(s_record_store_time, 40);
        s_record_store_reason = label(body, "", 0, 216, 456, MUTED);
        lv_obj_set_style_text_font(s_record_store_reason, META_FONT, 0);
        lv_obj_set_height(s_record_store_reason, 72);
        s_record_store_prepare = button(body, "Prepare unavailable", 0, 304, 456, 44, APP_RECORDING_STORAGE_PREPARE_REQUEST, 0);
        button(body, "Back to Audio", 0, 360, 456, 48, APP_MENU, APP_OVERLAY_AUDIO);
    } else if (kind == APP_OVERLAY_WIFI_SETUP) {
        label_text(title, "Set up Wi-Fi");
        s_setup_state = metadata(body, "Preparing setup...", 0, 0, 456);
        metadata(body, "1. Join this temporary Wi-Fi network", 0, 32, 456);
        s_setup_ssid = label(body, "--", 0, 60, 456, INK);
        lv_obj_set_height(s_setup_ssid, 42);
        lv_label_set_long_mode(s_setup_ssid, LV_LABEL_LONG_DOT);
        metadata(body, "Passcode", 0, 112, 456);
        s_setup_passcode = label(body, "--", 0, 140, 456, INK);
        lv_obj_set_height(s_setup_passcode, 42);
        lv_label_set_long_mode(s_setup_passcode, LV_LABEL_LONG_DOT);
        metadata(body, "2. Open this address in your browser", 0, 196, 456);
        s_setup_url = label(body, "--", 0, 224, 456, INK);
        lv_obj_set_height(s_setup_url, 42);
        lv_label_set_long_mode(s_setup_url, LV_LABEL_LONG_DOT);
        label(body, "Save your home Wi-Fi there.\nSetup ends automatically.", 0, 292, 456, MUTED);
        s_setup_stop = button(body, "Stop setup", 0, 360, 456, 48, APP_WIFI_SETUP, 0);
    } else if (kind == APP_OVERLAY_CONFIRM) {
        if (s_view.confirmation == APP_CONFIRM_REMOTE) {
            create_remote_review(body, title);
        } else {
        label_text(title, "Confirm action");
        s_confirm_title = heading(body, "Confirm action", 0, 32, 456);
        lv_obj_set_height(s_confirm_title, 64);
        s_confirm_detail = label(body, "", 0, 128, 456, MUTED);
        lv_obj_set_height(s_confirm_detail, 156);
        button(body, "Cancel", 0, 360, 224, 48, APP_CONFIRM, 0);
        s_confirm_accept = button(body, "Confirm", 232, 360, 224, 48, APP_CONFIRM, 1);
        selected(s_confirm_accept, true);
        }
    } else create_extension(body, title, kind);
    /* Safety surfaces remain above newly created ordinary navigation. */
    lv_obj_move_foreground(s_lock_shield);
    lv_obj_move_foreground(s_sleep_shield);
}

static bool reconcile_overlay(const app_view_t *view)
{
    app_overlay_t desired = view->locked || view->screen_off ? APP_OVERLAY_NONE : view->overlay;
    bool same = desired == s_built_overlay &&
        (desired != APP_OVERLAY_CONFIRM || s_built_remote_review == (view->confirmation == APP_CONFIRM_REMOTE)) &&
        (desired != APP_OVERLAY_SETTINGS || view->settings_page == s_built_settings) &&
        (desired != APP_OVERLAY_WIFI_NETWORKS || (view->wifi_scan.generation == s_built_scan_generation && view->wifi_scan.page == s_built_scan_page)) &&
        (desired != APP_OVERLAY_WIFI_NETWORK || (view->wifi_selection.generation == s_built_scan_generation && view->wifi_selection.network.record_index == s_built_scan_page)) &&
        (desired != APP_OVERLAY_LOCAL_EDITOR || (view->local_editor.session == s_built_editor_session &&
            view->local_editor.purpose == s_built_editor_purpose));
    if (same) return true;
    /* Never delete a pressed object or its callback binding. A stale intent
     * retains its press-time revision and is rejected by the coordinator. */
    if (s_touch_active) return false;
    if (s_overlay) lv_obj_delete(s_overlay);
    s_overlay = NULL;
    s_binding_count = s_core_bindings;
    s_color_count = s_core_colors;
    clear_panel_bindings();
    s_built_overlay = desired;
    s_built_remote_review = desired == APP_OVERLAY_CONFIRM && view->confirmation == APP_CONFIRM_REMOTE;
    s_built_settings = view->settings_page;
    s_built_scan_generation = desired == APP_OVERLAY_WIFI_NETWORK ? view->wifi_selection.generation : view->wifi_scan.generation;
    s_built_scan_page = desired == APP_OVERLAY_WIFI_NETWORK ? view->wifi_selection.network.record_index : view->wifi_scan.page;
    s_built_editor_session = view->local_editor.session;
    s_built_editor_purpose = view->local_editor.purpose;
    if (desired != APP_OVERLAY_NONE) create_overlay(desired, view->settings_page);
    return true;
}

static void on_screen_delete(lv_event_t *event)
{
    if (lv_event_get_target(event) != s_screen) return;
    clear_editor();
    secure_clear(&s_view, sizeof(s_view));
    s_local_submit = NULL;
    s_screen = NULL;
    s_capture_indicator = (app_ui_capture_indicator_t){0};
}

void app_ui_create(lv_display_t *display, app_submit_fn callback)
{
    s_keyboard_header_compact = false;
    s_submit = callback;
    s_local_submit = NULL;
    clear_editor();
    s_editor_session = 0;
    s_editor_purpose = APP_LOCAL_EDIT_NONE;
    s_binding_count = 0;
    s_media_binding_count = 0;
    s_core_media_bindings = 0;
    memset(s_media_bindings, 0, sizeof(s_media_bindings));
    s_color_count = 0;
    s_dark_theme = false;
    s_gesture = 0;
    s_touch_active = false;
    memset(&s_wheel, 0, sizeof(s_wheel));
    s_touch_target = NULL;
    s_touch_device = NULL;
    s_wake_touch = false;
    s_capture_indicator = (app_ui_capture_indicator_t){0};
    memset(&s_view, 0, sizeof(s_view));
    s_screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(s_screen);
    background(s_screen, SURFACE, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_screen, BODY_FONT, 0);
    foreground(s_screen, INK, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_touch, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    clear_panel_bindings();
    s_overlay = NULL;
    s_built_overlay = APP_OVERLAY_NONE;
    s_built_settings = APP_SETTINGS_HOME;
    s_mode_title = heading(s_screen, "Mouse", 12, 8, 120);
    single_line(s_mode_title);
    s_context = metadata(s_screen, "BT / No target", 12, 36, 120);
    s_critical = critical_control(s_screen);
    button(s_screen, "Menu", 404, 8, 64, 48, APP_MENU, APP_OVERLAY_MODES);
    s_keyboard_choice = button(s_screen, "Touch", 140, 8, 132, 48, APP_MENU, APP_OVERLAY_KEYBOARD);
    s_host_keys = button(s_screen, "Keys", 224, 8, 48, 48, APP_MENU, APP_OVERLAY_COMMON_KEYS);
    s_navigation_home = button(s_screen, "Home", 140, 8, 62, 48, APP_NAV_HOME, 0);
    s_navigation_back = button(s_screen, "Back", 210, 8, 62, 48, APP_NAV_BACK, 0);
    s_audio_choice = button(s_screen, "Connection", 140, 8, 132, 48, APP_MENU, APP_OVERLAY_MEDIA_OPTIONS);
    for (int i = 0; i < 3; ++i) s_panels[i] = box(s_screen, 12, 64, 456, 408, SURFACE);
    create_mouse(s_panels[INPUT_MOUSE]);
    create_keyboard(s_panels[INPUT_KEYBOARD]);
    create_audio(s_panels[INPUT_AUDIO]);
    s_lock_shield = box(s_screen, 0, 0, 480, 480, SURFACE);
    heading(s_lock_shield, "Input locked", 24, 144, 432);
    s_lock_detail = label(s_lock_shield, "Input stopped.", 24, 216, 432, MUTED);
    heading(s_lock_shield, "Hold AI again to unlock.", 24, 352, 432);
    s_sleep_shield = box(s_screen, 0, 0, 480, 480, 0x000000);
    lv_obj_add_flag(s_sleep_shield, LV_OBJ_FLAG_PRESS_LOCK);
    render_media_core();
    render_speech();
    s_core_media_bindings = s_media_binding_count;
    s_core_bindings = s_binding_count;
    s_core_colors = s_color_count;
    static const app_view_t initial = {.mode = INPUT_MOUSE, .dark_theme = true,
        .keyboard_group = -1, .gain_divisor = 1,
        .microphone_muted = true};
    app_ui_render(&initial);
}

void app_ui_set_local_edit_submit(app_local_edit_submit_fn callback)
{
    s_local_submit = callback;
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

static const char *security_name(app_ui_wifi_security_t security)
{
    switch (security) {
    case APP_UI_SECURITY_OPEN: return "Open";
    case APP_UI_SECURITY_WEP: return "WEP";
    case APP_UI_SECURITY_WPA: return "WPA";
    case APP_UI_SECURITY_WPA2: return "WPA2";
    case APP_UI_SECURITY_WPA_WPA2: return "WPA/WPA2";
    case APP_UI_SECURITY_WPA3: return "WPA3";
    case APP_UI_SECURITY_WPA2_WPA3: return "WPA2/WPA3";
    case APP_UI_SECURITY_ENTERPRISE: return "Enterprise";
    case APP_UI_SECURITY_OWE: return "OWE";
    case APP_UI_SECURITY_OTHER: return "Other security";
    default: return "Security unknown";
    }
}

static const char *join_reason(app_ui_join_availability_t availability)
{
    switch (availability) {
    case APP_UI_JOIN_AVAILABLE: return "Password required";
    case APP_UI_JOIN_HIDDEN_SSID: return "Hidden network / manual entry unavailable";
    case APP_UI_JOIN_UNSUPPORTED_SSID: return "Network name is unsupported";
    case APP_UI_JOIN_UNSUPPORTED_SECURITY: return "Security type is unsupported";
    default: return "Join unavailable";
    }
}

static void render_extension(const app_view_t *view, bool hid_ready)
{
    if (view->overlay == APP_OVERLAY_LOCAL_EDITOR) { render_editor(); return; }
    if (view->overlay == APP_OVERLAY_COMMON_KEYS) {
        label_format(s_common_context, "%s / %s",
            view->input_transport == APP_INPUT_USB ? "USB" : "Bluetooth", s_target_text);
        label_text(s_common_modifiers, view->host_os == APP_HOST_COMMAND ?
            "Apple shortcuts / Cmd+C, Cmd+V, Cmd+Z, Cmd+A" : "Windows shortcuts / Ctrl+C, Ctrl+V, Ctrl+Z, Ctrl+A");
        for (unsigned i = 0; i < COMMON_KEY_BUTTONS; ++i)
            enabled(s_common_keys[i], hid_ready && view->mode == INPUT_KEYBOARD && !view->sending);
        return;
    }
    if (view->overlay == APP_OVERLAY_WIFI_NETWORKS) {
        const app_wifi_scan_view_t *scan = &view->wifi_scan;
        const char *phase = scan->state == APP_UI_SCAN_IDLE ? "Ready to scan" :
            scan->state == APP_UI_SCAN_QUEUED ? "Scan queued" :
            scan->state == APP_UI_SCAN_RUNNING ? "Scanning nearby networks..." :
            scan->state == APP_UI_SCAN_CANCELLING ? "Cancelling scan..." :
            scan->state == APP_UI_SCAN_READY ? "Scan ready" :
            scan->state == APP_UI_SCAN_CANCELLED ? "Scan cancelled" :
            scan->state == APP_UI_SCAN_ERROR ? "Scan failed" :
            scan->state == APP_UI_SCAN_EXPIRED ? "Scan expired / refresh to continue" : "Scan unavailable";
        label_text(s_scan_state, *scan->message ? scan->message : phase);
        bool ready = scan->state == APP_UI_SCAN_READY && scan->generation && !scan->cleanup_pending;
        size_t count = ready ? scan->row_count : 0;
        for (unsigned i = 0; i < 4; ++i) {
            bool show = i < count;
            visible(s_scan_rows[i], show);
            if (!show) continue;
            const app_wifi_row_view_t *row = &scan->rows[i];
            text(s_scan_rows[i], *row->label ? row->label : "Unnamed network");
            char signal[24];
            if (row->rssi_valid) snprintf(signal, sizeof(signal), "%d dBm", row->rssi_dbm);
            else snprintf(signal, sizeof(signal), "Signal unknown");
            label_format(s_scan_meta[i], "%s / %s%s%s%s", security_name(row->security), signal,
                row->current ? " / current" : "", row->remembered ? " / saved" : "",
                row->availability != APP_UI_JOIN_AVAILABLE ? " / unsupported" : "");
            enabled(s_scan_rows[i], true); /* Unsupported rows still explain why. */
            if (!s_touch_active) binding_identity(s_scan_rows[i], scan->generation, row->record_index);
        }
        visible(s_scan_empty, !count);
        label_text(s_scan_empty, ready ? "No networks found. Refresh to try again." : phase);
        unsigned pages = ((unsigned)scan->count + 3U) / 4U;
        if (!pages) pages = 1;
        label_format(s_scan_page, "%u / %u%s", scan->page + 1, pages, scan->capacity_reached ? " / capped" : "");
        enabled(s_scan_previous, ready && scan->page > 0);
        enabled(s_scan_next, ready && scan->page + 1 < pages);
        text(s_scan_refresh, scan->can_cancel ? "Cancel scan" : scan->retry_seconds ? "Scan cooling down" : "Refresh");
        enabled(s_scan_refresh, scan->can_cancel || scan->can_start);
        return;
    }
    if (view->overlay == APP_OVERLAY_WIFI_NETWORK) {
        const app_wifi_selection_view_t *selection = &view->wifi_selection;
        const app_wifi_row_view_t *row = &selection->network;
        label_text(s_network_name, selection->valid ? row->label : "Network selection expired");
        label_format(s_network_detail, "%s%s%s\n%s", security_name(row->security),
            row->current ? " / current" : "", row->remembered ? " / saved" : "",
            selection->valid ? join_reason(row->availability) : "Return to the list and refresh.");
        label_text(s_network_note, *selection->message ? selection->message :
            "Joins your existing Wi-Fi network.\nYour computer stays on its Wi-Fi.\nWeb/API availability is shown separately.");
        enabled(s_network_join[0], selection->valid && selection->can_join_once && row->availability == APP_UI_JOIN_AVAILABLE);
        enabled(s_network_join[1], selection->valid && selection->can_remember && row->availability == APP_UI_JOIN_AVAILABLE);
    }
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
    const char *wifi_status = view->wifi_state == APP_UI_WIFI_CONNECTED ? "connected" :
        view->wifi_state == APP_UI_WIFI_UNAVAILABLE ? "unavailable" :
        view->wifi_state == APP_UI_WIFI_ERROR ? "error" :
        view->wifi_state == APP_UI_WIFI_CONNECTING ? "connecting" :
        view->wifi_state == APP_UI_WIFI_OBTAINING_IP ? "getting address" :
        view->wifi_state == APP_UI_WIFI_RETRYING ? "reconnecting" : "disconnected";
    if (view->wifi_state == APP_UI_WIFI_CONNECTED && view->wifi_rssi_valid)
        label_format(s_telemetry, "%s / Wi-Fi %d dBm", battery, view->wifi_rssi_dbm);
    else label_format(s_telemetry, "%s / Wi-Fi %s", battery, wifi_status);
    const char *titles[] = {"Settings", "Connectivity", "Display", "Input", "About", "Bluetooth hosts", "Wi-Fi",
        "Motion sensitivity", "Motion tuning", "Touch gestures", "Navigation"};
    unsigned page = (unsigned)view->settings_page < sizeof(titles) / sizeof(titles[0]) ? (unsigned)view->settings_page : 0;
    label_text(s_settings_title, titles[page]);
    const char *saved = view->settings_save_state == APP_SAVE_SAVED ? "Settings saved" :
        view->settings_save_state == APP_SAVE_PENDING ? "Saving settings..." :
        view->settings_save_state == APP_SAVE_ERROR ? "Could not save settings. Try again." :
        view->settings_save_state == APP_SAVE_DEFAULTS ? "Defaults active / not saved yet" :
        view->settings_save_state == APP_SAVE_VOLATILE ? "Temporary settings / not saved" : "Settings storage unavailable";
    if (view->settings_page != APP_SETTINGS_BLUETOOTH) {
        label_text(s_save_status, saved);
        foreground(s_save_status, view->settings_save_state == APP_SAVE_ERROR ? WARNING : MUTED, 0);
    }
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
    bool pairing_available = view->pairing_state != APP_PAIRING_UNAVAILABLE && view->pairing_state != APP_PAIRING_CLEARING &&
        view->pairing_state != APP_PAIRING_ERROR;
    visible(s_pairing_buttons[0], view->pairing_state != APP_PAIRING_OPEN);
    visible(s_pairing_buttons[1], view->pairing_state == APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[0], pairing_available && pairing_room && view->pairing_state != APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[1], view->pairing_state == APP_PAIRING_OPEN);
    enabled(s_pairing_buttons[2], pairing_available && view->paired_host_count != 0);
    text(s_bond_add, !capacity_known ? "Limit unknown" : !count_known ? "Count unknown" :
         pairings_full ? "Pairings full" : "Add host");
    enabled(s_bond_add, view->bluetooth_management.known && view->bluetooth_management.can_add &&
        pairing_available && pairing_room && view->pairing_state != APP_PAIRING_OPEN);
    text(s_bond_disconnect, view->bluetooth_connected ? "Disconnect" : view->bluetooth_reconnect_paused ? "Resume" : "Disconnect");
    enabled(s_bond_disconnect, view->bluetooth_connected || view->bluetooth_reconnect_paused);
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
        if (!show) {
            connection_bind(s_bond_rows[i], APP_BLUETOOTH_TARGET_CONTROL, (connection_request_t){0}, false);
            continue;
        }
        const app_bond_view_t *host = &view->bluetooth_hosts[index];
        char name[48];
        size_t length = bounded_length(host->identity, sizeof(host->identity));
        snprintf(name, sizeof(name), "Host %s", length > 8 ? host->identity + length - 8 :
                 length ? host->identity : "(identity unavailable)");
        label_text(s_bond_names[i], *host->name ? host->name : name);
        label_text(s_bond_ids[i], *host->identity ? host->identity : "Identity unavailable");
        label_text(s_bond_states[i], (view->bluetooth_connected && host->connected) ? "Connected" : "Saved");
        foreground(s_bond_states[i], (view->bluetooth_connected && host->connected) ? ACCENT : MUTED, 0);
        bind_bluetooth_row(s_bond_rows[i], host);
    }
    char capacity[32];
    if (!capacity_known) snprintf(capacity, sizeof(capacity), "Limit unknown");
    else if (!count_known) snprintf(capacity, sizeof(capacity), "Count unknown");
    else snprintf(capacity, sizeof(capacity), "%s%d/%u%s", pairings_full ? "Full: " : "",
                  view->paired_host_count, view->paired_host_capacity, pairings_full ? "" : " saved");
    label_format(s_bond_page, "%u / %u\n%s", host_page + 1, page_count, capacity);
    foreground(s_bond_page, pairings_full ? WARNING : MUTED, 0);
    enabled(s_bond_previous, view->bluetooth_hosts_available && host_page > 0);
    enabled(s_bond_next, view->bluetooth_hosts_available && host_page + 1 < page_count);
    enabled(s_name_edit, view->bluetooth_name.available && !view->bluetooth_name.pending && !view->bluetooth_name.uncertain);
    if (view->overlay == APP_OVERLAY_SETTINGS && view->settings_page == APP_SETTINGS_BLUETOOTH) {
        char name[APP_UI_BLUETOOTH_NAME_CAPACITY * 4];
        name_label(name, sizeof(name), view->bluetooth_name.effective);
        label_format(s_save_status, "This device: %s / %s", view->bluetooth_name.effective_valid ? name : "Name unavailable",
            view->bluetooth_name.uncertain ? "outcome unknown" : view->bluetooth_name.pending ? "applying name" :
            view->bluetooth_name.save_state == APP_SAVE_ERROR ? "name not saved" :
            view->bluetooth_name.save_state == APP_SAVE_PENDING ? "saving name" :
            view->bluetooth_name.save_state == APP_SAVE_SAVED ? "name saved" :
            view->bluetooth_name.save_state == APP_SAVE_VOLATILE ? "temporary name / not saved" : "save unconfirmed");
        foreground(s_save_status, view->bluetooth_name.uncertain || view->bluetooth_name.save_state == APP_SAVE_ERROR ? WARNING : MUTED, 0);
    }
    const char *wifi = view->wifi_state == APP_UI_WIFI_CONNECTED ? "Connected" :
        view->wifi_state == APP_UI_WIFI_CONNECTING ? "Connecting" :
        view->wifi_state == APP_UI_WIFI_OBTAINING_IP ? "Getting address" :
        view->wifi_state == APP_UI_WIFI_RETRYING ? "Reconnecting" :
        view->wifi_state == APP_UI_WIFI_ERROR ? "Connection error" :
        view->wifi_state == APP_UI_WIFI_DISCONNECTED ? "Disconnected" : "Unavailable";
    snprintf(summary, sizeof(summary), "%s%s%s", wifi, *view->wifi_ssid ? " / " : "", view->wifi_ssid);
    label_text(s_wifi_summary, *view->wifi_message ? view->wifi_message : summary);
    foreground(s_wifi_summary, *view->wifi_message ? WARNING : MUTED, 0);
    label_text(s_wifi_state, wifi);
    label_text(s_wifi_ssid, *view->wifi_ssid ? view->wifi_ssid : "No network");
    label_text(s_wifi_note, *view->wifi_message ? view->wifi_message :
               "Join here; keep your computer on its Wi-Fi.");
    if (view->wifi_rssi_valid && view->wifi_state == APP_UI_WIFI_CONNECTED)
        snprintf(summary, sizeof(summary), "%d dBm / %s", view->wifi_rssi_dbm,
                 *view->wifi_ipv4 ? view->wifi_ipv4 : "IP unavailable");
    else if (view->wifi_state == APP_UI_WIFI_CONNECTED)
        snprintf(summary, sizeof(summary), "Signal unknown / %s", *view->wifi_ipv4 ? view->wifi_ipv4 : "IP unavailable");
    else snprintf(summary, sizeof(summary), "%s", view->wifi_state == APP_UI_WIFI_UNAVAILABLE ?
                  "Signal and IP unavailable" : "No active network connection");
    label_text(s_wifi_detail, summary);
    label_format(s_wifi_saved, "%s%s%s", view->wifi_storage_uncertain ? "Saved state uncertain" :
        !view->wifi_stored_known ? "Saved state unknown" : !view->wifi_stored_present ? "No saved network" :
        !view->wifi_stored_valid ? "Saved record unavailable" : "Saved", *view->wifi_stored_label ? ": " : "", view->wifi_stored_label);
    const char *backend = view->wifi_lan_service == APP_UI_LAN_SERVICE_READY ? "Web/API ready" :
        view->wifi_lan_service == APP_UI_LAN_SERVICE_STARTING ? "Web/API starting" :
        view->wifi_lan_service == APP_UI_LAN_SERVICE_DISABLED ? "Web/API disabled" :
        view->wifi_lan_service == APP_UI_LAN_SERVICE_ERROR ? "Web/API error" : "Web/API unavailable";
    label_format(s_wifi_backend, "%s / %s%s%s", view->wifi_lan_ready ? "LAN connected" : "LAN not ready", backend,
        view->wifi_lan_service == APP_UI_LAN_SERVICE_READY && *view->wifi_lan_service_url ? "\n" :
        *view->wifi_lan_service_message ? "\n" : "",
        view->wifi_lan_service == APP_UI_LAN_SERVICE_READY ? view->wifi_lan_service_url : view->wifi_lan_service_message);
    enabled(s_wifi_find, view->wifi_scan.can_start && !view->wifi_operation_pending);
    text(s_wifi_setup, view->wifi_setup_active ? "View recovery hotspot" : view->wifi_operation_pending ? "Please wait..." : "Recovery hotspot");
    enabled(s_wifi_setup, view->wifi_state != APP_UI_WIFI_UNAVAILABLE &&
            (!view->wifi_operation_pending || view->wifi_setup_active));
    enabled(s_wifi_forget, view->wifi_state != APP_UI_WIFI_UNAVAILABLE &&
            (!view->wifi_stored_known || view->wifi_stored_present || view->wifi_storage_uncertain) && !view->wifi_operation_pending);
    if (view->brightness_available) label_format(s_brightness_text, "%u%%", view->brightness_percent);
    else label_text(s_brightness_text, "Unavailable");
    if (s_brightness) {
        unsigned brightness = view->brightness_percent < 10 ? 10 : view->brightness_percent > 100 ? 100 : view->brightness_percent;
        slider_value(s_brightness, (int)((brightness + 5) / 10));
    }
    enabled(s_brightness, view->brightness_available);
    const unsigned seconds[] = {0, 60, 300, 600, 1800};
    for (unsigned i = 0; i < 5; ++i) {
        selected(s_timeouts[i], view->idle_timeout_seconds == seconds[i]);
        enabled(s_timeouts[i], view->screen_off_available);
    }
    const char *identity[] = {view->firmware_version, view->build_id, view->board_revision, view->device_id};
    for (unsigned i = 0; i < 4; ++i) label_text(s_about_identity[i], *identity[i] ? identity[i] : "Unavailable");
    if (s_about_identity[4]) {
        label_text(s_about_identity[4], about_text_valid(view->developer, sizeof(view->developer)) ? view->developer : "Unknown");
        if (about_commit_valid(view->git_commit))
            label_format(s_about_identity[5], "%.12s / %s", view->git_commit,
                view->git_state == APP_GIT_CLEAN ? "Clean" : view->git_state == APP_GIT_DIRTY ? "Modified" : "Unknown");
        else label_text(s_about_identity[5], "Unknown");
        label_text(s_about_repository, about_repository_valid(view->repository_url) ? view->repository_url : "Unavailable");
    }
    if (!view->battery_supported) snprintf(summary, sizeof(summary), "Battery unavailable\nUSB power detection unavailable");
    else if (view->battery_percent_valid && view->battery_percent <= 100)
        snprintf(summary, sizeof(summary), "Battery  %u%% / %s\nUSB power detection unavailable", view->battery_percent, battery_state_name(view->battery_state));
    else snprintf(summary, sizeof(summary), "Battery  -- / %s\nUSB power detection unavailable", battery_state_name(view->battery_state));
    label_text(s_about_battery, summary);
    label_format(s_about_uptime, "Uptime  %lluh %02llum", (unsigned long long)(view->uptime_seconds / 3600),
                         (unsigned long long)(view->uptime_seconds / 60 % 60));
    if (!view->wifi_setup_active && *view->wifi_message) label_text(s_setup_state, view->wifi_message);
    else if (view->wifi_setup_active) label_format(s_setup_state, "Protected setup / expires in %u:%02u", view->wifi_setup_seconds_left / 60, view->wifi_setup_seconds_left % 60);
    else label_text(s_setup_state, view->wifi_operation_pending ? "Preparing protected setup..." : "Setup ended / credentials cleared");
    bool show_credentials = view->wifi_setup_active && view->overlay == APP_OVERLAY_WIFI_SETUP && !view->locked && !view->screen_off;
    label_text(s_setup_ssid, show_credentials && *view->wifi_setup_ssid ? view->wifi_setup_ssid : "--");
    label_text(s_setup_passcode, show_credentials && *view->wifi_setup_passcode ? view->wifi_setup_passcode : "--");
    label_text(s_setup_url, show_credentials && *view->wifi_setup_url ? view->wifi_setup_url : "--");
    enabled(s_setup_stop, view->wifi_setup_active || view->wifi_operation_pending);
    const char *title = "Confirm action";
    const char *detail = "No action is pending. Return to Settings.";
    const char *accept = "Confirm";
    char pairing_detail[192];
    switch (view->confirmation) {
    case APP_CONFIRM_PAIR_OPEN:
        title = "Disconnect and add host?";
        {
            char name[APP_UI_BLUETOOTH_NAME_CAPACITY * 4];
            name_label(name, sizeof(name), view->bluetooth_name.effective);
            snprintf(pairing_detail, sizeof(pairing_detail), "Release input; disconnect current host.\nAllow pairing for 2 minutes. Saved hosts remain.\nDevice: %s",
                view->bluetooth_name.effective_valid ? name : "unavailable");
            detail = pairing_detail;
        }
        accept = "Disconnect and add";
        break;
    case APP_CONFIRM_PAIR_CLOSE:
        title = "Close Bluetooth pairing?";
        detail = "New hosts will no longer be able to pair.\nSaved pairings remain available.";
        accept = "Close pairing";
        break;
    case APP_CONFIRM_PAIR_FORGET_ALL:
        title = "Forget Bluetooth pairings?";
        detail = "Disconnect the current input host and remove all saved input pairings.\nWait for removal and target policy verification.\nUse Add host afterward to pair again.";
        accept = "Forget pairings";
        break;
    case APP_CONFIRM_BLUETOOTH_FORGET_ONE:
        title = "Forget this saved device?";
        snprintf(pairing_detail, sizeof(pairing_detail), "Disconnect the current input host.\nForget only: %s\nOther saved pairings remain. This cannot be undone.", view->bluetooth_management.forget_identity);
        detail = pairing_detail; accept = "Forget this device";
        break;
    case APP_CONFIRM_DISCONNECT:
        title = "Disconnect Bluetooth host?";
        detail = "The Bluetooth host will disconnect.\nIts saved pairing stays available; automatic return pauses.\nUse Resume or select this saved host to reconnect.";
        accept = "Disconnect";
        break;
    case APP_CONFIRM_WIFI_FORGET:
        title = "Forget Wi-Fi network?";
        detail = "The saved Wi-Fi network will be removed.\nIt disconnects if currently in use.\nSet up this network again to reconnect.";
        accept = "Forget network";
        break;
    case APP_CONFIRM_RECORDING_STORAGE_PREPARE:
        title = "Prepare recording storage?";
        detail = "Initialize the dedicated internal recording area.\nAny data in that reserved area will be removed.\nFactory NAND storage stays untouched.\nRecording starts only after a separate action.";
        accept = "Prepare storage";
        break;
    default: break;
    }
    if (view->confirmation == APP_CONFIRM_REMOTE) { render_remote_review(); return; }
    if ((view->confirmation >= APP_CONFIRM_RECORDING_START && view->confirmation <= APP_CONFIRM_RECORDING_CANCEL_DISCARD) ||
        view->confirmation == APP_CONFIRM_AUDIO_PAIR) {
        render_media_confirmation();
        return;
    }
    label_text(s_confirm_title, title);
    label_text(s_confirm_detail, detail);
    text(s_confirm_accept, accept);
    enabled(s_confirm_accept, view->confirmation != APP_CONFIRM_NONE &&
        (view->confirmation != APP_CONFIRM_RECORDING_STORAGE_PREPARE || recording_storage_can_prepare(view)));
}

static void arm_capture_indicator(lv_obj_t *region, const app_ui_capture_indicator_t *previous)
{
    lv_obj_update_layout(region);
    if (!s_view.capture_indicator_generation || !lv_obj_is_visible(region)) return;
    lv_area_t area;
    lv_obj_get_coords(region, &area);
    if (area.x1 < 0 || area.y1 < 0 || area.x2 >= 480 || area.y2 >= 480 ||
        area.x2 < area.x1 || area.y2 < area.y1) return;
    bool same = previous->armed && previous->capture_generation == s_view.capture_indicator_generation &&
        previous->x1 == area.x1 && previous->y1 == area.y1 && previous->x2 == area.x2 && previous->y2 == area.y2;
    if (!same && s_capture_indicator_revision == UINT64_MAX) return;
    if (!same) ++s_capture_indicator_revision;
    s_capture_indicator = (app_ui_capture_indicator_t){
        .armed = true, .capture_generation = s_view.capture_indicator_generation,
        .indicator_revision = s_capture_indicator_revision,
        .x1 = (int16_t)area.x1, .y1 = (int16_t)area.y1, .x2 = (int16_t)area.x2, .y2 = (int16_t)area.y2
    };
    if (!same) lv_obj_invalidate(region);
}

void app_ui_render(const app_view_t *view)
{
    if (!view || !s_screen) return;
    app_ui_capture_indicator_t previous_indicator = s_capture_indicator;
    s_capture_indicator = (app_ui_capture_indicator_t){0};
    observe_remote_review_clock(&view->remote_review);
    bool access_changed = s_view.local_access != view->local_access;
    s_view = *view;
    if (access_changed) {
        for (size_t i = 0; i < s_binding_count; ++i) {
            lv_obj_t *button = s_bindings[i].object;
            bool allow = !lv_obj_has_state(button, LV_STATE_DISABLED) || lv_obj_has_flag(button, UI_UPDATE_MASKED);
            lv_obj_remove_flag(button, UI_UPDATE_MASKED);
            enabled(button, allow);
        }
    }
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
    s_view.wifi_scan.message[64] = '\0';
    if (s_view.wifi_scan.row_count > APP_UI_WIFI_ROWS_PER_PAGE)
        s_view.wifi_scan.row_count = APP_UI_WIFI_ROWS_PER_PAGE;
    for (unsigned i = 0; i < APP_UI_WIFI_ROWS_PER_PAGE; ++i)
        s_view.wifi_scan.rows[i].label[APP_UI_WIFI_LABEL_CAPACITY - 1] = '\0';
    s_view.wifi_selection.network.label[APP_UI_WIFI_LABEL_CAPACITY - 1] = '\0';
    s_view.wifi_selection.message[64] = '\0';
    s_view.wifi_stored_label[APP_UI_WIFI_LABEL_CAPACITY - 1] = '\0';
    s_view.wifi_lan_service_message[64] = '\0';
    s_view.wifi_lan_service_url[95] = '\0';
    if (bounded_length(s_view.bluetooth_name.configured, APP_UI_BLUETOOTH_NAME_CAPACITY) == APP_UI_BLUETOOTH_NAME_CAPACITY) {
        memset(s_view.bluetooth_name.configured, 0, sizeof(s_view.bluetooth_name.configured));
        s_view.bluetooth_name.configured_valid = false;
    }
    if (bounded_length(s_view.bluetooth_name.effective, APP_UI_BLUETOOTH_NAME_CAPACITY) == APP_UI_BLUETOOTH_NAME_CAPACITY) {
        memset(s_view.bluetooth_name.effective, 0, sizeof(s_view.bluetooth_name.effective));
        s_view.bluetooth_name.effective_valid = false;
    }
    if (bounded_length(s_view.bluetooth_name.requested, APP_UI_BLUETOOTH_NAME_CAPACITY) == APP_UI_BLUETOOTH_NAME_CAPACITY)
        memset(s_view.bluetooth_name.requested, 0, sizeof(s_view.bluetooth_name.requested));
    s_view.bluetooth_name.message[64] = '\0';
    if (bounded_length(s_view.local_editor.bluetooth_initial, APP_UI_BLUETOOTH_NAME_CAPACITY) == APP_UI_BLUETOOTH_NAME_CAPACITY) {
        secure_clear(s_view.local_editor.bluetooth_initial, sizeof(s_view.local_editor.bluetooth_initial));
        s_view.local_editor.submit_available = false;
        snprintf(s_view.local_editor.message, sizeof(s_view.local_editor.message), "Initial name unavailable. Close and retry.");
    }
    s_view.local_editor.message[64] = '\0';
    s_view.recording_storage.profile[32] = '\0';
    s_view.recording_storage.reason[96] = '\0';
    sanitize_media();
    sanitize_connections();
    sanitize_remote_review();
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
    reconcile_editor(view);
    apply_theme(view->dark_theme);
    bool usb = view->input_transport == APP_INPUT_USB;
    bool provider_available = usb ? view->usb_available : view->bluetooth_available;
    const char *link = view->transport_switch_pending ? "Switching input..." :
        !provider_available ? "Input unavailable" : view->transport_error ? "Input connection error" :
        !view->connected ? (usb ? "Connect USB" : "Pair device") :
        view->release_pending ? "Releasing input" : view->congested ? "Host busy" :
        view->ready ? "Input ready" : "Connecting input";
    bool capture = capture_warning_required(view);
    bool alert = view->local_access != APP_LOCAL_NORMAL || view->remote_owned || capture || view->release_pending || view->transport_error ||
        !provider_available || !view->ready || view->transport_switch_pending;
    bool hid_ready = view->connected && view->ready && !view->release_pending &&
        !view->transport_error && provider_available && !view->transport_switch_pending;
    if (!hid_ready || view->mode != INPUT_MOUSE || view->overlay != APP_OVERLAY_NONE ||
        view->locked || view->screen_off || view->remote_owned ||
        view->epoch != s_touch_epoch || view->ui_revision != s_touch_revision)
        s_wheel.tracking = s_wheel.tap = false;
    /* A lock/wake shield does not depend on navigation teardown. Clear any
     * setup labels immediately even when a held gesture defers destruction. */
    if (view->locked || view->screen_off || view->overlay != APP_OVERLAY_WIFI_SETUP) {
        label_text(s_setup_ssid, "--");
        label_text(s_setup_passcode, "--");
        label_text(s_setup_url, "--");
    }
    if (!view->screen_off) visible(s_lock_shield, view->locked);
    visible(s_sleep_shield, view->screen_off);
    bool reconciled = reconcile_overlay(view);
    visible(s_overlay, !view->locked && !view->screen_off);
    if (view->locked && !view->screen_off) {
        label_format(s_lock_detail, "%s\n%s\nUnsent characters remain in preview.",
            view->release_pending ? "Input stopped. Releasing held keys..." : "Input stopped.",
            capture ? capture_warning(view, false) : view->capture_state == APP_CAPTURE_OFF_CONFIRMED ?
            "Microphone off (confirmed)." : "Microphone status unavailable.");
        foreground(s_lock_detail, capture ? ERROR : MUTED, 0);
        if (view->capture_indicator_required) arm_capture_indicator(s_lock_shield, &previous_indicator);
    } else if (!view->screen_off) {
        /* Update the actual visible surface even while a held press defers
         * navigation teardown. An old panel must never hide a new warning. */
        lv_obj_t *indicator = s_overlay ? s_overlay_critical : s_critical;
        critical_state(indicator, alert, capture, provider_available);
        if (view->capture_indicator_required && indicator) arm_capture_indicator(indicator, &previous_indicator);
    }
    if (view->locked || view->screen_off || !reconciled) return;
    if (view->overlay != APP_OVERLAY_NONE) {
        critical_state(s_overlay_critical, alert, capture, provider_available);
        render_device(view, link);
        render_extension(view, hid_ready);
        if (view->overlay == APP_OVERLAY_RECORDING_STORAGE) {
            const app_recording_storage_view_t *store = &view->recording_storage;
            const char *status = store->state == APP_RECORD_STORE_UNPREPARED ? "Storage not prepared" :
                store->state == APP_RECORD_STORE_PREPARING ? "Preparing storage..." :
                store->state == APP_RECORD_STORE_READY ? "Recording storage ready" :
                store->state == APP_RECORD_STORE_FULL ? "Recording storage full" :
                store->state == APP_RECORD_STORE_ERROR ? "Recording storage error" :
                store->state == APP_RECORD_STORE_UNCERTAIN ? "Storage state uncertain" : "Storage status unknown";
            label_text(s_record_store_state, status);
            foreground(s_record_store_state, store->state == APP_RECORD_STORE_UNCERTAIN || store->state == APP_RECORD_STORE_ERROR ?
                ERROR : store->state == APP_RECORD_STORE_UNPREPARED || store->state == APP_RECORD_STORE_FULL ? WARNING : INK, 0);
            label_format(s_record_store_profile, "%s / %s",
                store->backend == APP_RECORD_BACKEND_INTERNAL_RESERVED ? "Internal" :
                store->backend == APP_RECORD_BACKEND_EXTERNAL_NAND ? "External NAND" : "Store unknown",
                *store->profile ? store->profile : "profile unavailable");
            bool capacity_known = store->capacity_known && store->free_bytes <= store->quota_bytes;
            if (capacity_known) {
                char free_text[40], quota_text[40];
                storage_bytes(free_text, sizeof(free_text), store->free_bytes);
                storage_bytes(quota_text, sizeof(quota_text), store->quota_bytes);
                label_format(s_record_store_capacity, "Free: %s\nQuota: %s", free_text, quota_text);
            } else label_text(s_record_store_capacity, "Capacity unavailable");
            if (capacity_known && store->remaining_time_known &&
                (store->state == APP_RECORD_STORE_READY || store->state == APP_RECORD_STORE_FULL))
                label_format(s_record_store_time, "Remaining: %um %02us at this profile",
                    (unsigned)(store->remaining_seconds / 60), (unsigned)(store->remaining_seconds % 60));
            else label_text(s_record_store_time, "Remaining time unavailable");
            label_text(s_record_store_reason, *store->reason ? store->reason :
                store->state == APP_RECORD_STORE_UNPREPARED ? "Prepare the reserved recording area before recording." :
                store->state == APP_RECORD_STORE_READY ? "Available time reflects this profile and the current quota." :
                "Storage status has not been confirmed.");
            text(s_record_store_prepare, recording_storage_can_prepare(view) ? "Prepare storage" :
                store->state == APP_RECORD_STORE_PREPARING ? "Preparing..." :
                store->state == APP_RECORD_STORE_READY ? "Storage ready" : "Prepare unavailable");
            enabled(s_record_store_prepare, recording_storage_can_prepare(view));
        }
        render_media_panel();
        render_connection_panel();
        label_text(s_overlay_target, s_target_text);
        if (view->local_access == APP_LOCAL_BROWSE && !capture)
            label_text(s_overlay_state, "Verifying first boot. Input and edits are paused.");
        else if (view->overlay == APP_OVERLAY_MODES)
            label_format(s_overlay_state, "%s / %s", usb ? "USB" : "Bluetooth", link);
        else label_text(s_overlay_state, capture ? capture_warning(view, false) : *s_message_text ? s_message_text :
            view->remote_owned ? "Remote control / touch to take local control" : "Local control");
        label_text(s_target_detail, s_target_text);
        label_text(s_target_heading, usb ? "USB input" : "Bluetooth input");
        label_format(s_target_note, "%s input / Motion: %s", usb ? "USB" : "Bluetooth", motion_reason(view));
        /* Detail retains simultaneous conditions which cannot all fit in the
         * compact alert. A disconnected link does not erase a release/error. */
        label_format(s_target_link, "%s%s%s%s", link,
            view->release_pending && strcmp(link, "Releasing input") != 0 ? " / Releasing input" : "",
            view->transport_error && strcmp(link, "Input connection error") != 0 ? " / Input connection error" : "",
            !view->connected ? "\nDisconnected" : "");
        for (int i = 0; i < 3; ++i) selected(s_mode_choices[i], (input_mode_t)i == view->mode);
        for (int i = 0; i < 4; ++i) selected(s_submodes[i], (app_keyboard_submode_t)i == view->keyboard_submode);
        selected(s_themes[0], !view->dark_theme);
        selected(s_themes[1], view->dark_theme);
        selected(s_hosts[0], view->host_os == APP_HOST_CTRL);
        selected(s_hosts[1], view->host_os == APP_HOST_COMMAND);
        label_text(s_haptics_text, !view->haptics_available ? "Unavailable" : view->haptics_enabled ? "On" : "Off");
        if (s_haptics) {
            if (view->haptics_available && view->haptics_enabled) lv_obj_add_state(s_haptics, LV_STATE_CHECKED);
            else lv_obj_remove_state(s_haptics, LV_STATE_CHECKED);
            enabled(s_haptics, view->haptics_available);
        }
        text(s_calibrate, view->calibrating ? "Calibrating... keep still" : "Calibrate sensor");
        enabled(s_calibrate, (view->calibration_available || view->motion_available) && !view->calibrating);
        text(s_tap_click, !view->tap_to_click_available ? "Tap to click / unavailable" :
            view->tap_to_click_enabled ? "Tap to click / on" : "Tap to click / off");
        selected(s_tap_click, view->tap_to_click_available && view->tap_to_click_enabled);
        enabled(s_tap_click, view->tap_to_click_available);
        text(s_navigation, view->navigation_enabled ? "Navigation / on" : "Navigation / off");
        selected(s_navigation, view->navigation_enabled);
        enabled(s_reset_grip, view->motion_recenter_available);
        label_text(s_motion_detail, motion_reason(view));
        bool settings_known = motion_settings_valid(&view->motion_settings);
        bool settings_editable = settings_known && view->motion_settings.editable;
        const char *reference = view->motion_settings.reference == APP_REFERENCE_READY ? "Auto grip / ready" :
            view->motion_settings.reference == APP_REFERENCE_ACQUIRING ? "Auto grip / hold still to set" :
            view->motion_settings.reference == APP_REFERENCE_UNSTABLE ? "Auto grip / hold still to restore" :
            view->motion_settings.reference == APP_REFERENCE_UNAVAILABLE ? "Auto grip unavailable" :
            "Auto grip status unknown";
        label_text(s_motion_reference, reference);
        for (unsigned field = 0; field < 5; ++field) {
            int value = motion_setting((app_motion_field_t)field);
            int minimum = field <= APP_MOTION_FIELD_GAIN_Y ? 25 : 0;
            int maximum = field <= APP_MOTION_FIELD_DEADZONE ? 10000 : 1000;
            if (!settings_known) label_text(s_motion_values[field], "Unavailable");
            else if (field <= APP_MOTION_FIELD_GAIN_Y)
                label_format(s_motion_values[field], "%d.%02d", value / 100, value % 100);
            else if (field == APP_MOTION_FIELD_DEADZONE)
                label_format(s_motion_values[field], "%d.%03d deg/s", value / 1000, value % 1000);
            else label_format(s_motion_values[field], "%d ms", value);
            if (settings_known) slider_value(s_motion_sliders[field], value);
            enabled(s_motion_sliders[field], settings_editable);
            enabled(s_motion_nudges[field][0], settings_editable && value > minimum);
            enabled(s_motion_nudges[field][1], settings_editable && value < maximum);
        }
        const int preset_values[] = {200, 400, 800};
        for (unsigned axis = 0; axis < 2; ++axis) {
            for (unsigned preset = 0; preset < 3; ++preset) {
                selected(s_motion_presets[axis][preset], settings_known && motion_setting((app_motion_field_t)axis) == preset_values[preset]);
                enabled(s_motion_presets[axis][preset], settings_editable);
            }
            bool inverted = axis ? view->motion_settings.invert_y : view->motion_settings.invert_x;
            text(s_motion_invert[axis], !settings_known ? (axis ? "Vertical unavailable" : "Horizontal unavailable") :
                axis ? (inverted ? "Vertical / reversed" : "Vertical / normal") :
                (inverted ? "Horizontal / reversed" : "Horizontal / normal"));
            selected(s_motion_invert[axis], settings_known && inverted);
            enabled(s_motion_invert[axis], settings_editable);
        }
        label_text(s_gesture_drag, view->release_pending ? "Releasing input..." : view->dragging ? "Dragging / hold active" : "Drag released");
        label_format(s_gesture_help, "%s\n\n%s", !view->tap_to_click_available ?
            "Tap gestures unavailable. Use the mouse buttons." : view->tap_to_click_enabled ?
            "Tap to click. Double tap for a double click.\nDouble-tap and hold, then move to drag.\nLift your finger to release a touch drag." :
            "Movement only. Tap gestures are off.\nExplicit mouse buttons still work.",
            view->two_finger_scroll_available ? "Use two fingers to scroll." :
            view->swipe_scroll_available ? "Swipe to scroll." : "Use the Scroll controls for scrolling.");
        enabled(s_gesture_release, view->dragging || view->release_pending);
        return;
    }
    /* The opaque panel/safety surfaces cover the entire core, including its
     * header. Ingest state above, but refresh core presentation only here. */
    const char *mode_names[] = {"Mouse", "Keyboard", "Audio"};
    const char *entry_names[] = {"Motion", "Touch", "Spoken", "Hotkeys"};
    label_text(s_mode_title, (unsigned)view->mode < 3 ? mode_names[view->mode] : "Input");
    if (view->mode != INPUT_AUDIO) label_format(s_context, "%s / %s", usb ? "USB" : "BT", s_target_text);
    critical_state(s_critical, alert, capture, provider_available);
    bool navigation = view->mode == INPUT_MOUSE && view->navigation_enabled;
    /* Revoke a pressed header before hiding it, so LVGL cannot retarget the
     * same held contact onto a newly revealed Home/Back/Recenter control. */
    enabled(s_keyboard_choice, !navigation &&
        (view->mode != INPUT_MOUSE || (view->motion_recenter_available && view->local_access == APP_LOCAL_NORMAL)));
    enabled(s_navigation_home, navigation && view->overlay == APP_OVERLAY_NONE && view->navigation_available);
    enabled(s_navigation_back, navigation && view->overlay == APP_OVERLAY_NONE && view->navigation_available);
    visible(s_keyboard_choice, view->mode == INPUT_KEYBOARD || (view->mode == INPUT_MOUSE && !navigation));
    visible(s_navigation_home, navigation && view->overlay == APP_OVERLAY_NONE);
    visible(s_navigation_back, navigation && view->overlay == APP_OVERLAY_NONE);
    visible(s_host_keys, view->mode == INPUT_KEYBOARD);
    bool compact_header = view->mode == INPUT_KEYBOARD;
    if (compact_header != s_keyboard_header_compact) {
        s_keyboard_header_compact = compact_header;
        lv_obj_set_width(s_keyboard_choice, compact_header ? 76 : 132);
        lv_obj_t *entry_caption = lv_obj_get_child(s_keyboard_choice, 0);
        lv_obj_set_width(entry_caption, compact_header ? 68 : 124);
    }
    visible(s_audio_choice, view->mode == INPUT_AUDIO);
    if (view->mode == INPUT_KEYBOARD)
        text(s_keyboard_choice, (unsigned)view->keyboard_submode < 4 ? entry_names[view->keyboard_submode] : "Entry");
    if (view->mode == INPUT_AUDIO) {
        text(s_audio_choice, media_connection_name(&view->media));
        label_text(s_context, !view->media.known ? "Status unknown" :
            view->media.command_pending ? "Updating" : view->media.state == APP_MEDIA_FAULT ? "Audio error" : "Mic / Speaker");
    }
    for (int i = 0; i < 3; ++i) visible(s_panels[i], (input_mode_t)i == view->mode);

    /* Only the foreground mode is refreshed. Hidden uptime/settings changes
     * cannot invalidate primary input pixels, even with FULL rendering. */
    if (view->mode == INPUT_MOUSE) {
        enabled(s_pad, true);
        text(s_keyboard_choice, "Recenter");
        label_format(s_pad_hint, "%s\n\n%s", motion_reason(view), !view->tap_to_click_available ? "Tap gestures unavailable" : view->tap_to_click_enabled ?
            "Tap / double tap / double-tap hold to drag" : "Tap to click off / movement only");
        text(s_motion, motion_caption(view));
        selected(s_motion, view->motion_enabled);
        enabled(s_motion, view->motion_enabled || (view->motion_available && hid_ready &&
            !view->calibrating && view->motion_reason == APP_MOTION_READY));
        text(s_gain, view->gain_divisor == 4 ? "Fine gain / 1/4x" : view->gain_divisor == 8 ? "Fine gain / 1/8x" : "Fine gain / 1x");
        text(s_drag, view->dragging ? "Release" : "Drag");
        selected(s_drag, view->dragging);
        enabled(s_drag, hid_ready || view->dragging);
        for (unsigned i = 0; i < 4; ++i) enabled(s_mouse_controls[i], hid_ready);
    } else if (view->mode == INPUT_KEYBOARD) {
        label_text(s_status, *s_message_text ? s_message_text :
            view->sending ? "Sending without Enter" : "Local control / preview first / no Enter");
        label_text(s_preview, *view->preview ? view->preview : "Ready for input...");
        if (view->sending || view->send_total)
            label_format(s_progress, "Accepted %u / %u / no Enter", (unsigned)view->accepted_prefix, (unsigned)view->send_total);
        else label_format(s_progress, "%u / 64", (unsigned)bounded_length(view->preview, sizeof(view->preview)));
        enabled(s_clear, !view->sending && *view->preview);
        visible(s_grid_page, view->keyboard_submode <= APP_KEYBOARD_TOUCH);
        visible(s_voice_page, view->keyboard_submode == APP_KEYBOARD_SPOKEN);
        visible(s_shortcut_page, view->keyboard_submode == APP_KEYBOARD_SHORTCUTS);
        if (view->keyboard_submode <= APP_KEYBOARD_TOUCH) {
            enabled(s_back, view->keyboard_group >= 0 && !view->sending);
            text(s_case, view->lowercase ? "abc" : "ABC");
            text(s_symbols, view->symbols ? "ABC/123" : "Symbols");
            enabled(s_case, !view->sending);
            enabled(s_symbols, !view->sending);
            for (unsigned i = 0; i < APP_UI_GRID_CELLS; ++i) {
                text(s_grid[i], view->keyboard_cells[i]);
                bool valid = i < view->keyboard_cell_count && view->keyboard_cells[i][0];
                visible(s_grid[i], valid);
                enabled(s_grid[i], valid && !view->sending);
                bool focused = view->keyboard_submode == APP_KEYBOARD_MOTION && view->keyboard_focus == i;
                background(s_grid[i], focused ? FOCUS : SOFT, 0);
                border(s_grid[i], focused ? ACCENT : SOFT, 0);
            }
        } else if (view->keyboard_submode == APP_KEYBOARD_SPOKEN) {
            render_speech();
        } else if (view->keyboard_submode == APP_KEYBOARD_SHORTCUTS) {
            const char *ctrl[] = {"Ctrl+C", "Ctrl+V", "Ctrl+Z"};
            const char *command[] = {"Cmd+C", "Cmd+V", "Cmd+Z"};
            for (unsigned i = 0; i < 6; ++i) {
                if (i < 3) text(s_shortcuts[i], view->host_os == APP_HOST_COMMAND ? command[i] : ctrl[i]);
                enabled(s_shortcuts[i], hid_ready && !view->sending);
            }
        }
        enabled(s_delete, !view->sending && *view->preview);
        enabled(s_space, !view->sending && bounded_length(view->preview, sizeof(view->preview)) < APP_UI_DRAFT_CAPACITY);
        text(s_send, view->sending ? "Stop sending" : "Send");
        selected(s_send, true);
        enabled(s_send, view->sending || (hid_ready && *view->preview));
    } else if (view->mode == INPUT_AUDIO) {
        render_media_core();
    }
}

uint64_t app_ui_capture_indicator_armed_generation(void)
{
    return app_ui_capture_indicator().capture_generation;
}

app_ui_capture_indicator_t app_ui_capture_indicator(void)
{
    return s_screen && lv_obj_is_visible(s_screen) ? s_capture_indicator : (app_ui_capture_indicator_t){0};
}

void app_ui_observe_contact_count(uint8_t count, bool valid)
{
    /* LVGL receives only the primary point. A second finger may replace
     * that point without a release, even when both fingers are still. */
    if (!valid || count > 1) {
        s_wheel.wait_all_up = true;
        s_wheel.tracking = s_wheel.tap = false;
    } else if (count == 0) {
        s_wheel.wait_all_up = false;
    }
}

app_ui_touch_context_t app_ui_touch_context(void)
{
    app_ui_touch_context_t context = {0};
    if (!s_screen) return context;
    context.epoch = s_view.epoch;
    context.ui_revision = s_view.ui_revision;
    if (!s_pad || s_view.mode != INPUT_MOUSE || s_view.overlay != APP_OVERLAY_NONE ||
        s_overlay || s_view.locked || s_view.screen_off || s_view.remote_owned ||
        !lv_obj_is_visible(s_screen) || !lv_obj_is_visible(s_pad)) return context;
    lv_area_t area;
    lv_obj_get_coords(s_pad, &area);
    if (area.x1 < 0 || area.y1 < 0 || area.x2 >= 480 || area.y2 >= 480 ||
        area.x2 < area.x1 || area.y2 < area.y1) return context;
    context.eligible_pad = true;
    context.x1 = (int16_t)area.x1;
    context.y1 = (int16_t)area.y1;
    context.x2 = (int16_t)area.x2;
    context.y2 = (int16_t)area.y2;
    return context;
}
