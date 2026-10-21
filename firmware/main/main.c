// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include "ble_hid.h"
#include "board_port.h"
#include "hid_engine.h"
#include "interaction.h"
#include "device_settings.h"
#include "settings_store.h"
#include "app_wifi.h"
#include "usb_console.h"
#include "usb_hid.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#define INTENT_CAPACITY 16
#define COORDINATOR_PERIOD_MS 10
#define LOCK_REQUEST 1U
#define RELEASE_REQUEST 2U
#define PHYSICAL_LONG_PRESS 4U

static const char *TAG = "input";
/* Fixed boot observations survive late CDC attachment. They never allocate or
 * change admission policy. The failure hook can run in a low-level allocator:
 * publish once with lock-free atomics, without logging, locks or other calls. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Allocation diagnostics require lock-free integers");
static atomic_uint s_allocation_failure_state;
static size_t s_allocation_failure_size;
static uint32_t s_allocation_failure_caps;
static const char *s_allocation_failure_function;
typedef struct {
    const char *stage;
    size_t free_internal, largest_internal, free_dma, largest_dma;
} boot_memory_t;
static boot_memory_t s_boot_memory[6];
static size_t s_boot_memory_count;

static void IRAM_ATTR record_allocation_failure(size_t size, uint32_t caps, const char *function)
{
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_allocation_failure_state, &expected, 1,
                                                 memory_order_relaxed, memory_order_relaxed)) return;
    s_allocation_failure_size = size;
    s_allocation_failure_caps = caps;
    s_allocation_failure_function = function;
    atomic_store_explicit(&s_allocation_failure_state, 2, memory_order_release);
}

static void record_boot_memory(const char *stage)
{
    if (s_boot_memory_count >= sizeof(s_boot_memory) / sizeof(s_boot_memory[0])) return;
    s_boot_memory[s_boot_memory_count++] = (boot_memory_t){
        .stage = stage,
        .free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .free_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    };
}

static void report_memory_diagnostics(void)
{
    /* Repeat the bounded boot history so a late serial reader can see it. */
    for (size_t i = 0; i < s_boot_memory_count; ++i) {
        const boot_memory_t *sample = &s_boot_memory[i];
        ESP_LOGI(TAG, "Boot memory stage=%s free_internal=%u largest_internal=%u free_dma=%u largest_dma=%u",
                 sample->stage, (unsigned)sample->free_internal, (unsigned)sample->largest_internal,
                 (unsigned)sample->free_dma, (unsigned)sample->largest_dma);
    }
    if (atomic_load_explicit(&s_allocation_failure_state, memory_order_acquire) == 2) {
        ESP_LOGE(TAG, "First allocation failure size=%u caps=0x%08x function=%s",
                 (unsigned)s_allocation_failure_size, (unsigned)s_allocation_failure_caps,
                 s_allocation_failure_function ? s_allocation_failure_function : "unknown");
    }
}

static QueueHandle_t s_intents;
static StaticQueue_t s_intent_storage;
static uint8_t s_intent_bytes[INTENT_CAPACITY * sizeof(app_intent_t)];
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_priority_requests;
static app_intent_t s_touch_emergency[2];
static bool s_touch_emergency_pending[2];
static hid_engine_t s_engine;
static interaction_t s_interaction;
static const char *s_message = "Keep still to calibrate. Pair with ESP32-INPUT.";
static app_overlay_t s_overlay;
static app_keyboard_submode_t s_keyboard_submode;
static app_host_os_t s_host_os;
static app_audio_kind_t s_audio_kind;
static bool s_dark_theme = true;
static device_settings_t s_settings;
static app_input_transport_t s_input_transport, s_requested_transport;
static bool s_transport_switch_pending;
static uint64_t s_transport_switch_deadline_ms;
static uint64_t s_transport_origin_generation;
typedef struct {
    app_input_transport_t kind;
    bool initialized, connected, ready, congested, failed, suspended;
    uint8_t keyboard_leds;
    uint64_t generation;
} input_link_t;
static device_idle_t s_idle;
static app_settings_page_t s_settings_page;
static unsigned s_bluetooth_page;
static app_confirmation_t s_confirmation;
static uint64_t s_confirmation_generation;
static char s_confirmation_ssid[33];
static char s_confirmation_stored_ssid[33];
static bool s_confirmation_storage_known, s_confirmation_storage_present;
static bool s_confirmation_storage_uncertain;
static app_confirmation_t s_pair_operation;
static uint64_t s_pair_deadline_ms;
static uint64_t s_pair_origin_generation;
static bool s_pair_disconnect_requested;
static bool s_screen_accepts_input = true;
static int s_brightness_requested = -1;
static uint64_t s_touch_token, s_ai_token;
static uint32_t s_idle_touch_gesture;
static uint64_t s_user_activity_ms, s_seen_activity_ms;
static char s_firmware_version[32], s_build_id[24], s_device_id[24];
static uint64_t s_ui_revision = 1;
static uint64_t s_now_ms;
static uint64_t s_send_token, s_send_epoch;
static size_t s_send_base;
static bool s_send_batch;
static bool s_drag_requested;
static bool s_touch_down;
static bool s_physical_down;
static uint32_t s_touch_gesture, s_consumed_gesture;
static uint32_t s_live_touch_gesture;
static uint64_t s_touch_transition_ms;
static uint64_t s_last_imu_us;
typedef struct { bool down; uint64_t time_ms; } button_edge_t;
static button_edge_t s_button_edges[8];
static unsigned s_button_head, s_button_count;
static bool s_button_overflow;
static bool s_button_resync;
static uint64_t s_button_context_revision, s_button_context_epoch;

static void update_send(bool cancel);
static void sync_interaction(void);

static void physical_button(board_button_event_t event, uint64_t timestamp_us)
{
    portENTER_CRITICAL(&s_control_lock);
    s_physical_down = event == BOARD_BUTTON_DOWN;
    s_user_activity_ms = timestamp_us / 1000U;
    if (s_button_count < 8) {
        unsigned tail = (s_button_head + s_button_count) % 8;
        s_button_edges[tail] = (button_edge_t){s_physical_down, timestamp_us / 1000U};
        ++s_button_count;
    } else {
        /* Lost button edges cannot become a delayed click or an unlock. */
        s_priority_requests |= LOCK_REQUEST;
        s_button_overflow = true;
    }
    portEXIT_CRITICAL(&s_control_lock);
}

static void report_boot_fault(const char *operation, esp_err_t error)
{
    /* USB enumeration and terminal attachment can occur after initialization.
     * Keep a slow, bounded-memory fault report available; never start HID. */
    for (;;) {
        ESP_LOGE(TAG, "Boot fault: %s: %s; HID disabled", operation, esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static bool submit_intent(app_intent_t *intent)
{
    if (intent->action == APP_TOUCH_BEGIN || intent->action == APP_TOUCH_END ||
        intent->action == APP_TOUCH_CANCEL) {
        portENTER_CRITICAL(&s_control_lock);
        if (intent->action == APP_TOUCH_BEGIN || intent->gesture == s_live_touch_gesture) {
            s_touch_down = intent->action == APP_TOUCH_BEGIN;
            if (s_touch_down) s_live_touch_gesture = intent->gesture;
            s_touch_transition_ms = (uint64_t)esp_timer_get_time() / 1000U;
            s_user_activity_ms = s_touch_transition_ms;
        }
        portEXIT_CRITICAL(&s_control_lock);
    }
    unsigned priority = intent->action == APP_LOCK ? LOCK_REQUEST :
                        intent->action == APP_RELEASE ? RELEASE_REQUEST : 0;
    portENTER_CRITICAL(&s_control_lock);
    bool screen_accepts = s_screen_accepts_input;
    portEXIT_CRITICAL(&s_control_lock);
    /* Touch controls must pass the coordinator's complete gesture policy.
     * A delayed action can arrive after the screen is bright again. */
    if (priority && screen_accepts && !intent->gesture) {
        portENTER_CRITICAL(&s_control_lock);
        s_priority_requests |= priority;
        portEXIT_CRITICAL(&s_control_lock);
        return true;
    }
    if (xQueueSend(s_intents, intent, 0) == pdTRUE) {
        return true;
    }
    if (priority && intent->gesture) {
        /* Preserve a real Lock under queue pressure without bypassing wake
         * capture. Each emergency kind has one bounded coordinator-owned slot. */
        unsigned slot = intent->action == APP_LOCK ? 0U : 1U;
        portENTER_CRITICAL(&s_control_lock);
        s_touch_emergency[slot] = *intent;
        s_touch_emergency_pending[slot] = true;
        portEXIT_CRITICAL(&s_control_lock);
        return true;
    }
    /* Overflow cannot discard an emergency release. The coordinator cancels
     * the saturated producer queue before accepting a new input epoch. */
    portENTER_CRITICAL(&s_control_lock);
    s_priority_requests |= RELEASE_REQUEST;
    portEXIT_CRITICAL(&s_control_lock);
    return false;
}

static void physical_lock(void)
{
    /* Only the board callback can request unlock. Never expose this request
     * through touch or future network intents. Coalesced holds toggle once. */
    portENTER_CRITICAL(&s_control_lock);
    s_priority_requests |= PHYSICAL_LONG_PRESS;
    portEXIT_CRITICAL(&s_control_lock);
}

static bool send_report(void *context, uint8_t report_id,
                         const uint8_t *data, size_t length)
{
    const uint64_t *generation = context;
    return s_input_transport == APP_INPUT_USB ?
        usb_hid_send_generation(*generation, report_id, data, length) :
        ble_hid_send_generation(*generation, report_id, data, length);
}

static input_link_t input_link_snapshot(const ble_hid_snapshot_t *bluetooth)
{
    if (s_input_transport == APP_INPUT_USB) {
        usb_hid_snapshot_t usb = usb_hid_snapshot();
        return (input_link_t){.kind = APP_INPUT_USB, .initialized = usb.initialized,
            .connected = usb.mounted, .ready = usb.ready, .congested = usb.congested,
            .failed = usb.failed, .suspended = usb.suspended,
            .keyboard_leds = usb.keyboard_leds, .generation = usb.generation};
    }
    return (input_link_t){.kind = APP_INPUT_BLUETOOTH, .initialized = bluetooth->initialized,
        .connected = bluetooth->connected, .ready = bluetooth->ready,
        .congested = bluetooth->congested, .failed = bluetooth->failed,
        .suspended = bluetooth->suspended, .keyboard_leds = bluetooth->keyboard_leds,
        .generation = bluetooth->generation};
}

static void service_usb_selection(void)
{
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    /* A real USB reset may clear a quarantined endpoint while retaining the
     * selected transport. Never rearm while failed. The caller must observe
     * the new generation, discard old work and require fresh zero reports. */
    if (s_input_transport == APP_INPUT_USB && usb.initialized && !usb.failed && !usb.enabled)
        usb_hid_set_enabled(true);
}

static void sync_interaction(void)
{
    interaction_sync(&s_interaction, &s_engine.state,
        s_engine.ready && !s_engine.state.release_pending && !s_engine.congested, s_now_ms);
    bool touch, physical;
    uint64_t transition;
    portENTER_CRITICAL(&s_control_lock);
    touch = s_touch_down;
    physical = s_physical_down;
    transition = s_touch_transition_ms;
    portEXIT_CRITICAL(&s_control_lock);
    interaction_set_suppressed(&s_interaction, touch || physical,
        s_overlay != APP_OVERLAY_NONE || s_transport_switch_pending ||
            (s_engine.state.mode == INPUT_KEYBOARD && s_idle.asleep),
        board_haptics_active() || s_idle.wake_capture, s_now_ms);
    if (transition && transition <= UINT64_MAX - INTERACTION_FREEZE_MS)
        interaction_freeze_until(&s_interaction, transition + INTERACTION_FREEZE_MS);
}

static void cancel_input(void)
{
    update_send(true);
    hid_engine_cancel(&s_engine);
    s_drag_requested = false;
    ++s_ui_revision;
    sync_interaction();
}

static bool enqueue_draft_chunk(void)
{
    size_t length = s_interaction.view.preview_length;
    if (length > HID_ENGINE_TEXT_MAX_LENGTH) length = HID_ENGINE_TEXT_MAX_LENGTH;
    if (!length) return false;
    s_send_batch = hid_engine_enqueue_text(&s_engine, s_engine.state.epoch,
        INPUT_LOCAL, s_interaction.view.preview, length) == HID_ENGINE_ENQUEUE_OK;
    return s_send_batch;
}

static void update_send(bool cancel)
{
    if (!s_send_token) return;
    size_t accepted = s_send_base + (s_send_batch ? s_engine.text_accepted : 0);
    bool valid = !cancel && s_engine.state.epoch == s_send_epoch &&
        s_engine.ready && !s_engine.state.locked && s_engine.state.mode == INPUT_KEYBOARD;
    bool finished_batch = s_send_batch && s_engine.text_pending == 0;
    bool active = valid && (s_engine.text_pending != 0 ||
        accepted < s_interaction.view.send_total);
    (void)interaction_send_update(&s_interaction, s_send_token, accepted, active);
    if (active && finished_batch) {
        s_send_base = accepted;
        if (!enqueue_draft_chunk()) {
            (void)interaction_send_update(&s_interaction, s_send_token, accepted, false);
            active = false;
            s_message = "Sending stopped; unsent characters remain in preview.";
        }
    }
    if (!active) {
        s_send_token = 0;
        s_send_batch = false;
    }
}

static void open_overlay(app_overlay_t overlay)
{
    cancel_input();
    if (s_overlay == APP_OVERLAY_WIFI_SETUP && overlay != APP_OVERLAY_WIFI_SETUP)
        (void)app_wifi_stop_setup();
    s_overlay = overlay;
    sync_interaction();
}

static void toggle_motion(void)
{
    bool enable = !s_engine.state.gyro_enabled;
    if (enable && !s_interaction.view.calibrated) {
        s_message = "Keep still to calibrate motion first.";
        return;
    }
    if (hid_engine_set_gyro(&s_engine, enable)) {
        (void)interaction_set_motion(&s_interaction, enable);
        s_message = enable ? "Motion on. Touch pauses motion." : "Motion off.";
        board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
    }
}

static bool update_settings(const device_settings_t *candidate)
{
    device_settings_apply_t result = device_settings_apply(&s_settings, candidate);
    if (result == DEVICE_SETTINGS_APPLY_INVALID) return false;
    if (result == DEVICE_SETTINGS_APPLY_UNCHANGED) return true;
    s_dark_theme = s_settings.theme == DEVICE_THEME_DARK;
    s_host_os = s_settings.host_modifier == DEVICE_HOST_COMMAND ? APP_HOST_COMMAND : APP_HOST_CTRL;
    board_haptics_enable(s_settings.haptics_enabled);
    (void)device_idle_configure(&s_idle, s_settings.idle_timeout, s_now_ms);
    (void)device_idle_activity(&s_idle, s_now_ms);
    if (settings_store_request(&s_settings) != ESP_OK)
        s_message = "Setting applied; storage unavailable.";
    ++s_ui_revision;
    return true;
}

static void service_display(void)
{
    portENTER_CRITICAL(&s_control_lock);
    bool held = s_touch_down || s_physical_down;
    uint64_t activity = s_user_activity_ms;
    portEXIT_CRITICAL(&s_control_lock);
    if (activity != s_seen_activity_ms) {
        s_seen_activity_ms = activity;
        /* The callback is physical evidence even when a safety queue reset
         * discarded its UI event. It cannot wake without a routed BEGIN. */
        device_idle_note_physical_activity(&s_idle, activity);
    }
    bool adjustable = board_capabilities().brightness;
    if (!adjustable) (void)device_idle_configure(&s_idle, DEVICE_IDLE_OFF, s_now_ms);
    else if (s_idle.timeout != s_settings.idle_timeout)
        (void)device_idle_configure(&s_idle, s_settings.idle_timeout, s_now_ms);
    if (!held) (void)device_idle_tick(&s_idle, s_now_ms);
    int desired = s_idle.asleep ? 0 : (int)s_settings.brightness_percent;
    if (adjustable && desired != s_brightness_requested) {
        s_brightness_requested = desired;
        if (board_brightness_set(desired) != ESP_OK)
            s_message = "Display brightness request failed.";
    }
    board_brightness_status_t brightness = board_brightness_status();
    bool accepts = !s_idle.asleep && !s_idle.wake_capture &&
        (!adjustable || (brightness.applied_known && brightness.applied_percent > 0 && brightness.error == ESP_OK));
    portENTER_CRITICAL(&s_control_lock);
    s_screen_accepts_input = accepts;
    portEXIT_CRITICAL(&s_control_lock);
}

static bool route_touch(const app_intent_t *intent)
{
    if (!intent->gesture) return !s_idle.asleep && !s_idle.wake_capture;
    device_input_phase_t phase = DEVICE_INPUT_ACTION;
    if (intent->action == APP_TOUCH_BEGIN) {
        if (intent->gesture == s_idle_touch_gesture || s_touch_token == UINT64_MAX) return false;
        s_idle_touch_gesture = intent->gesture;
        ++s_touch_token;
        phase = DEVICE_INPUT_BEGIN;
    } else {
        if (intent->gesture != s_idle_touch_gesture) return false;
        if (intent->action == APP_TOUCH_MOVE) phase = DEVICE_INPUT_MOVE;
        else if (intent->action == APP_TOUCH_END) phase = DEVICE_INPUT_END;
        else if (intent->action == APP_TOUCH_CANCEL) phase = DEVICE_INPUT_CANCEL;
    }
    bool was_asleep = s_idle.asleep;
    device_input_route_t route = device_idle_input(&s_idle, DEVICE_INPUT_TOUCH,
        phase, s_touch_token, s_now_ms);
    if (was_asleep && !s_idle.asleep) {
        ++s_ui_revision;
        interaction_consume_press(&s_interaction);
    }
    if (route != DEVICE_INPUT_PASS) return false;
    /* The worker must actually illuminate the display before a fresh gesture
     * can activate a control; an accepted brightness request is not enough. */
    board_brightness_status_t brightness = board_brightness_status();
    if (phase == DEVICE_INPUT_BEGIN && board_capabilities().brightness &&
        (!brightness.applied_known || brightness.applied_percent == 0 || brightness.error != ESP_OK)) {
        /* A fresh physical wake gesture may retry a failed driver operation;
         * ordinary polling never floods the worker with retries. */
        (void)board_brightness_set((int)s_settings.brightness_percent);
        (void)device_idle_input(&s_idle, DEVICE_INPUT_TOUCH, DEVICE_INPUT_CANCEL,
            s_touch_token, s_now_ms);
        return false;
    }
    return true;
}

static bool allowed_in_overlay(app_action_t action)
{
    if (s_overlay == APP_OVERLAY_NONE) return true;
    if (s_overlay == APP_OVERLAY_CONFIRM) return action == APP_CONFIRM;
    if (action == APP_CLOSE) return true;
    if (action == APP_MENU)
        return s_overlay == APP_OVERLAY_MODES || s_overlay == APP_OVERLAY_TARGET ||
            s_overlay == APP_OVERLAY_SETTINGS || s_overlay == APP_OVERLAY_WIFI_SETUP ||
            s_overlay == APP_OVERLAY_KEYBOARD || s_overlay == APP_OVERLAY_AUDIO;
    if (s_overlay == APP_OVERLAY_MODES) return action == APP_MODE;
    if (s_overlay == APP_OVERLAY_KEYBOARD) return action == APP_KEYBOARD_SUBMODE;
    if (s_overlay == APP_OVERLAY_AUDIO) return action == APP_AUDIO_KIND;
    if (s_overlay == APP_OVERLAY_WIFI_SETUP)
        return action == APP_WIFI_SETUP || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_TARGET)
        return action == APP_RECOVER || action == APP_PAIRING_REQUEST || action == APP_SETTINGS_PAGE;
    if (s_overlay != APP_OVERLAY_SETTINGS) return false;
    if (action == APP_SETTINGS_PAGE) return true;
    switch (s_settings_page) {
    case APP_SETTINGS_INPUT:
        return action == APP_HAPTICS || action == APP_HOST_OS || action == APP_CALIBRATE;
    case APP_SETTINGS_DISPLAY:
        return action == APP_THEME || action == APP_BRIGHTNESS || action == APP_IDLE_TIMEOUT;
    case APP_SETTINGS_CONNECTIVITY:
        return action == APP_TRANSPORT_SELECT;
    case APP_SETTINGS_BLUETOOTH:
        return action == APP_PAIRING_REQUEST || action == APP_BLUETOOTH_PAGE;
    case APP_SETTINGS_WIFI:
        return action == APP_WIFI_SETUP || action == APP_WIFI_FORGET_REQUEST;
    default: return false;
    }
}

static void service_pair_operation(const ble_hid_snapshot_t *link)
{
    if (s_pair_operation == APP_CONFIRM_NONE) return;
    /* This runs after the HID step, which may have just admitted a release. */
    ble_hid_snapshot_t current = ble_hid_snapshot();
    link = &current;
    /* Our accepted disconnect revokes the adapter generation immediately,
     * before its asynchronous link-down event. Do not retarget another peer:
     * once submitted, wait for link-down or the operation deadline. */
    if (link->connected && !s_pair_disconnect_requested &&
        link->generation != s_pair_origin_generation) {
        s_pair_operation = APP_CONFIRM_NONE;
        s_message = "Bluetooth changed; review it before trying again.";
        return;
    }
    if (s_now_ms >= s_pair_deadline_ms || link->failed) {
        s_pair_operation = APP_CONFIRM_NONE;
        s_message = "Pairing change failed; check Bluetooth status.";
        return;
    }
    if (link->connected) {
        if (!s_engine.state.release_pending && !link->congested && !s_pair_disconnect_requested)
            s_pair_disconnect_requested = ble_hid_disconnect_generation(link->generation);
        return;
    }
    if (s_pair_operation == APP_CONFIRM_DISCONNECT) {
        s_pair_operation = APP_CONFIRM_NONE;
        s_message = "Bluetooth disconnected; saved pairing retained.";
        return;
    }
    bool accepted = s_pair_operation == APP_CONFIRM_PAIR_FORGET_ALL ?
        ble_hid_forget_bonds(link->generation) : ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS);
    if (accepted) {
        s_message = s_pair_operation == APP_CONFIRM_PAIR_FORGET_ALL ?
            "Forgetting saved hosts; see pairing status." : "Pairing window opened for two minutes.";
        s_pair_operation = APP_CONFIRM_NONE;
    }
}

static void service_transport_switch(void)
{
    if (!s_transport_switch_pending) return;
    /* Never reuse the pre-send snapshot: the final release can have entered
     * the adapter during this iteration and still be awaiting completion. */
    ble_hid_snapshot_t current = ble_hid_snapshot();
    const ble_hid_snapshot_t *bluetooth = &current;
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    bool target_available = s_requested_transport == APP_INPUT_USB ?
        usb.initialized && !usb.failed : bluetooth->initialized && !bluetooth->failed;
    input_link_t old = input_link_snapshot(bluetooth);
    if (old.connected && old.generation != s_transport_origin_generation) {
        s_transport_switch_pending = false;
        s_requested_transport = s_input_transport;
        ++s_ui_revision;
        s_message = "Old connection changed; review it before switching again.";
        return;
    }
    if (!target_available || s_now_ms >= s_transport_switch_deadline_ms) {
        s_transport_switch_pending = false;
        s_requested_transport = s_input_transport;
        ++s_ui_revision;
        s_message = !target_available ? "Requested connection is unavailable; selection unchanged." :
            "Switch stopped. Resume or disconnect the old host, then try again.";
        return;
    }
    /* A disconnected old host cannot accept releases; explicit selection is
     * allowed without claiming they arrived. Otherwise wait for BOTH report
     * admission and adapter completion, including its final zero report. */
    if (old.connected && (s_engine.state.release_pending || old.congested ||
                          old.suspended || old.failed)) return;
    bool old_disconnected = !old.connected;
    hid_engine_set_link(&s_engine, false, false);
    usb_hid_set_enabled(s_requested_transport == APP_INPUT_USB);
    s_input_transport = s_requested_transport;
    s_transport_switch_pending = false;
    device_settings_t candidate = s_settings;
    candidate.preferred_transport = s_input_transport == APP_INPUT_USB ?
        DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH;
    (void)update_settings(&candidate);
    ++s_ui_revision;
    s_message = old_disconnected ?
        "Connection selected; old host was disconnected. Pending input discarded." :
        "Connection selected; previous input released. Motion stays off.";
    sync_interaction();
}

static void handle_intent(const app_intent_t *intent, const ble_hid_snapshot_t *link)
{
    if (!route_touch(intent)) { sync_interaction(); return; }
    if (intent->action == APP_TOUCH_BEGIN) {
        s_touch_gesture = intent->gesture;
        if (s_engine.state.owner == INPUT_REMOTE) {
            s_consumed_gesture = intent->gesture;
            update_send(true);
            (void)hid_engine_set_owner(&s_engine, INPUT_LOCAL);
            s_drag_requested = false;
            ++s_ui_revision;
            s_message = "Local control restored; remote work stopped.";
        }
        sync_interaction();
        return;
    }
    if (intent->action == APP_TOUCH_END || intent->action == APP_TOUCH_CANCEL) {
        if (intent->action == APP_TOUCH_CANCEL) cancel_input();
        sync_interaction();
        return;
    }
    if (intent->epoch != s_engine.state.epoch || intent->ui_revision != s_ui_revision ||
        (intent->gesture && intent->gesture == s_consumed_gesture)) return;
    if (intent->action == APP_LOCK || intent->action == APP_RELEASE) {
        portENTER_CRITICAL(&s_control_lock);
        s_priority_requests |= intent->action == APP_LOCK ? LOCK_REQUEST : RELEASE_REQUEST;
        portEXIT_CRITICAL(&s_control_lock);
        return;
    }
    if (s_engine.state.locked) return;
    if (s_transport_switch_pending) return;
    if (s_engine.state.mode != INPUT_KEYBOARD &&
        (intent->action == APP_BACK || intent->action == APP_CASE ||
         intent->action == APP_SYMBOLS || intent->action == APP_DELETE ||
         intent->action == APP_SPACE || intent->action == APP_CLEAR ||
         intent->action == APP_SEND || intent->action == APP_STOP)) return;
    /* A modal and its settings page own focus. */
    if (!allowed_in_overlay(intent->action)) return;
    hid_engine_enqueue_result_t result = HID_ENGINE_ENQUEUE_BLOCKED;
    bool edited = false;
    switch (intent->action) {
    case APP_MODE:
        if (intent->x < INPUT_MOUSE || intent->x > INPUT_AUDIO) return;
        if ((input_mode_t)intent->x == s_engine.state.mode) {
            /* Re-selecting the current mode is not a send cancellation. A
             * chooser already cancelled input when it was opened. */
            if (s_overlay != APP_OVERLAY_NONE) {
                s_overlay = APP_OVERLAY_NONE;
                ++s_ui_revision;
                sync_interaction();
            }
            return;
        }
        update_send(true);
        if (hid_engine_set_mode(&s_engine, (input_mode_t)intent->x)) {
            s_drag_requested = false;
            s_overlay = APP_OVERLAY_NONE;
            ++s_ui_revision;
            sync_interaction();
            s_message = intent->x == INPUT_AUDIO ? "Audio is not available on either connection yet." :
                "Mode changed; previous input released.";
            board_haptic_feedback(BOARD_FEEDBACK_MODE);
        }
        return;
    case APP_MENU:
        if ((intent->x >= APP_OVERLAY_MODES && intent->x <= APP_OVERLAY_SETTINGS) ||
            (intent->x == APP_OVERLAY_KEYBOARD && s_overlay == APP_OVERLAY_NONE &&
             s_engine.state.mode == INPUT_KEYBOARD) ||
            (intent->x == APP_OVERLAY_AUDIO && s_overlay == APP_OVERLAY_NONE &&
             s_engine.state.mode == INPUT_AUDIO)) {
            if (intent->x == APP_OVERLAY_SETTINGS) s_settings_page = APP_SETTINGS_HOME;
            open_overlay((app_overlay_t)intent->x);
        }
        return;
    case APP_CLOSE:
        if (s_overlay == APP_OVERLAY_WIFI_SETUP) (void)app_wifi_stop_setup();
        s_overlay = APP_OVERLAY_NONE;
        ++s_ui_revision;
        sync_interaction();
        return;
    case APP_RECOVER:
        cancel_input();
        {
            input_link_t active = input_link_snapshot(link);
            hid_engine_set_link(&s_engine, active.connected, active.ready);
        }
        s_message = "Link recovery requested; old input will not replay.";
        return;
    case APP_HAPTICS:
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_INPUT ||
            (intent->x != 0 && intent->x != 1)) return;
        {
            device_settings_t candidate = s_settings;
            candidate.haptics_enabled = intent->x != 0;
            (void)update_settings(&candidate);
        }
        return;
    case APP_THEME:
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_DISPLAY) return;
        if (intent->x == 0 || intent->x == 1) {
            device_settings_t candidate = s_settings;
            candidate.theme = intent->x ? DEVICE_THEME_DARK : DEVICE_THEME_LIGHT;
            (void)update_settings(&candidate);
            board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
        }
        return;
    case APP_HOST_OS:
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_INPUT) return;
        if (intent->x == APP_HOST_CTRL || intent->x == APP_HOST_COMMAND) {
            cancel_input();
            device_settings_t candidate = s_settings;
            candidate.host_modifier = intent->x == APP_HOST_COMMAND ? DEVICE_HOST_COMMAND : DEVICE_HOST_CTRL;
            (void)update_settings(&candidate);
        }
        return;
    case APP_CALIBRATE:
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_INPUT) return;
        cancel_input();
        interaction_calibrate(&s_interaction);
        s_message = "Keep the device still while calibrating.";
        return;
    case APP_SETTINGS_PAGE:
        if (intent->x < APP_SETTINGS_HOME || intent->x > APP_SETTINGS_WIFI) return;
        s_settings_page = (app_settings_page_t)intent->x;
        open_overlay(APP_OVERLAY_SETTINGS);
        return;
    case APP_TRANSPORT_SELECT: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_CONNECTIVITY ||
            intent->x < APP_INPUT_BLUETOOTH || intent->x > APP_INPUT_USB) return;
        if ((app_input_transport_t)intent->x == s_input_transport) return;
        usb_hid_snapshot_t usb = usb_hid_snapshot();
        bool available = intent->x == APP_INPUT_USB ? usb.initialized && !usb.failed :
            link->initialized && !link->failed;
        if (!available) { s_message = "Requested connection is unavailable."; return; }
        cancel_input();
        s_transport_origin_generation = input_link_snapshot(link).generation;
        s_requested_transport = (app_input_transport_t)intent->x;
        s_transport_switch_pending = true;
        s_transport_switch_deadline_ms = s_now_ms > UINT64_MAX - 5000U ? UINT64_MAX : s_now_ms + 5000U;
        s_message = "Releasing old input before switching connection.";
        return;
    }
    case APP_BLUETOOTH_PAGE: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_BLUETOOTH || intent->x < 0) return;
        ble_hid_bond_list_t bonds = ble_hid_bond_list();
        unsigned pages = bonds.known && bonds.count ?
            ((unsigned)bonds.count + APP_UI_BONDS_PER_PAGE - 1U) / APP_UI_BONDS_PER_PAGE : 1U;
        s_bluetooth_page = (unsigned)intent->x < pages ? (unsigned)intent->x : pages - 1U;
        ++s_ui_revision;
        return;
    }
    case APP_BRIGHTNESS: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_DISPLAY ||
            !board_capabilities().brightness || intent->x < 10 || intent->x > 100 || intent->x % 10) return;
        device_settings_t candidate = s_settings;
        candidate.brightness_percent = (uint8_t)intent->x;
        (void)update_settings(&candidate);
        return;
    }
    case APP_IDLE_TIMEOUT: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_DISPLAY ||
            !board_capabilities().brightness || intent->x < 0 || intent->x % 60) return;
        device_settings_t candidate = s_settings;
        candidate.idle_timeout = (device_idle_timeout_t)(intent->x / 60);
        (void)update_settings(&candidate);
        return;
    }
    case APP_PAIRING_REQUEST:
        if (intent->x < APP_PAIR_OPEN || intent->x > APP_PAIR_DISCONNECT ||
            s_pair_operation != APP_CONFIRM_NONE) return;
        if (s_overlay != APP_OVERLAY_TARGET &&
            !(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_BLUETOOTH)) return;
        if (intent->x == APP_PAIR_DISCONNECT && !link->connected) return;
        if (intent->x == APP_PAIR_OPEN && link->bond_count_known &&
            link->bond_capacity && link->bond_count >= link->bond_capacity) {
            s_message = "Bluetooth pairings full; existing hosts are preserved.";
            return;
        }
        s_confirmation = intent->x == APP_PAIR_OPEN ? APP_CONFIRM_PAIR_OPEN :
            intent->x == APP_PAIR_CLOSE ? APP_CONFIRM_PAIR_CLOSE :
            intent->x == APP_PAIR_DISCONNECT ? APP_CONFIRM_DISCONNECT : APP_CONFIRM_PAIR_FORGET_ALL;
        s_confirmation_generation = link->generation;
        open_overlay(APP_OVERLAY_CONFIRM);
        return;
    case APP_WIFI_FORGET_REQUEST: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_WIFI) return;
        app_wifi_snapshot_t wifi;
        app_wifi_get_snapshot(&wifi);
        if (!wifi.initialized || wifi.operation_pending) return;
        (void)snprintf(s_confirmation_ssid, sizeof(s_confirmation_ssid), "%s", wifi.ssid);
        (void)snprintf(s_confirmation_stored_ssid, sizeof(s_confirmation_stored_ssid), "%s", wifi.stored_ssid);
        s_confirmation_storage_known = wifi.stored_data_known;
        s_confirmation_storage_present = wifi.stored_data_present;
        s_confirmation_storage_uncertain = wifi.storage_uncertain;
        s_confirmation = APP_CONFIRM_WIFI_FORGET;
        open_overlay(APP_OVERLAY_CONFIRM);
        return;
    }
    case APP_CONFIRM: {
        if (s_overlay != APP_OVERLAY_CONFIRM || (intent->x != 0 && intent->x != 1)) return;
        app_confirmation_t confirmation = s_confirmation;
        s_confirmation = APP_CONFIRM_NONE;
        s_settings_page = confirmation == APP_CONFIRM_WIFI_FORGET ? APP_SETTINGS_WIFI : APP_SETTINGS_BLUETOOTH;
        open_overlay(APP_OVERLAY_SETTINGS);
        if (!intent->x) return;
        if (confirmation == APP_CONFIRM_WIFI_FORGET) {
            app_wifi_snapshot_t wifi;
            app_wifi_get_snapshot(&wifi);
            if (!wifi.initialized || wifi.operation_pending ||
                strcmp(wifi.ssid, s_confirmation_ssid) != 0 ||
                strcmp(wifi.stored_ssid, s_confirmation_stored_ssid) != 0 ||
                wifi.stored_data_known != s_confirmation_storage_known ||
                wifi.stored_data_present != s_confirmation_storage_present ||
                wifi.storage_uncertain != s_confirmation_storage_uncertain) {
                s_message = "Network changed; review it before forgetting.";
                return;
            }
            s_message = app_wifi_forget_network() == ESP_OK ?
                "Forgetting Wi-Fi network." : "Wi-Fi request rejected; check status.";
        } else if (confirmation != APP_CONFIRM_NONE) {
            if (link->generation != s_confirmation_generation) {
                s_message = "Bluetooth changed; review it before confirming.";
                return;
            }
            if (confirmation == APP_CONFIRM_PAIR_CLOSE) {
                ble_hid_pairing_close();
                s_message = "Pairing window closed.";
                return;
            }
            ble_hid_pairing_close();
            s_pair_operation = confirmation;
            s_pair_origin_generation = link->generation;
            s_pair_disconnect_requested = false;
            s_pair_deadline_ms = s_now_ms > UINT64_MAX - 5000U ? UINT64_MAX : s_now_ms + 5000U;
            s_message = "Releasing input before changing Bluetooth pairing.";
        }
        return;
    }
    case APP_WIFI_SETUP: {
        if ((s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_WIFI) &&
            s_overlay != APP_OVERLAY_WIFI_SETUP) return;
        if (intent->x == 1) {
            app_wifi_snapshot_t wifi;
            app_wifi_get_snapshot(&wifi);
            esp_err_t setup_error = wifi.setup_active ? ESP_OK : app_wifi_start_setup();
            if (setup_error == ESP_OK) open_overlay(APP_OVERLAY_WIFI_SETUP);
            else s_message = "Wi-Fi setup unavailable; check status.";
        } else if (intent->x == 0) {
            if (s_overlay != APP_OVERLAY_WIFI_SETUP) (void)app_wifi_stop_setup();
            s_settings_page = APP_SETTINGS_WIFI;
            open_overlay(APP_OVERLAY_SETTINGS);
        }
        return;
    }
    case APP_MOTION:
        if (s_engine.state.mode == INPUT_MOUSE) toggle_motion();
        return;
    case APP_GAIN:
        if (s_engine.state.mode != INPUT_MOUSE) return;
        (void)interaction_set_touch_divisor(&s_interaction, (unsigned)intent->x);
        return;
    case APP_TOUCH_MOVE: {
        portENTER_CRITICAL(&s_control_lock);
        bool touch_down = s_touch_down;
        uint32_t live_gesture = s_live_touch_gesture;
        portEXIT_CRITICAL(&s_control_lock);
        if (!touch_down || intent->gesture != live_gesture || intent->gesture != s_touch_gesture ||
            intent->surface != APP_SURFACE_TOUCHPAD)
            return;
        input_delta_t delta = interaction_touch_move(&s_interaction,
            (float)intent->x, (float)intent->y);
        if (!delta.x && !delta.y) return;
        result = hid_engine_enqueue_mouse_move(&s_engine, intent->epoch, INPUT_LOCAL,
            delta.x, delta.y);
        break;
    }
    case APP_DRAG:
        if (s_drag_requested) {
            cancel_input();
            s_message = "Drag released.";
            return;
        }
        result = hid_engine_enqueue_mouse_buttons(&s_engine, intent->epoch, INPUT_LOCAL, 1);
        if (result == HID_ENGINE_ENQUEUE_OK) s_drag_requested = true;
        break;
    case APP_KEYBOARD_SUBMODE:
        if (s_engine.state.mode != INPUT_KEYBOARD || intent->x < APP_KEYBOARD_MOTION ||
            intent->x > APP_KEYBOARD_SHORTCUTS) return;
        cancel_input();
        s_keyboard_submode = (app_keyboard_submode_t)intent->x;
        interaction_back(&s_interaction);
        interaction_set_keyboard_motion(&s_interaction,
            s_keyboard_submode == APP_KEYBOARD_MOTION);
        if (s_overlay == APP_OVERLAY_KEYBOARD) {
            s_overlay = APP_OVERLAY_NONE;
            sync_interaction();
        }
        return;
    case APP_GRID:
        if (s_engine.state.mode == INPUT_KEYBOARD &&
            (s_keyboard_submode == APP_KEYBOARD_MOTION || s_keyboard_submode == APP_KEYBOARD_TOUCH))
            edited = interaction_select_cell(&s_interaction, (unsigned)intent->x);
        break;
    case APP_BACK:
        interaction_back(&s_interaction);
        ++s_ui_revision;
        return;
    case APP_CASE:
        edited = interaction_set_lowercase(&s_interaction, !s_interaction.view.lowercase);
        break;
    case APP_SYMBOLS:
        edited = interaction_set_page(&s_interaction,
            s_interaction.view.page == INTERACTION_SYMBOLS ? INTERACTION_LETTERS : INTERACTION_SYMBOLS);
        break;
    case APP_DELETE: edited = interaction_delete(&s_interaction); break;
    case APP_SPACE: edited = interaction_append(&s_interaction, ' '); break;
    case APP_CLEAR: edited = interaction_clear(&s_interaction); break;
    case APP_SEND:
        if (s_engine.state.mode != INPUT_KEYBOARD || s_send_token) return;
        s_send_token = interaction_send_begin(&s_interaction);
        if (!s_send_token) {
            s_message = "Choose a ready host and add text to preview.";
            return;
        }
        s_send_epoch = s_engine.state.epoch;
        s_send_base = 0;
        if (!enqueue_draft_chunk()) update_send(true);
        s_message = s_send_token ? "Sending preview without Enter." : "Send blocked; preview retained.";
        return;
    case APP_STOP:
        cancel_input();
        s_message = "Stopped; unsent characters remain in preview.";
        return;
    case APP_SHORTCUT: {
        if (s_engine.state.mode != INPUT_KEYBOARD || s_keyboard_submode != APP_KEYBOARD_SHORTCUTS)
            return;
        static const unsigned usages[] = {0x06, 0x19, 0x1d, 0x2b, 0x29, 0x28};
        if (intent->x < APP_SHORTCUT_COPY || intent->x > APP_SHORTCUT_ENTER) return;
        unsigned modifier = intent->x <= APP_SHORTCUT_UNDO ?
            (s_host_os == APP_HOST_COMMAND ? 0x08U : 0x01U) : 0;
        result = hid_engine_enqueue_key(&s_engine, intent->epoch, INPUT_LOCAL,
            modifier, usages[intent->x]);
        break;
    }
    case APP_AUDIO_KIND:
        if (s_engine.state.mode == INPUT_AUDIO &&
            (intent->x == APP_AUDIO_VOICE || intent->x == APP_AUDIO_SPEAKER)) {
            bool changed = s_audio_kind != (app_audio_kind_t)intent->x;
            s_audio_kind = (app_audio_kind_t)intent->x;
            if (s_overlay == APP_OVERLAY_AUDIO) {
                s_overlay = APP_OVERLAY_NONE;
                changed = true;
            }
            if (changed) {
                ++s_ui_revision;
                sync_interaction();
            }
        }
        return;
    case APP_VOICE_START: case APP_VOICE_CONFIRM:
        s_message = "Spoken letter recognition is not available yet.";
        return;
    case APP_AUDIO_POWER: case APP_AUDIO_MUTE: case APP_AUDIO_VOLUME:
        s_message = "Bluetooth audio is not available yet.";
        return;
    case APP_MOVE:
        result = hid_engine_enqueue_mouse_move(&s_engine, intent->epoch, INPUT_LOCAL,
            intent->x, intent->y);
        break;
    case APP_CLICK:
        result = hid_engine_enqueue_mouse_click(&s_engine, intent->epoch, INPUT_LOCAL,
            (unsigned)intent->x);
        break;
    case APP_SCROLL:
        result = hid_engine_enqueue_mouse_scroll(&s_engine, intent->epoch, INPUT_LOCAL, intent->x);
        break;
    case APP_TEXT:
        result = hid_engine_enqueue_text(&s_engine, intent->epoch, INPUT_LOCAL,
            intent->text, strnlen(intent->text, sizeof(intent->text)));
        break;
    case APP_KEY:
        result = hid_engine_enqueue_key(&s_engine, intent->epoch, INPUT_LOCAL,
            (unsigned)intent->y, (unsigned)intent->x);
        break;
    default: return;
    }
    if (edited) {
        ++s_ui_revision;
        s_message = "Preview only; press Send when ready.";
        board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
    } else if (result == HID_ENGINE_ENQUEUE_OK) {
        if (intent->action != APP_MOVE && intent->action != APP_TOUCH_MOVE && intent->action != APP_SCROLL)
            board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
    } else if (intent->action != APP_MOVE && intent->action != APP_TOUCH_MOVE) {
        s_message = result == HID_ENGINE_ENQUEUE_FULL ? "Input queue full; request rejected." :
            "Check mode, lock and host readiness.";
    }
}

static void discard_intents(void)
{
    xQueueReset(s_intents);
    portENTER_CRITICAL(&s_control_lock);
    s_touch_emergency_pending[0] = false;
    s_touch_emergency_pending[1] = false;
    portEXIT_CRITICAL(&s_control_lock);
    /* Queue cancellation may remove the matching END. Consume the local
     * policy gesture too; raw touch state still suppresses motion until UP. */
    if (s_idle.down[DEVICE_INPUT_TOUCH])
        (void)device_idle_input(&s_idle, DEVICE_INPUT_TOUCH, DEVICE_INPUT_CANCEL,
            s_touch_token, s_now_ms < s_idle.now_ms ? s_idle.now_ms : s_now_ms);
}

static bool process_priority(void)
{
    app_intent_t emergency[2];
    bool pending[2];
    portENTER_CRITICAL(&s_control_lock);
    unsigned requests = s_priority_requests;
    s_priority_requests = 0;
    for (unsigned i = 0; i < 2; ++i) {
        emergency[i] = s_touch_emergency[i];
        pending[i] = s_touch_emergency_pending[i];
        s_touch_emergency_pending[i] = false;
    }
    uint32_t live_gesture = s_live_touch_gesture;
    portEXIT_CRITICAL(&s_control_lock);
    for (unsigned i = 0; i < 2; ++i) {
        if (!pending[i] || emergency[i].epoch != s_engine.state.epoch ||
            emergency[i].ui_revision != s_ui_revision ||
            emergency[i].gesture == s_consumed_gesture) continue;
        if (emergency[i].gesture != s_idle_touch_gesture &&
            emergency[i].gesture == live_gesture && !requests) {
            /* Its BEGIN may still be in the normal queue. Let that boundary
             * run first; a queue discard clears this bounded deferred slot. */
            portENTER_CRITICAL(&s_control_lock);
            if (!s_touch_emergency_pending[i]) {
                s_touch_emergency[i] = emergency[i];
                s_touch_emergency_pending[i] = true;
            }
            portEXIT_CRITICAL(&s_control_lock);
        } else if (route_touch(&emergency[i])) {
            requests |= i == 0 ? LOCK_REQUEST : RELEASE_REQUEST;
        }
    }
    if (!requests) {
        return false;
    }
    discard_intents();
    s_transport_switch_pending = false;
    s_requested_transport = s_input_transport;
    update_send(true);
    s_drag_requested = false;
    bool setup_was_visible = s_overlay == APP_OVERLAY_WIFI_SETUP;
    if (setup_was_visible) (void)app_wifi_stop_setup();
    s_overlay = APP_OVERLAY_NONE;
    ++s_ui_revision;
    if (requests & (LOCK_REQUEST | PHYSICAL_LONG_PRESS)) {
        bool locked = (requests & LOCK_REQUEST) || !s_engine.state.locked;
        hid_engine_set_locked(&s_engine, locked);
        s_message = locked ? "Locked. Hold AI to unlock; old input is cancelled." :
                    "Unlocked. Motion remains off; old input will not resume.";
        board_haptic_feedback(BOARD_FEEDBACK_LOCK);
        if (locked) {
            ble_hid_pairing_close();
            if (!setup_was_visible) (void)app_wifi_stop_setup();
            s_pair_operation = APP_CONFIRM_NONE;
            s_confirmation = APP_CONFIRM_NONE;
        }
    } else {
        hid_engine_cancel(&s_engine);
        s_message = "Pending input cancelled; releasing keys and buttons.";
    }
    sync_interaction();
    return true;
}

static void handle_button_events(unsigned events)
{
    if (events & (INTERACTION_EVENT_SINGLE | INTERACTION_EVENT_DOUBLE | INTERACTION_EVENT_HOLD)) {
        uint64_t now = s_now_ms < s_idle.now_ms ? s_idle.now_ms : s_now_ms;
        if (device_idle_input(&s_idle, DEVICE_INPUT_AI, DEVICE_INPUT_ACTION, s_ai_token, now)
                != DEVICE_INPUT_PASS) {
            interaction_consume_press(&s_interaction);
            return;
        }
    }
    bool same_context = s_button_context_revision == s_ui_revision &&
        s_button_context_epoch == s_engine.state.epoch;
    if (events & INTERACTION_EVENT_DOWN) {
        s_button_context_revision = s_ui_revision;
        s_button_context_epoch = s_engine.state.epoch;
        if (s_engine.state.owner == INPUT_REMOTE) {
            update_send(true);
            (void)hid_engine_set_owner(&s_engine, INPUT_LOCAL);
            s_drag_requested = false;
            ++s_ui_revision;
            interaction_consume_press(&s_interaction);
        }
    }
    if (events & INTERACTION_EVENT_HOLD) {
        physical_lock();
        return;
    }
    if (s_engine.state.locked) return;
    if (events & INTERACTION_EVENT_DOUBLE) {
        /* A physical shortcut navigates ordinary panels, never confirms or
         * dismisses a pending privileged operation on the user's behalf. */
        if (s_overlay == APP_OVERLAY_CONFIRM) return;
        open_overlay(s_overlay == APP_OVERLAY_MODES ? APP_OVERLAY_NONE : APP_OVERLAY_MODES);
        return;
    }
    if (!(events & INTERACTION_EVENT_SINGLE) || !same_context ||
        s_overlay != APP_OVERLAY_NONE) return;
    if (s_engine.state.mode == INPUT_MOUSE) {
        toggle_motion();
    } else if (s_engine.state.mode == INPUT_KEYBOARD &&
        (s_keyboard_submode == APP_KEYBOARD_MOTION || s_keyboard_submode == APP_KEYBOARD_TOUCH)) {
        if (interaction_confirm(&s_interaction)) {
            ++s_ui_revision;
            board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
            s_message = "Preview only; press Send when ready.";
        }
    }
}

static void process_buttons(void)
{
    uint64_t poll_ms = s_now_ms;
    portENTER_CRITICAL(&s_control_lock);
    bool lost_edges = s_button_overflow;
    bool physical_down = s_physical_down;
    s_button_overflow = false;
    if (lost_edges || s_button_resync) {
        s_button_head = 0;
        s_button_count = 0;
    }
    portEXIT_CRITICAL(&s_control_lock);
    if (lost_edges || s_button_resync) {
        /* Do not classify a hold from a partial edge sequence. Remain locked
         * until UP is observed, then require a new complete physical hold. */
        interaction_consume_press(&s_interaction);
        s_interaction.view.button_down = false;
        (void)device_idle_input(&s_idle, DEVICE_INPUT_AI, DEVICE_INPUT_CANCEL,
            s_ai_token, poll_ms < s_idle.now_ms ? s_idle.now_ms : poll_ms);
        s_button_resync = physical_down;
        return;
    }
    /* Consume physical timestamps before advancing the model clock to now. */
    for (unsigned i = 0; i < 8; ++i) {
        button_edge_t edge;
        portENTER_CRITICAL(&s_control_lock);
        bool available = s_button_count != 0;
        if (available) {
            edge = s_button_edges[s_button_head];
            s_button_head = (s_button_head + 1) % 8;
            --s_button_count;
        }
        portEXIT_CRITICAL(&s_control_lock);
        if (!available) break;
        /* Handlers may synchronize/open a modal. Keep those operations on
         * this edge's time until the complete ordered batch is consumed. */
        s_now_ms = edge.time_ms;
        if (s_now_ms > poll_ms) poll_ms = s_now_ms;
        /* Classify any older pending single before assigning a new press token. */
        handle_button_events(interaction_tick(&s_interaction, edge.time_ms));
        if (edge.down && s_ai_token < UINT64_MAX) ++s_ai_token;
        bool was_asleep = s_idle.asleep;
        device_input_route_t route = device_idle_input(&s_idle, DEVICE_INPUT_AI,
            edge.down ? DEVICE_INPUT_BEGIN : DEVICE_INPUT_END, s_ai_token,
            poll_ms < s_idle.now_ms ? s_idle.now_ms : poll_ms);
        if (was_asleep && !s_idle.asleep) ++s_ui_revision;
        board_brightness_status_t brightness = board_brightness_status();
        if (route != DEVICE_INPUT_PASS || (edge.down && board_capabilities().brightness &&
            (!brightness.applied_known || !brightness.applied_percent || brightness.error != ESP_OK))) {
            interaction_consume_press(&s_interaction);
            s_interaction.view.button_down = false;
            if (route == DEVICE_INPUT_PASS)
                (void)device_idle_input(&s_idle, DEVICE_INPUT_AI, DEVICE_INPUT_CANCEL,
                    s_ai_token, s_idle.now_ms);
            if (edge.down && board_capabilities().brightness &&
                (!brightness.applied_known || brightness.error != ESP_OK))
                (void)board_brightness_set((int)s_settings.brightness_percent);
            continue;
        }
        handle_button_events(interaction_button_update(&s_interaction, edge.down, edge.time_ms));
    }
    s_now_ms = poll_ms;
    handle_button_events(interaction_tick(&s_interaction, s_now_ms));
}

static void process_motion(void)
{
    board_imu_sample_t sample;
    if (board_imu_read(&sample) != ESP_OK) {
        if (!board_capabilities().imu) {
            (void)hid_engine_set_gyro(&s_engine, false);
            (void)interaction_set_motion(&s_interaction, false);
        }
        return;
    }
    if (sample.timestamp_us == s_last_imu_us) return;
    s_last_imu_us = sample.timestamp_us;
    /* Initial screen/grip mapping; physical axis direction is a device
     * acceptance check. All calibration and integration remain portable. */
    interaction_imu_sample_t mapped = {
        .timestamp_us = sample.timestamp_us,
        .gx_dps = -sample.z_dps, .gy_dps = sample.x_dps, .gz_dps = sample.y_dps,
        .ax_g = -sample.z_g, .ay_g = sample.x_g, .az_g = sample.y_g
    };
    bool was_calibrated = s_interaction.view.calibrated;
    input_delta_t delta = interaction_motion_sample(&s_interaction, &mapped);
    if (!was_calibrated && s_interaction.view.calibrated) {
        s_message = "Motion calibrated. Click AI to toggle motion.";
    }
    if (delta.x || delta.y) {
        (void)hid_engine_enqueue_mouse_move(&s_engine, s_engine.state.epoch,
            INPUT_LOCAL, delta.x, delta.y);
    }
}

static void render_view(const ble_hid_snapshot_t *link, esp_err_t error)
{
    input_link_t active = input_link_snapshot(link);
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    board_capabilities_t capabilities = board_capabilities();
    board_power_status_t power;
    (void)board_power_read(&power);
    app_wifi_snapshot_t wifi;
    app_wifi_get_snapshot(&wifi);
    settings_store_status_t storage = settings_store_status();
    interaction_view_t interaction;
    interaction_get_view(&s_interaction, &interaction);
    char bluetooth_target[40] = "No Bluetooth host";
    if (link->connected && link->peer_known) {
        (void)snprintf(bluetooth_target, sizeof(bluetooth_target), "Host %02X:%02X:%02X:%02X:%02X:%02X",
            link->peer_address[0], link->peer_address[1], link->peer_address[2],
            link->peer_address[3], link->peer_address[4], link->peer_address[5]);
    } else if (link->connected) {
        (void)snprintf(bluetooth_target, sizeof(bluetooth_target), "Bluetooth host (identity unknown)");
    }
    const char *target = s_input_transport == APP_INPUT_USB ?
        (usb.mounted ? "USB host" : "USB: no host connected") : bluetooth_target;
    app_view_t view = {
        .epoch = s_engine.state.epoch, .ui_revision = s_ui_revision,
        .mode = s_engine.state.mode, .connected = active.connected, .ready = s_engine.ready,
        .locked = s_engine.state.locked, .release_pending = s_engine.state.release_pending,
        .congested = active.congested, .transport_error = active.failed ||
            (s_input_transport == APP_INPUT_BLUETOOTH && error != ESP_OK),
        .haptics_available = capabilities.haptics, .haptics_enabled = board_haptics_enabled(),
        .queued = s_engine.count, .keyboard_leds = active.keyboard_leds,
        .message = s_message, .target_name = target,
        .remote_owned = s_engine.state.owner == INPUT_REMOTE,
        .motion_available = capabilities.imu && interaction.calibrated,
        .calibration_available = capabilities.imu,
        .dark_theme = s_dark_theme,
        .motion_enabled = s_engine.state.gyro_enabled,
        .calibrating = capabilities.imu && interaction.calibrating,
        .gain_divisor = interaction.touch_divisor, .dragging = s_drag_requested,
        .overlay = s_overlay, .keyboard_submode = s_keyboard_submode,
        .keyboard_group = interaction.group, .keyboard_focus = interaction.focus,
        .lowercase = interaction.lowercase, .symbols = interaction.page == INTERACTION_SYMBOLS,
        .sending = interaction.sending, .accepted_prefix = interaction.send_accepted,
        .send_total = interaction.send_total, .host_os = s_host_os,
        .audio_kind = s_audio_kind, .microphone_muted = true, .volume = 50
    };
    view.settings_page = s_settings_page;
    view.input_transport = s_input_transport;
    view.requested_transport = s_requested_transport;
    view.transport_switch_pending = s_transport_switch_pending;
    view.bluetooth_available = link->initialized && !link->failed;
    view.bluetooth_connected = link->connected;
    view.bluetooth_ready = link->ready;
    (void)snprintf(view.bluetooth_target_name, sizeof(view.bluetooth_target_name), "%s", bluetooth_target);
    view.usb_available = usb.initialized && !usb.failed;
    view.usb_connected = usb.mounted;
    view.usb_ready = usb.ready;
    (void)snprintf(view.transport_message, sizeof(view.transport_message), "%s",
        s_transport_switch_pending ? "Waiting for old-host releases to complete." :
        s_engine.state.mode == INPUT_AUDIO ? "Audio is not implemented for either connection yet." :
        "Input goes only to the selected connection.");
    view.confirmation = s_confirmation;
    view.battery_supported = capabilities.battery;
    view.battery_percent_valid = power.percent_valid && power.battery_present == BOARD_TRUTH_YES;
    view.battery_percent = view.battery_percent_valid ? power.percent : 0;
    view.battery_state = power.full == BOARD_TRUTH_YES ? APP_BATTERY_FULL :
        power.charging == BOARD_TRUTH_YES ? APP_BATTERY_CHARGING :
        (power.current_valid && power.current_ma < 0 ? APP_BATTERY_DISCHARGING : APP_BATTERY_UNKNOWN);
    view.pairing_state = !link->initialized ? APP_PAIRING_UNAVAILABLE :
        (link->failed || link->bond_forget_state == BLE_HID_FORGET_FAILED) ? APP_PAIRING_ERROR :
        (s_pair_operation != APP_CONFIRM_NONE || link->bond_forget_state == BLE_HID_FORGET_RUNNING) ?
            APP_PAIRING_CLEARING : link->pairing_open ? APP_PAIRING_OPEN : APP_PAIRING_CLOSED;
    view.paired_host_count = link->bond_count_known ? (int)link->bond_count : -1;
    view.paired_host_capacity = link->bond_capacity;
    view.pairing_seconds_left = (link->pairing_remaining_ms + 999U) / 1000U;
    ble_hid_bond_list_t bonds = ble_hid_bond_list();
    view.bluetooth_hosts_available = bonds.known;
    view.bluetooth_host_count = bonds.known && bonds.count <= APP_UI_BOND_CAPACITY ? bonds.count : 0;
    unsigned pages = view.bluetooth_host_count ?
        ((unsigned)view.bluetooth_host_count + APP_UI_BONDS_PER_PAGE - 1U) / APP_UI_BONDS_PER_PAGE : 1U;
    if (s_bluetooth_page >= pages) s_bluetooth_page = pages - 1U;
    view.bluetooth_page = s_bluetooth_page;
    for (size_t i = 0; i < view.bluetooth_host_count; ++i) {
        const ble_hid_bond_t *peer = &bonds.bonds[i];
        (void)snprintf(view.bluetooth_hosts[i].identity, sizeof(view.bluetooth_hosts[i].identity),
            "%02X:%02X:%02X:%02X:%02X:%02X/%s%u",
            peer->address[0], peer->address[1], peer->address[2],
            peer->address[3], peer->address[4], peer->address[5],
            peer->address_type_known ? "" : "?", (unsigned)peer->address_type);
        view.bluetooth_hosts[i].connected = link->connected && bonds.current_match_known && peer->is_current;
    }
    switch (wifi.state) {
    case APP_WIFI_IDLE: view.wifi_state = APP_UI_WIFI_DISCONNECTED; break;
    case APP_WIFI_CONNECTING: view.wifi_state = APP_UI_WIFI_CONNECTING; break;
    case APP_WIFI_CONNECTED: view.wifi_state = APP_UI_WIFI_CONNECTED; break;
    case APP_WIFI_BACKOFF: view.wifi_state = APP_UI_WIFI_RETRYING; break;
    case APP_WIFI_ERROR: view.wifi_state = APP_UI_WIFI_ERROR; break;
    default: view.wifi_state = APP_UI_WIFI_UNAVAILABLE; break;
    }
    view.wifi_rssi_valid = wifi.state == APP_WIFI_CONNECTED && wifi.rssi_valid;
    view.wifi_rssi_dbm = view.wifi_rssi_valid ? wifi.rssi_dbm : 0;
    view.wifi_saved_network = wifi.configured || wifi.stored_data_present ||
        wifi.storage_uncertain || (wifi.initialized && !wifi.stored_data_known);
    view.wifi_operation_pending = wifi.operation_pending || wifi.cleanup_pending;
    (void)snprintf(view.wifi_ssid, sizeof(view.wifi_ssid), "%s", wifi.ssid);
    (void)snprintf(view.wifi_ipv4, sizeof(view.wifi_ipv4), "%s", wifi.ip);
    if (wifi.cleanup_pending)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Setup closing; retrying shutdown.");
    else if (wifi.storage_uncertain)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Saved network uncertain; set up again or forget.");
    else if (wifi.storage_error != ESP_OK)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Saved network error; forget to clear.");
    else if (wifi.connection_error != ESP_OK)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Connection: %s", esp_err_to_name(wifi.connection_error));
    else if (wifi.last_error != ESP_OK)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Wi-Fi: %s", esp_err_to_name(wifi.last_error));
    else if (wifi.retry_in_ms)
        (void)snprintf(view.wifi_message, sizeof(view.wifi_message), "Retry in %u seconds", (unsigned)((wifi.retry_in_ms + 999U) / 1000U));
    view.wifi_setup_active = wifi.setup_active;
    view.wifi_setup_seconds_left = (wifi.setup_remaining_ms + 999U) / 1000U;
    if (s_overlay == APP_OVERLAY_WIFI_SETUP && !s_engine.state.locked) {
        app_wifi_setup_info_t setup;
        if (app_wifi_get_setup_info(&setup) == ESP_OK) {
            (void)snprintf(view.wifi_setup_ssid, sizeof(view.wifi_setup_ssid), "%s", setup.ssid);
            (void)snprintf(view.wifi_setup_passcode, sizeof(view.wifi_setup_passcode), "%s", setup.password);
            (void)snprintf(view.wifi_setup_url, sizeof(view.wifi_setup_url), "http://%s", setup.ip);
        }
        /* The local view is the only recipient of setup credentials. */
        volatile unsigned char *bytes = (volatile unsigned char *)&setup;
        for (size_t i = 0; i < sizeof(setup); ++i) bytes[i] = 0;
    }
    view.brightness_available = capabilities.brightness;
    view.brightness_percent = s_settings.brightness_percent;
    view.screen_off_available = capabilities.brightness;
    view.idle_timeout_seconds = (unsigned)s_settings.idle_timeout * 60U;
    view.screen_off = s_idle.asleep;
    view.settings_save_state = !storage.available ? APP_SAVE_UNAVAILABLE :
        storage.pending ? APP_SAVE_PENDING : storage.error != ESP_OK ? APP_SAVE_ERROR :
        (storage.loaded || storage.saved_revision != 0) ? APP_SAVE_SAVED : APP_SAVE_DEFAULTS;
    (void)snprintf(view.firmware_version, sizeof(view.firmware_version), "%s", s_firmware_version);
    (void)snprintf(view.build_id, sizeof(view.build_id), "%s", s_build_id);
    (void)snprintf(view.board_revision, sizeof(view.board_revision), "%s", capabilities.name);
    (void)snprintf(view.device_id, sizeof(view.device_id), "%s", s_device_id);
    view.uptime_seconds = s_now_ms / 1000U;
    memcpy(view.preview, interaction.preview, sizeof(view.preview));
    for (size_t i = 0; i < INTERACTION_GRID_CELLS; ++i) {
        (void)snprintf(view.keyboard_cells[i], sizeof(view.keyboard_cells[i]), "%s", interaction.cells[i]);
        if (interaction.cells[i][0]) ++view.keyboard_cell_count;
    }
    if (board_display_lock(0)) {
        app_ui_render(&view);
        board_display_unlock();
    }
    volatile char *secret = view.wifi_setup_passcode;
    for (size_t i = 0; i < sizeof(view.wifi_setup_passcode); ++i) secret[i] = 0;
}

static void observe_input_link(const input_link_t *link, input_link_t *previous)
{
    bool replaced = link->kind != previous->kind || link->generation != previous->generation;
    if (replaced) {
        cancel_input();
        discard_intents();
    }
    if (replaced || link->connected != previous->connected || link->ready != previous->ready) {
        update_send(true);
        hid_engine_set_link(&s_engine, link->connected, link->ready);
        s_drag_requested = false;
        ESP_LOGI(TAG, "Input connection=%d connected=%d ready=%d generation=%llu",
                 (int)link->kind, link->connected, link->ready, (unsigned long long)link->generation);
    }
    *previous = *link;
    if (hid_engine_observe_transport(&s_engine, s_now_ms, link->congested)) {
        update_send(true);
        s_drag_requested = false;
        discard_intents();
        s_message = "Host stalled; pending input expired. Releases have priority.";
    }
}

void app_main(void)
{
    esp_err_t diagnostic_error = heap_caps_register_failed_alloc_callback(record_allocation_failure);
    if (diagnostic_error != ESP_OK)
        ESP_LOGW(TAG, "Allocation diagnostics unavailable: %s", esp_err_to_name(diagnostic_error));
    record_boot_memory("entry");
    esp_err_t usb_error = usb_console_start();
    if (usb_error != ESP_OK) {
        ESP_LOGE(TAG, "USB unavailable: %s", esp_err_to_name(usb_error));
        s_message = "USB unavailable; Bluetooth remains selectable.";
    }
    record_boot_memory("usb");
    s_intents = xQueueCreateStatic(INTENT_CAPACITY, sizeof(app_intent_t),
                                    s_intent_bytes, &s_intent_storage);
    configASSERT(s_intents);
    hid_engine_init(&s_engine);
    interaction_init(&s_interaction);
    s_now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    device_settings_defaults(&s_settings);
    device_idle_init(&s_idle, s_now_ms);
    interaction_calibrate(&s_interaction);
    esp_err_t button_error = board_button_events_register(physical_button);
    if (button_error != ESP_OK) {
        report_boot_fault("physical button events", button_error);
        return;
    }
    esp_err_t error = board_init(physical_lock);
    if (error != ESP_OK) {
        report_boot_fault("board initialization", error);
        return;
    }
    record_boot_memory("board");
    esp_err_t storage_error = nvs_flash_init();
    if (storage_error == ESP_OK) storage_error = settings_store_init(&s_settings);
    if (storage_error != ESP_OK) {
        ESP_LOGW(TAG, "Settings storage unavailable: %s; existing data preserved", esp_err_to_name(storage_error));
        s_message = "Using device settings with a storage error; data preserved.";
    }
    s_dark_theme = s_settings.theme == DEVICE_THEME_DARK;
    s_host_os = s_settings.host_modifier == DEVICE_HOST_COMMAND ? APP_HOST_COMMAND : APP_HOST_CTRL;
    s_input_transport = s_settings.preferred_transport == DEVICE_TRANSPORT_USB ? APP_INPUT_USB : APP_INPUT_BLUETOOTH;
    s_requested_transport = s_input_transport;
    usb_hid_set_enabled(s_input_transport == APP_INPUT_USB);
    board_haptics_enable(s_settings.haptics_enabled);
    (void)device_idle_configure(&s_idle, s_settings.idle_timeout, s_now_ms);
    const esp_app_desc_t *description = esp_app_get_description();
    (void)snprintf(s_firmware_version, sizeof(s_firmware_version), "%s", description->version);
    for (size_t i = 0; i < 6; ++i)
        (void)snprintf(s_build_id + i * 2, sizeof(s_build_id) - i * 2, "%02x", description->app_elf_sha256[i]);
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        (void)snprintf(s_device_id, sizeof(s_device_id), "INPUT-%02X%02X%02X", mac[3], mac[4], mac[5]);
    else (void)snprintf(s_device_id, sizeof(s_device_id), "Unavailable");
    service_display();
    if (!board_display_lock(1000)) {
        report_boot_fault("display lock", ESP_ERR_TIMEOUT);
        return;
    }
    app_ui_create(board_display_get(), submit_intent);
    board_display_unlock();
    record_boot_memory("ui");
    error = ble_hid_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(error));
        s_message = "Bluetooth initialization failed. See device diagnostics.";
    }
    record_boot_memory("bluetooth");
    esp_err_t wifi_error = app_wifi_init();
    if (wifi_error != ESP_OK)
        ESP_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_error));
    record_boot_memory("wifi");
    input_link_t previous = {0};
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    int64_t last_health_us = esp_timer_get_time();
    hid_engine_step_result_t previous_step = HID_ENGINE_STEP_IDLE;
    unsigned ui_tick = 0;
    for (;;) {
        s_now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        process_buttons();
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
        service_usb_selection();
        input_link_t link = input_link_snapshot(&bluetooth);
        observe_input_link(&link, &previous);
        sync_interaction();
        (void)process_priority();
        app_intent_t intent;
        for (unsigned i = 0; i < 4 && xQueueReceive(s_intents, &intent, 0) == pdTRUE; ++i) {
            handle_intent(&intent, &bluetooth);
            if (process_priority()) {
                break;
            }
        }
        /* Congestion may begin after the snapshot; send rechecks its gate.
         * Failure cancels queued actions and starts bounded release recovery. */
        (void)process_priority();
        sync_interaction();
        process_motion();
        if (!link.congested) {
            hid_engine_step_result_t step = hid_engine_step(&s_engine, send_report, &link.generation);
            if (step != previous_step &&
                (step == HID_ENGINE_STEP_SEND_FAILED || step == HID_ENGINE_STEP_RETRY_EXHAUSTED)) {
                s_message = "Send failed; input cancelled. Release recovery has priority.";
                ESP_LOGW(TAG, "HID send failed; release pending=%d", s_engine.state.release_pending);
            }
            if (step == HID_ENGINE_STEP_RETRY_EXHAUSTED && step != previous_step) {
                if (s_input_transport == APP_INPUT_USB)
                    (void)usb_hid_recover_generation(link.generation);
                else (void)ble_hid_disconnect_generation(link.generation);
            }
            previous_step = step;
        }
        update_send(false);
        service_pair_operation(&bluetooth);
        service_transport_switch();
        service_display();
        if (s_engine.state.release_pending) s_drag_requested = false;
        if (++ui_tick >= 10) {
            ui_tick = 0;
            render_view(&bluetooth, error);
        }
        int64_t now = esp_timer_get_time();
        if (now - last_health_us >= 10000000) {
            last_health_us = now;
            report_memory_diagnostics();
            ESP_LOGI(TAG, "Health uptime_s=%llu heap=%u min_heap=%u queue=%u locked=%d "
                     "free_internal=%u largest_internal=%u stack_bytes=%u "
                     "free_dma=%u largest_dma=%u "
                     "ble_initialized=%d ble_connected=%d ble_ready=%d ble_failed=%d",
                     (unsigned long long)(now / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)s_engine.count, s_engine.state.locked,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     bluetooth.initialized,
                     bluetooth.connected, bluetooth.ready, bluetooth.failed);
            if (error != ESP_OK || bluetooth.failed) {
                /* Initialization logs can precede USB attachment. Preserve a
                 * bounded periodic failure signal after enumeration. */
                ESP_LOGE(TAG, "Bluetooth unavailable: init_error=%s transport_failed=%d",
                         esp_err_to_name(error), bluetooth.failed);
                ble_hid_failure_t failure = ble_hid_failure();
                if (failure.recorded) {
                    ESP_LOGE(TAG, "First Bluetooth failure stage=%s error=%s sdk_status_known=%d "
                             "sdk_status=%u uptime_ms=%llu", failure.stage,
                             esp_err_to_name(failure.error), failure.sdk_status_known,
                             (unsigned)failure.sdk_status, (unsigned long long)failure.uptime_ms);
                }
            }
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        /* Never catch up delayed ticks by emitting a burst of stale reports. */
        vTaskDelay(pdMS_TO_TICKS(COORDINATOR_PERIOD_MS));
    }
}
