// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "input_core.h"
#include "lvgl.h"
#include <stddef.h>

#define APP_UI_DRAFT_CAPACITY 64
#define APP_UI_GRID_CELLS 9
#define APP_UI_BOND_CAPACITY 32
#define APP_UI_BONDS_PER_PAGE 4

typedef enum {
    APP_MOVE, APP_CLICK, APP_SCROLL, APP_TEXT, APP_KEY, APP_MODE,
    APP_LOCK, APP_RELEASE, APP_RECOVER, APP_HAPTICS,
    APP_MOTION, APP_GAIN, APP_DRAG, APP_MENU, APP_CLOSE,
    APP_KEYBOARD_SUBMODE, APP_GRID, APP_BACK, APP_CASE, APP_SYMBOLS,
    APP_DELETE, APP_SPACE, APP_CLEAR, APP_SEND, APP_STOP,
    APP_HOST_OS, APP_SHORTCUT, APP_VOICE_START, APP_VOICE_CONFIRM,
    APP_AUDIO_KIND, APP_AUDIO_POWER, APP_AUDIO_MUTE, APP_AUDIO_VOLUME,
    APP_TOUCH_BEGIN, APP_TOUCH_MOVE, APP_TOUCH_END, APP_TOUCH_CANCEL, APP_CALIBRATE, APP_THEME,
    APP_SETTINGS_PAGE, APP_PAIRING_REQUEST, APP_CONFIRM, APP_WIFI_SETUP,
    APP_WIFI_FORGET_REQUEST, APP_BRIGHTNESS, APP_IDLE_TIMEOUT, APP_BLUETOOTH_PAGE, APP_TRANSPORT_SELECT
} app_action_t;

typedef enum {
    APP_OVERLAY_NONE, APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS,
    APP_OVERLAY_WIFI_SETUP, APP_OVERLAY_CONFIRM,
    APP_OVERLAY_KEYBOARD, APP_OVERLAY_AUDIO
} app_overlay_t;
typedef enum {
    APP_KEYBOARD_MOTION, APP_KEYBOARD_TOUCH, APP_KEYBOARD_SPOKEN, APP_KEYBOARD_SHORTCUTS
} app_keyboard_submode_t;
typedef enum {
    APP_SETTINGS_HOME, APP_SETTINGS_CONNECTIVITY, APP_SETTINGS_DISPLAY,
    APP_SETTINGS_INPUT, APP_SETTINGS_ABOUT, APP_SETTINGS_BLUETOOTH, APP_SETTINGS_WIFI
} app_settings_page_t;
typedef enum {
    APP_PAIR_OPEN, APP_PAIR_CLOSE, APP_PAIR_FORGET_ALL, APP_PAIR_DISCONNECT
} app_pairing_operation_t;
typedef enum {
    APP_CONFIRM_NONE, APP_CONFIRM_PAIR_OPEN, APP_CONFIRM_PAIR_CLOSE,
    APP_CONFIRM_PAIR_FORGET_ALL, APP_CONFIRM_WIFI_FORGET, APP_CONFIRM_DISCONNECT
} app_confirmation_t;
typedef enum {
    APP_PAIRING_UNAVAILABLE, APP_PAIRING_CLOSED, APP_PAIRING_OPEN,
    APP_PAIRING_CLEARING, APP_PAIRING_ERROR
} app_pairing_state_t;
/* UI values intentionally do not share names/types with the Wi-Fi service. */
typedef enum {
    APP_UI_WIFI_UNAVAILABLE, APP_UI_WIFI_DISCONNECTED, APP_UI_WIFI_CONNECTING,
    APP_UI_WIFI_CONNECTED, APP_UI_WIFI_RETRYING, APP_UI_WIFI_ERROR
} app_ui_wifi_state_t;
typedef enum {
    APP_BATTERY_UNKNOWN, APP_BATTERY_CHARGING, APP_BATTERY_DISCHARGING,
    APP_BATTERY_FULL, APP_BATTERY_IDLE
} app_battery_state_t;
typedef enum {
    APP_SAVE_UNAVAILABLE, APP_SAVE_SAVED, APP_SAVE_PENDING, APP_SAVE_ERROR, APP_SAVE_DEFAULTS
} app_save_state_t;
typedef enum { APP_INPUT_BLUETOOTH, APP_INPUT_USB } app_input_transport_t;
typedef enum { APP_HOST_CTRL, APP_HOST_COMMAND } app_host_os_t;
typedef enum { APP_AUDIO_VOICE, APP_AUDIO_SPEAKER } app_audio_kind_t;
typedef enum { APP_SURFACE_CONTROL, APP_SURFACE_TOUCHPAD } app_touch_surface_t;
typedef enum {
    APP_SHORTCUT_COPY, APP_SHORTCUT_PASTE, APP_SHORTCUT_UNDO,
    APP_SHORTCUT_TAB, APP_SHORTCUT_ESCAPE, APP_SHORTCUT_ENTER
} app_shortcut_t;

/* Actual bonded peer data. Name is optional and must be host-reported.
 * Identity is a stable SDK identity, including address type if needed. */
typedef struct {
    char identity[24];
    char name[33];
    bool connected;
} app_bond_view_t;

typedef struct {
    app_action_t action;
    uint64_t epoch;
    uint64_t ui_revision;
    int x;
    int y;
    char text[33];
    uint32_t gesture;
    app_touch_surface_t surface;
} app_intent_t;

/* All state is a read-only snapshot from the coordinator. Strings are copied by
 * render; the caller retains ownership. No UI callback mutates product state.
 * APP_THEME x is 0 (Light) or 1 (Dark), owned by the coordinator.
 * APP_GAIN x is a denominator (1, 4, 8). APP_GRID x is the visible cell (0..8),
 * interpreted by the coordinator's current group/page; only nonempty cells are
 * actionable. TRANSPORT_SELECT x is APP_INPUT_BLUETOOTH or APP_INPUT_USB. It
 * requests one shared input transport for every mode; it never changes mode,
 * duplicates input, or claims audio support. The coordinator owns selection,
 * pending transitions, persistence and readiness. connected/ready/error refer
 * to the currently selected path; Bluetooth-specific fields remain independent.
 * APP_SHORTCUT x is the
 * explicit shortcut enum, including Enter; Send must never imply Enter.
 * TOUCH BEGIN/END coordinates are absolute logical display coordinates; MOVE
 * contains unscaled relative deltas. BEGIN/END/CANCEL cover every touch; MOVE
 * applies only to the touchpad. A gesture id links a
 * later button action to the same press, so local takeover can consume it.
 * ui_revision captures the visible context at press time; reject stale grid or
 * menu actions after a context change. Safety touch boundaries remain valid.
 * Menus and physical double-click are coordinator state, not local navigation.
 * APP_MENU opens the shared quick panel (MODES), Status (TARGET), Settings,
 * or the KEYBOARD/AUDIO choice panel. Choices emit KEYBOARD_SUBMODE/AUDIO_KIND;
 * the coordinator acknowledges the selection and closes that panel.
 * SETTINGS_PAGE x selects app_settings_page_t and opens Settings. APP_CLOSE
 * returns to the primary mode. Settings Home Back emits MENU MODES; nested
 * Back uses SETTINGS_PAGE (HOME/CONNECTIVITY);
 * leaving Wi-Fi setup stops its AP and clears credentials in the coordinator.
 * BLUETOOTH_PAGE x requests a zero-based page of four saved hosts, clamped by
 * the coordinator. No list row sends an outbound connect request.
 * PAIRING_REQUEST x selects app_pairing_operation_t and opens confirmation only;
 * APP_PAIR_DISCONNECT confirms disconnecting the current peer without unpairing.
 * WIFI_FORGET_REQUEST opens confirmation only. CONFIRM x=0 cancels/x=1 approves
 * the coordinator-owned pending operation. Confirmations invalidate revision.
 * WIFI_SETUP x=1 starts/opens protected AP setup; x=0 explicitly stops setup.
 * BRIGHTNESS x=10..100 in steps of 10, emitted on slider release. IDLE_TIMEOUT
 * x=0/60/300/600/1800 seconds. Theme/haptics/modifier/brightness/timeout persist
 * in the coordinator; SAVE_SAVED means persistence was actually acknowledged.
 * Screen-off and wake are coordinator-owned. The first entire touch/AI gesture
 * is consumed there; screen-off UI only emits safety touch boundaries. Never
 * replay a button action from a pre-wake gesture or infer charger attachment.
 */
typedef struct {
    uint64_t epoch;
    uint64_t ui_revision;
    input_mode_t mode;
    app_input_transport_t input_transport; /* Bluetooth is the zero/default value. */
    app_input_transport_t requested_transport; /* Meaningful only while switching. */
    bool transport_switch_pending;
    bool bluetooth_available;
    bool bluetooth_connected;
    bool bluetooth_ready;
    char bluetooth_target_name[48]; /* Actual reported name/identity, optional. */
    bool usb_available; /* Provider capability, never inferred from a cable. */
    bool usb_connected;
    bool usb_ready;
    char transport_message[65];
    bool connected;
    bool ready;
    bool locked;
    bool release_pending;
    bool congested; /* Transport status, not a reason to disable local controls. */
    bool transport_error;
    bool haptics_available;
    bool haptics_enabled;
    bool dark_theme;
    size_t queued;
    uint8_t keyboard_leds;
    const char *message;
    const char *target_name;
    bool remote_owned;
    bool motion_available; /* IMU is ready and calibration is valid. */
    bool calibration_available; /* IMU can attempt calibration, even after failure. */
    bool motion_enabled;
    bool calibrating;
    unsigned gain_divisor;
    bool dragging;
    app_overlay_t overlay;
    app_keyboard_submode_t keyboard_submode;
    int keyboard_group; /* -1 selects groups; 0..8 selects their characters. */
    unsigned keyboard_focus;
    size_t keyboard_cell_count;
    char keyboard_cells[APP_UI_GRID_CELLS][8];
    bool lowercase;
    bool symbols;
    char preview[APP_UI_DRAFT_CAPACITY + 1];
    bool sending;
    size_t accepted_prefix;
    size_t send_total;
    app_host_os_t host_os;
    bool recognition_available;
    bool listening;
    char voice_candidate;
    bool audio_available;
    app_audio_kind_t audio_kind;
    bool audio_active;
    bool microphone_muted;
    unsigned volume;
    app_settings_page_t settings_page;
    app_confirmation_t confirmation;
    bool battery_supported;
    bool battery_percent_valid;
    unsigned battery_percent; /* 0..100 only when valid. */
    app_battery_state_t battery_state; /* Gauge state, never inferred USB presence. */
    app_pairing_state_t pairing_state;
    int paired_host_count; /* -1 means unknown. */
    unsigned paired_host_capacity; /* Actual provider bond capacity; 0 means unknown. */
    unsigned pairing_seconds_left; /* 0 means no known countdown. */
    bool bluetooth_hosts_available;
    size_t bluetooth_host_count; /* 0..APP_UI_BOND_CAPACITY; unknown is not zero. */
    app_bond_view_t bluetooth_hosts[APP_UI_BOND_CAPACITY];
    unsigned bluetooth_page; /* Coordinator-owned, zero-based, four hosts/page. */
    app_ui_wifi_state_t wifi_state;
    bool wifi_rssi_valid;
    int wifi_rssi_dbm;
    bool wifi_saved_network;
    bool wifi_operation_pending;
    char wifi_ssid[33];
    char wifi_ipv4[16];
    char wifi_message[65];
    bool wifi_setup_active;
    char wifi_setup_ssid[33];
    char wifi_setup_passcode[25]; /* Display only; clear when setup closes/ends. */
    char wifi_setup_url[64];
    unsigned wifi_setup_seconds_left;
    bool brightness_available;
    unsigned brightness_percent; /* 10..100 in steps of 10; default 70. */
    bool screen_off_available;
    unsigned idle_timeout_seconds; /* 0,60,300,600,1800; default 300; all power states. */
    bool screen_off; /* Backlight only; radio/HID continue. */
    app_save_state_t settings_save_state;
    char firmware_version[32];
    char build_id[24];
    char board_revision[24];
    char device_id[24];
    uint64_t uptime_seconds;
} app_view_t;

/* Call create/render while holding the display lock. Submit must only enqueue
 * bounded work; it must not render synchronously or block on device I/O. */
typedef bool (*app_submit_fn)(app_intent_t *intent);
void app_ui_create(lv_display_t *display, app_submit_fn submit);
void app_ui_render(const app_view_t *view);
