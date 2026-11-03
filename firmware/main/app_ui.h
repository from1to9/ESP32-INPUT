// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "input_core.h"
#include "app_media_ui.h"
#include "app_connection_ui.h"
#include "app_remote_review_ui.h"
#include "lvgl.h"
#include <stddef.h>

#define APP_UI_DRAFT_CAPACITY 64
#define APP_UI_GRID_CELLS 9
#define APP_UI_BOND_CAPACITY 32
#define APP_UI_BONDS_PER_PAGE 4
#define APP_UI_WIFI_ROWS_PER_PAGE 4
#define APP_UI_WIFI_LABEL_CAPACITY 129
#define APP_UI_WIFI_PASSWORD_CAPACITY 64
#define APP_UI_BLUETOOTH_NAME_CAPACITY 19
#define APP_UI_LOCAL_EDIT_TIMEOUT_MS 300000U

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
    APP_WIFI_FORGET_REQUEST, APP_BRIGHTNESS, APP_IDLE_TIMEOUT, APP_BLUETOOTH_PAGE, APP_TRANSPORT_SELECT,
    APP_WIFI_SCAN, APP_WIFI_SCAN_PAGE, APP_WIFI_NETWORK_SELECT, APP_WIFI_CREDENTIAL_OPEN,
    APP_BLUETOOTH_NAME_EDIT, APP_TAP_TO_CLICK,
    APP_MOTION_CONFIG, APP_RECORDING_STORAGE_PREPARE_REQUEST, APP_MEDIA_CONTROL,
    APP_API_COMMISSIONING, APP_WIFI_PROFILE_CONTROL, APP_BLUETOOTH_TARGET_CONTROL,
    APP_API_INPUT_ALLOW_SET, /* x = explicit Boolean; api.revision = allowance generation. */
    APP_RECENTER /* Reacquire the grip reference; never recalibrate sensor bias. */
} app_action_t;

typedef enum {
    APP_OVERLAY_NONE, APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS,
    APP_OVERLAY_WIFI_SETUP, APP_OVERLAY_CONFIRM,
    APP_OVERLAY_KEYBOARD, APP_OVERLAY_AUDIO,
    APP_OVERLAY_WIFI_NETWORKS, APP_OVERLAY_WIFI_NETWORK, APP_OVERLAY_LOCAL_EDITOR,
    APP_OVERLAY_COMMON_KEYS, APP_OVERLAY_RECORDING_STORAGE,
    APP_OVERLAY_MEDIA_OPTIONS, APP_OVERLAY_RECORDINGS, APP_OVERLAY_RECORDING_DETAIL,
    APP_OVERLAY_RECORDING_START, APP_OVERLAY_API_CONNECTION, APP_OVERLAY_API_REVIEW,
    APP_OVERLAY_WIFI_PROFILES, APP_OVERLAY_BLUETOOTH_TARGET, APP_OVERLAY_BLUETOOTH_PAIRING
} app_overlay_t;
typedef enum {
    APP_KEYBOARD_MOTION, APP_KEYBOARD_TOUCH, APP_KEYBOARD_SPOKEN, APP_KEYBOARD_SHORTCUTS
} app_keyboard_submode_t;
typedef enum {
    APP_SETTINGS_HOME, APP_SETTINGS_CONNECTIVITY, APP_SETTINGS_DISPLAY,
    APP_SETTINGS_INPUT, APP_SETTINGS_ABOUT, APP_SETTINGS_BLUETOOTH, APP_SETTINGS_WIFI,
    APP_SETTINGS_MOTION, APP_SETTINGS_MOTION_ADVANCED, APP_SETTINGS_GESTURES
} app_settings_page_t;
typedef enum {
    APP_PAIR_OPEN, APP_PAIR_CLOSE, APP_PAIR_FORGET_ALL, APP_PAIR_DISCONNECT, APP_PAIR_RESUME
} app_pairing_operation_t;
typedef enum {
    APP_CONFIRM_NONE, APP_CONFIRM_PAIR_OPEN, APP_CONFIRM_PAIR_CLOSE,
    APP_CONFIRM_PAIR_FORGET_ALL, APP_CONFIRM_WIFI_FORGET, APP_CONFIRM_DISCONNECT,
    APP_CONFIRM_RECORDING_STORAGE_PREPARE, APP_CONFIRM_RECORDING_START,
    APP_CONFIRM_RECORDING_DELETE, APP_CONFIRM_RECORDING_CANCEL_DISCARD, APP_CONFIRM_REMOTE,
    APP_CONFIRM_BLUETOOTH_FORGET_ONE, APP_CONFIRM_AUDIO_PAIR
} app_confirmation_t;
typedef enum {
    APP_PAIRING_UNAVAILABLE, APP_PAIRING_CLOSED, APP_PAIRING_OPEN,
    APP_PAIRING_CLEARING, APP_PAIRING_ERROR
} app_pairing_state_t;
/* UI values intentionally do not share names/types with the Wi-Fi service. */
typedef enum {
    APP_UI_WIFI_UNAVAILABLE, APP_UI_WIFI_DISCONNECTED, APP_UI_WIFI_CONNECTING,
    APP_UI_WIFI_CONNECTED, APP_UI_WIFI_RETRYING, APP_UI_WIFI_ERROR,
    APP_UI_WIFI_OBTAINING_IP
} app_ui_wifi_state_t;
typedef enum {
    APP_BATTERY_UNKNOWN, APP_BATTERY_CHARGING, APP_BATTERY_DISCHARGING,
    APP_BATTERY_FULL, APP_BATTERY_IDLE
} app_battery_state_t;
typedef enum {
    APP_SAVE_UNAVAILABLE, APP_SAVE_SAVED, APP_SAVE_PENDING, APP_SAVE_ERROR, APP_SAVE_DEFAULTS, APP_SAVE_VOLATILE
} app_save_state_t;
typedef enum { APP_INPUT_BLUETOOTH, APP_INPUT_USB } app_input_transport_t;
typedef enum { APP_HOST_CTRL, APP_HOST_COMMAND } app_host_os_t;
typedef enum { APP_GIT_UNKNOWN, APP_GIT_CLEAN, APP_GIT_DIRTY } app_git_state_t;
typedef enum { APP_AUDIO_VOICE, APP_AUDIO_SPEAKER } app_audio_kind_t;
typedef enum { APP_SURFACE_CONTROL, APP_SURFACE_TOUCHPAD } app_touch_surface_t;
typedef enum {
    APP_SHORTCUT_COPY, APP_SHORTCUT_PASTE, APP_SHORTCUT_UNDO,
    APP_SHORTCUT_TAB, APP_SHORTCUT_ESCAPE, APP_SHORTCUT_ENTER,
    APP_SHORTCUT_SELECT_ALL
} app_shortcut_t;

typedef enum {
    APP_UI_SCAN_UNAVAILABLE, APP_UI_SCAN_IDLE, APP_UI_SCAN_QUEUED, APP_UI_SCAN_RUNNING,
    APP_UI_SCAN_CANCELLING, APP_UI_SCAN_READY, APP_UI_SCAN_CANCELLED,
    APP_UI_SCAN_ERROR, APP_UI_SCAN_EXPIRED
} app_ui_scan_state_t;
typedef enum {
    APP_UI_SECURITY_UNKNOWN, APP_UI_SECURITY_OPEN, APP_UI_SECURITY_WEP,
    APP_UI_SECURITY_WPA, APP_UI_SECURITY_WPA2, APP_UI_SECURITY_WPA_WPA2,
    APP_UI_SECURITY_WPA3, APP_UI_SECURITY_WPA2_WPA3, APP_UI_SECURITY_ENTERPRISE,
    APP_UI_SECURITY_OWE, APP_UI_SECURITY_OTHER
} app_ui_wifi_security_t;
typedef enum {
    APP_UI_JOIN_UNAVAILABLE, APP_UI_JOIN_AVAILABLE, APP_UI_JOIN_HIDDEN_SSID,
    APP_UI_JOIN_UNSUPPORTED_SSID, APP_UI_JOIN_UNSUPPORTED_SECURITY
} app_ui_join_availability_t;
typedef enum {
    APP_UI_LAN_SERVICE_UNAVAILABLE, APP_UI_LAN_SERVICE_DISABLED,
    APP_UI_LAN_SERVICE_STARTING, APP_UI_LAN_SERVICE_READY, APP_UI_LAN_SERVICE_ERROR
} app_ui_lan_service_state_t;
typedef enum {
    APP_LOCAL_EDIT_NONE, APP_LOCAL_EDIT_WIFI_PASSPHRASE, APP_LOCAL_EDIT_BLUETOOTH_NAME
} app_local_edit_purpose_t;
/* Coordinator-derived admission; UNKNOWN never implies that Motion is ready.
 * Independent link/calibration fields retain simultaneous blockers. */
typedef enum {
    APP_MOTION_UNKNOWN, APP_MOTION_READY, APP_MOTION_SENSOR_UNAVAILABLE,
    APP_MOTION_CALIBRATION_REQUIRED, APP_MOTION_CALIBRATING,
    APP_MOTION_PROVIDER_UNAVAILABLE, APP_MOTION_DISCONNECTED,
    APP_MOTION_INPUT_NOT_READY, APP_MOTION_RELEASING,
    APP_MOTION_TRANSPORT_SWITCHING, APP_MOTION_TRANSPORT_ERROR
} app_motion_reason_t;

typedef enum {
    APP_MOTION_FIELD_GAIN_X, APP_MOTION_FIELD_GAIN_Y, APP_MOTION_FIELD_DEADZONE,
    APP_MOTION_FIELD_FILTER, APP_MOTION_FIELD_FREEZE,
    APP_MOTION_FIELD_INVERT_X, APP_MOTION_FIELD_INVERT_Y
} app_motion_field_t;
typedef struct {
    app_motion_field_t field;
    uint64_t expected_settings_revision;
    union {
        uint16_t gain_centi; /* 25..10000; HID units per degree times 100. */
        uint16_t deadzone_mdps; /* 0..10000 milli-degrees per second. */
        uint16_t milliseconds; /* Filter/freeze: 0..1000. */
        bool inverted;
    } value;
} app_motion_patch_t;
typedef enum {
    APP_REFERENCE_UNKNOWN, APP_REFERENCE_UNAVAILABLE, APP_REFERENCE_ACQUIRING,
    APP_REFERENCE_READY, APP_REFERENCE_UNSTABLE
} app_motion_reference_t;
/* READY requires both gravity and geometry references, with no geometry limit.
 * These are owner-derived coarse states, independent of transient touch/freeze. */
typedef struct {
    bool available; /* Values below are actual validated settings, not defaults. */
    bool editable; /* Owner admits setting changes; independent of HID readiness. */
    uint64_t revision;
    uint16_t gain_x_centi, gain_y_centi;
    uint16_t deadzone_mdps, filter_ms, freeze_ms;
    bool invert_x, invert_y;
    app_motion_reference_t reference;
} app_motion_settings_view_t;
typedef enum {
    APP_CAPTURE_UNKNOWN, APP_CAPTURE_STARTING, APP_CAPTURE_ACTIVE,
    APP_CAPTURE_STOPPING, APP_CAPTURE_UNCERTAIN, APP_CAPTURE_OFF_CONFIRMED
} app_capture_state_t;
typedef enum {
    APP_RECORD_STORE_UNKNOWN, APP_RECORD_STORE_UNPREPARED, APP_RECORD_STORE_PREPARING,
    APP_RECORD_STORE_READY, APP_RECORD_STORE_FULL, APP_RECORD_STORE_ERROR, APP_RECORD_STORE_UNCERTAIN
} app_record_store_state_t;
typedef enum {
    APP_RECORD_BACKEND_UNKNOWN, APP_RECORD_BACKEND_INTERNAL_RESERVED, APP_RECORD_BACKEND_EXTERNAL_NAND
} app_record_backend_t;
typedef struct {
    app_record_store_state_t state;
    app_record_backend_t backend;
    uint64_t generation, revision;
    bool reserved_region_verified; /* Board proves the dedicated internal region. */
    bool can_prepare; /* Owner admission; never inferred from a mount failure. */
    bool capacity_known;
    uint64_t free_bytes, quota_bytes; /* Actual usable quota after reservations. */
    bool remaining_time_known;
    uint32_t remaining_seconds; /* Owner computes for the selected actual profile. */
    char profile[33];
    char reason[97]; /* Safe status only; no filename/path, recording or secret. */
} app_recording_storage_view_t;

/* Safe display labels only. Never parse a label to recover the original SSID. */
typedef struct {
    uint16_t record_index; /* Immutable manager index, not the visible row. */
    char label[APP_UI_WIFI_LABEL_CAPACITY];
    app_ui_wifi_security_t security;
    app_ui_join_availability_t availability;
    bool rssi_valid;
    int rssi_dbm;
    bool current;
    bool remembered;
} app_wifi_row_view_t;

typedef struct {
    app_ui_scan_state_t state;
    uint32_t generation;
    unsigned page;
    uint16_t count; /* Bounded retained count; meaningful only in READY. */
    size_t row_count; /* 0..APP_UI_WIFI_ROWS_PER_PAGE. */
    bool capacity_reached; /* True total nearby is unknown when capped. */
    bool cleanup_pending;
    bool can_start;
    bool can_cancel;
    unsigned seconds_left;
    unsigned retry_seconds;
    char message[65];
    app_wifi_row_view_t rows[APP_UI_WIFI_ROWS_PER_PAGE];
} app_wifi_scan_view_t;

typedef struct {
    bool valid; /* False for expired/replaced/unavailable candidates. */
    uint32_t generation;
    app_wifi_row_view_t network;
    bool can_join_once;
    bool can_remember;
    char message[65];
} app_wifi_selection_view_t;

typedef struct {
    bool available;
    bool configured_valid;
    bool effective_valid;
    bool pending;
    bool uncertain;
    char configured[APP_UI_BLUETOOTH_NAME_CAPACITY];
    char effective[APP_UI_BLUETOOTH_NAME_CAPACITY];
    char requested[APP_UI_BLUETOOTH_NAME_CAPACITY];
    app_save_state_t save_state;
    char message[65];
} app_bluetooth_name_view_t;

/* Public session metadata has no Wi-Fi passphrase. A fresh nonzero session is
 * required for each open/purpose replacement. UI never restores a submitted or
 * expired draft from later snapshots. Deadline enforcement is coordinator-owned. */
typedef struct {
    app_local_edit_purpose_t purpose;
    uint64_t session;
    unsigned seconds_left;
    bool submit_available;
    uint32_t wifi_scan_generation;
    uint16_t wifi_record_index;
    bool wifi_remember;
    char bluetooth_initial[APP_UI_BLUETOOTH_NAME_CAPACITY];
    char message[65]; /* Safe validation/admission status, never credential text. */
} app_local_editor_view_t;

/* Private local submissions do not enter app_intent_t, HID queues, snapshots or
 * logs. Length excludes NUL; the terminator must be at exactly that offset.
 * WIFI: printable ASCII, 8..63 bytes. NAME: valid UTF-8, 1..18 bytes, no NUL,
 * controls or leading/trailing ASCII spaces. Owners validate again on receipt. */
typedef struct {
    app_local_edit_purpose_t purpose;
    uint64_t epoch;
    uint64_t ui_revision;
    uint64_t editor_session;
    uint32_t gesture;
    union {
        struct {
            uint32_t scan_generation;
            uint16_t record_index;
            bool remember;
            size_t length;
            char password[APP_UI_WIFI_PASSWORD_CAPACITY];
        } wifi;
        struct {
            size_t length;
            char name[APP_UI_BLUETOOTH_NAME_CAPACITY];
        } bluetooth;
    } value;
} app_local_edit_request_t;
typedef enum {
    APP_LOCAL_EDIT_ACCEPTED, APP_LOCAL_EDIT_BUSY, APP_LOCAL_EDIT_STALE,
    APP_LOCAL_EDIT_INVALID, APP_LOCAL_EDIT_UNAVAILABLE
} app_local_edit_result_t;

/* Actual bonded peer data. Name is optional and must be host-reported.
 * Identity is a stable SDK identity, including address type if needed. */
typedef struct {
    char identity[24];
    char name[33];
    bool connected;
    bool target_known, selected;
    uint64_t bond_revision, preferences_revision;
    app_bluetooth_target_key_t target;
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
    uint32_t wifi_scan_generation;
    uint16_t wifi_record_index;
    uint64_t event_ms; /* Coordinator stamps monotonic ms at callback admission. */
    app_motion_patch_t motion_patch; /* APP_MOTION_CONFIG only; no x/y alias. */
    uint64_t recording_storage_generation, recording_storage_revision;
    app_api_request_t api;
    app_remote_review_request_t remote_review; /* Exact reviewed request; parent revalidates before APP_CONFIRM. */
    app_saved_wifi_request_t wifi_profile;
    app_bluetooth_target_request_t bluetooth_target;
    app_media_request_t media; /* APP_MEDIA_CONTROL and media APP_CONFIRM; x/y are not aliases. */
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
 * Real touchpad PRESSING emits a zero-delta MOVE at most every 50 ms while
 * stationary, providing physical contact freshness for held-drag safety.
 * event_ms is stamped by the coordinator at callback entry, not queue drain;
 * UI leaves it zero and does not expose its wrapping LVGL tick as monotonic time.
 * ui_revision captures the visible context at press time; reject stale grid or
 * menu actions after a context change. Safety touch boundaries remain valid.
 * Menus and physical double-click are coordinator state, not local navigation.
 * APP_MENU opens the shared quick panel (MODES), Status (TARGET), Settings,
 * or the KEYBOARD/AUDIO choice panel. Choices emit KEYBOARD_SUBMODE/AUDIO_KIND;
 * the coordinator acknowledges the selection and closes that panel.
 * The Keyboard header Keys button opens MENU COMMON_KEYS in every submode.
 * Its Copy/Paste/Undo emit APP_SHORTCUT with the existing shortcut identity;
 * the coordinator selects Ctrl or Command from the acknowledged host setting.
 * Other buttons emit APP_KEY: x is a supported HID usage and y=0; one explicit
 * press/release pair, never an implicit Enter or clipboard read.
 * APP_DELETE only edits the preview; host Backspace/Delete are distinct APP_KEY
 * usages. Host keys never send or edit the local preview.
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
 * WIFI_SCAN x=1 requests scan/opens NETWORKS; x=0 cancels the captured scan.
 * WIFI_SCAN_PAGE x selects a zero-based page of wifi_scan_generation.
 * WIFI_NETWORK_SELECT captures that generation plus the immutable record index.
 * WIFI_CREDENTIAL_OPEN x=0 means Join once, x=1 Remember and join; it opens a
 * fresh editor only after the coordinator revalidates the captured candidate.
 * BLUETOOTH_NAME_EDIT opens a fresh name editor from Bluetooth settings only.
 * Editor keystrokes are local UI state, never APP_GRID/TEXT/SEND. Only its
 * explicit Join/Apply calls the separate private submission callback. Close,
 * Back, AI double, lock, screen-off, timeout and session change clear its draft.
 * TAP_TO_CLICK x=0/1 sets the shared persistent setting from Input settings.
 * Off disables tap/double-tap/hold-drag recognition, not pointer movement or
 * explicit mouse buttons. The Input Engine owns gesture recognition and safety.
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
    bool bluetooth_reconnect_paused;
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
    unsigned brightness_percent; /* 10..100 in steps of 10; default 50. */
    bool screen_off_available;
    unsigned idle_timeout_seconds; /* 0,60,300,600,1800; default 300; all power states. */
    bool screen_off; /* Backlight only; radio/HID continue. */
    app_save_state_t settings_save_state;
    char firmware_version[32];
    char build_id[24];
    char board_revision[24];
    char device_id[24];
    uint64_t uptime_seconds;
    /* Appended extension state; producers must provide truthful capability and
     * session metadata before exposing these new controls. */
    bool tap_to_click_available;
    bool tap_to_click_enabled;
    app_motion_reason_t motion_reason;
    app_wifi_scan_view_t wifi_scan;
    app_wifi_selection_view_t wifi_selection;
    bool wifi_stored_known;
    bool wifi_stored_present;
    bool wifi_stored_valid;
    bool wifi_storage_uncertain;
    char wifi_stored_label[APP_UI_WIFI_LABEL_CAPACITY];
    bool wifi_lan_ready;
    app_ui_lan_service_state_t wifi_lan_service;
    char wifi_lan_service_message[65];
    char wifi_lan_service_url[96]; /* Only meaningful for actual service READY. */
    app_bluetooth_name_view_t bluetooth_name;
    app_local_editor_view_t local_editor;
    app_motion_settings_view_t motion_settings;
    /* Advertise gesture help only for implemented coordinator/driver paths.
     * This does not claim physical multi-touch qualification. */
    bool two_finger_scroll_available;
    bool swipe_scroll_available;
    /* Audio-manager obligation, independent of foreground page/HID ownership.
     * Required includes preparation and unconfirmed hardware stop. */
    bool capture_indicator_required;
    uint64_t capture_indicator_generation;
    app_capture_state_t capture_state;
    app_recording_storage_view_t recording_storage;
    app_api_view_t api;
    app_remote_review_view_t remote_review;
    app_saved_wifi_view_t saved_wifi;
    app_bluetooth_target_view_t bluetooth_target;
    app_bluetooth_management_view_t bluetooth_management;
    app_media_view_t media;
    app_media_review_t media_review;
    bool motion_recenter_available; /* Coordinator admits a local Mouse Recenter request. */
    char developer[32];
    char repository_url[96];
    char git_commit[41]; /* Full 40-digit hexadecimal commit, distinct from build_id. */
    app_git_state_t git_state;
} app_view_t;

/* Call create/render while holding the display lock. Submit must only enqueue
 * bounded work; it must not render synchronously or block on device I/O. */
typedef bool (*app_submit_fn)(app_intent_t *intent);
/* Called with a temporary private request while holding the display lock.
 * Copy into one bounded coordinator-owned slot before returning ACCEPTED; do
 * not retain this pointer, perform I/O, render, or claim the operation succeeded.
 * UI securely clears its request after every return and its draft on ACCEPTED
 * or STALE. Rejected valid drafts remain local for deliberate retry only.
 * Registration must precede exposing a local editor. NULL disables submission.
 * Coordinator cancellation also securely clears its pending private slot. */
typedef app_local_edit_result_t (*app_local_edit_submit_fn)(const app_local_edit_request_t *request);
void app_ui_create(lv_display_t *display, app_submit_fn submit);
void app_ui_set_local_edit_submit(app_local_edit_submit_fn submit);
void app_ui_render(const app_view_t *view);
/* Under the display lock: generation whose warning is armed on the current
 * visible object tree. Zero means absent/hidden/unknown generation. This is
 * NOT a flush, panel-transfer or readable-presentation acknowledgement. The
 * presenter/display owner must correlate the completed frame before reporting
 * indicator_visible/indicator_seen_ms to the audio manager. Never use publish
 * time, render-call return, or this value alone as that acknowledgement. */
uint64_t app_ui_capture_indicator_armed_generation(void);
typedef struct {
    bool armed;
    uint64_t capture_generation, indicator_revision;
    int16_t x1, y1, x2, y2; /* Inclusive logical region containing the warning/Stop. */
} app_ui_capture_indicator_t;
/* Read-only under the LVGL lock. A continuous privacy marker at the same
 * region keeps its revision across ordinary page/status/theme changes. Hiding,
 * relocating or replacing the capture generation arms a fresh revision and
 * invalidates its entire region. No framebuffer or transfer is acknowledged. */
app_ui_capture_indicator_t app_ui_capture_indicator(void);

typedef struct {
    uint64_t epoch, ui_revision; /* The visible snapshot, not a newer owner state. */
    bool eligible_pad;
    int16_t x1, y1, x2, y2; /* Inclusive LVGL logical bounds, valid only if eligible. */
} app_ui_touch_context_t;
/* Read-only, under the existing LVGL lock before the normal primary events.
 * Reports only local, visible, unlocked Mouse pad geometry; no domain gesture
 * recognition, I/O, event submission, allocation or layout update. Before valid
 * layout, while covered or after teardown, eligible_pad is false. The owner
 * still applies wake, authority, freshness, release and preference guards.
 * Put complete driver contact snapshots in the coordinator's queue envelope
 * before app_intent_t primary events; no APP_CONTACTS/UI dependency is needed. */
app_ui_touch_context_t app_ui_touch_context(void);

/* Called by the touch reader before LVGL processes its primary pointer. */
void app_ui_observe_contact_count(uint8_t count, bool valid);
