// SPDX-License-Identifier: Apache-2.0
#include "app_ui.h"
#include "app_presenter.h"
#include "app_boot_ui.h"
#include "app_media_runtime.h"
#include "app_media_controls.h"
#include "app_connectivity_controls.h"
#include "app_recording_store.h"
#include "app_control_runtime.h"
#include "app_remote_input.h"
#include "app_command_bridge.h"
#include "esp_random.h"
#include "ble_hid.h"
#include "board_port.h"
#include "hid_engine.h"
#include "interaction.h"
#include "interaction_multitouch.h"
#include "board_touch_port.h"
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

static void present_boot(app_boot_ui_state_t state, const char *stage, const char *detail)
{
    /* The board may return an error after successfully creating its display
     * (for example, a physical-button failure). Use the actual retained
     * handle, not the overall board-init result, to keep that fault visible. */
    lv_display_t *display = board_display_get();
    if (!display || !board_display_lock(100)) return;
    if (app_boot_ui_active() || app_boot_ui_create(display)) {
        app_boot_ui_view_t boot = {.state = state};
        (void)snprintf(boot.stage, sizeof(boot.stage), "%s", stage);
        (void)snprintf(boot.detail, sizeof(boot.detail), "%s", detail ? detail : "");
        (void)app_boot_ui_update(&boot);
    }
    board_display_unlock();
}

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

typedef struct {
    bool contacts;
    union {
        app_intent_t intent;
        struct { interaction_contact_snapshot_t sample; bool eligible; } touch;
    } value;
} coordinator_event_t;
static QueueHandle_t s_intents;
static StaticQueue_t s_intent_storage;
static uint8_t s_intent_bytes[INTENT_CAPACITY * sizeof(coordinator_event_t)];
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_priority_requests;
static app_intent_t s_touch_emergency[2];
static bool s_touch_emergency_pending[2];
static hid_engine_t s_engine;
static interaction_t s_interaction;
static interaction_multitouch_t s_multitouch;
static bool s_multitouch_available, s_multitouch_consuming, s_multitouch_eligible;
static const char *s_message = "Keep still to calibrate. Select an input connection.";
static app_overlay_t s_overlay;
static app_keyboard_submode_t s_keyboard_submode;
static app_host_os_t s_host_os;
static app_audio_kind_t s_audio_kind;
static bool s_dark_theme = true;
static device_settings_t s_settings;
/* Last admitted durable intent, separate from temporary effective overrides.
 * The settings store still owns commit/readback evidence and error reporting. */
static device_settings_t s_durable_settings;
static uint64_t s_settings_revision = 1;
static app_input_transport_t s_input_transport, s_requested_transport;
static bool s_transport_switch_pending;
static bool s_transport_switch_persist = true;
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
static uint64_t s_management_generation = 1, s_state_sequence = 1;
/* Remote input is an explicit local, current-boot allowance. Pairing, a LAN
 * connection or a surviving authenticated session cannot restore it. */
static uint64_t s_remote_allowance_generation = 1;
static bool s_remote_allowed;
static app_remote_input_t *s_remote_input;
static app_command_bridge_t *s_command_bridge;
static bool s_applying_remote_safety;
static void remote_local_override(bool locked);
static bool set_remote_allowance(bool allowed);
static bool remote_update_maintenance(void);
static bool remote_device_maintenance(void);
static bool remote_transport_admit(void);
static void remote_transport_applied(void);
static bool remote_device_confirm_local(bool confirm, const app_remote_review_request_t *request);
static void remote_device_review_view(app_remote_review_view_t *view);
static char s_media_message[97];
static char s_connectivity_message[97];
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
static unsigned s_wifi_page;
static uint32_t s_wifi_selection_generation;
static uint16_t s_wifi_selection_index;
static app_wifi_selection_t s_wifi_reserved;
static app_local_editor_view_t s_local_editor;
static uint64_t s_editor_sequence, s_editor_deadline_ms;
static app_local_edit_request_t s_local_submission;
static bool s_local_submission_pending;
static uint64_t s_name_request_id;
static bool s_name_should_persist;

static void update_send(bool cancel);
static void sync_interaction(void);

static void clear_private(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length--) *bytes++ = 0;
}

static void cancel_local_editor(void)
{
    if (s_wifi_reserved.id) (void)app_wifi_release_selection(s_wifi_reserved.id);
    clear_private(&s_wifi_reserved, sizeof(s_wifi_reserved));
    clear_private(&s_local_editor, sizeof(s_local_editor));
    s_editor_deadline_ms = 0;
    portENTER_CRITICAL(&s_control_lock);
    clear_private(&s_local_submission, sizeof(s_local_submission));
    s_local_submission_pending = false;
    portEXIT_CRITICAL(&s_control_lock);
}

static app_local_edit_result_t submit_local_edit(const app_local_edit_request_t *request)
{
    if (!request || !request->editor_session || !request->gesture)
        return APP_LOCAL_EDIT_INVALID;
    if (request->purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE) {
        size_t length = request->value.wifi.length;
        if (length < 8 || length >= APP_UI_WIFI_PASSWORD_CAPACITY ||
            request->value.wifi.password[length] != '\0') return APP_LOCAL_EDIT_INVALID;
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)request->value.wifi.password[i] < 0x20 ||
                (unsigned char)request->value.wifi.password[i] > 0x7e)
                return APP_LOCAL_EDIT_INVALID;
    } else if (request->purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME) {
        size_t length = request->value.bluetooth.length;
        if (length >= APP_UI_BLUETOOTH_NAME_CAPACITY ||
            request->value.bluetooth.name[length] != '\0' ||
            !device_settings_bluetooth_name_valid(request->value.bluetooth.name, length))
            return APP_LOCAL_EDIT_INVALID;
    } else return APP_LOCAL_EDIT_INVALID;
    portENTER_CRITICAL(&s_control_lock);
    app_local_edit_result_t result = !s_screen_accepts_input ? APP_LOCAL_EDIT_UNAVAILABLE :
        s_local_submission_pending ? APP_LOCAL_EDIT_BUSY : APP_LOCAL_EDIT_ACCEPTED;
    if (result == APP_LOCAL_EDIT_ACCEPTED) {
        s_local_submission = *request;
        s_local_submission_pending = true;
    }
    portEXIT_CRITICAL(&s_control_lock);
    return result;
}

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
    present_boot(APP_BOOT_UI_FAULT, operation, esp_err_to_name(error));
    for (;;) {
        ESP_LOGE(TAG, "Boot fault: %s: %s; HID disabled", operation, esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static bool submit_intent(app_intent_t *intent)
{
    intent->event_ms = (uint64_t)esp_timer_get_time() / 1000U;
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
    coordinator_event_t event = {.value.intent = *intent};
    if (xQueueSend(s_intents, &event, 0) == pdTRUE) {
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

static bool submit_contacts(const board_touch_snapshot_t *sample)
{
    if (!sample) return false;
    const app_ui_touch_context_t visible = app_ui_touch_context();
    coordinator_event_t event = {.contacts = true};
    interaction_contact_snapshot_t *out = &event.value.touch.sample;
    *out = (interaction_contact_snapshot_t){.sequence = sample->sequence,
        .timestamp_ms = sample->timestamp_ms, .epoch = visible.epoch,
        .ui_revision = visible.ui_revision, .supported = true, .fresh = true,
        .valid = sample->valid, .overflow = sample->overflow, .count = sample->count};
    event.value.touch.eligible = visible.eligible_pad;
    for (uint8_t i = 0; i < sample->count && i < INTERACTION_CONTACT_CAPACITY; ++i) {
        const board_touch_contact_t *point = &sample->contacts[i];
        out->contacts[i] = (interaction_contact_t){.id = point->id, .x = point->x, .y = point->y};
        if (point->x < visible.x1 || point->x > visible.x2 ||
            point->y < visible.y1 || point->y > visible.y2) event.value.touch.eligible = false;
    }
    /* Complete snapshots and primary events share one FIFO; a later sensor
     * update can never overtake an earlier primary pointer event. */
    if (xQueueSend(s_intents, &event, 0) == pdTRUE) return true;
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
        s_overlay != APP_OVERLAY_NONE || s_transport_switch_pending || remote_update_maintenance() ||
            (s_engine.state.mode == INPUT_KEYBOARD && s_idle.asleep),
        board_haptics_active() || s_idle.wake_capture, s_now_ms);
    if (transition) interaction_freeze_after(&s_interaction, transition);
}

static void cancel_input(void)
{
    update_send(true);
    hid_engine_cancel(&s_engine);
    interaction_touch_result_t touch = interaction_touch_cancel(&s_interaction);
    if (touch.effect == INTERACTION_TOUCH_RELEASE_REQUIRED)
        (void)interaction_touch_admit(&s_interaction, touch.token, true);
    s_drag_requested = false;
    (void)interaction_multitouch_cancel(&s_multitouch);
    interaction_multitouch_release_owned(&s_multitouch);
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
    cancel_local_editor();
    if (s_overlay == APP_OVERLAY_WIFI_SETUP && overlay != APP_OVERLAY_WIFI_SETUP)
        (void)app_wifi_stop_setup();
    s_overlay = overlay;
    sync_interaction();
}

/* Media ownership is independent of the selected HID mode and connection.
 * A confirmed capture marker plus a live display owner is required for capture;
 * a queued view or the mere existence of a display never grants permission. */
static ae_result_t recording_wall_time(void *context, uint64_t *seconds)
{
    (void)context;
    return app_control_runtime_wall_time(seconds) ? AE_OK : AE_TIME_UNKNOWN;
}

static bool capture_requires_display(void)
{
    app_media_snapshot_t media;
    return app_media_runtime_snapshot(&media) && media.audio_known &&
        media.audio.capture_indicator_required;
}

static void refresh_media_context(void)
{
    app_presenter_indicator_status_t indicator = app_presenter_indicator_status();
    board_brightness_status_t brightness = board_brightness_status();
    bool visible = !s_idle.asleep && !s_idle.wake_capture &&
        brightness.applied_known && brightness.applied_percent > 0 && brightness.error == ESP_OK;
    ae_environment_t environment = {.unlocked = !s_engine.state.locked,
        .privacy_latched = s_engine.state.locked, .screen_visible = visible,
        .maintenance = remote_update_maintenance(),
        .indicator_visible = visible && indicator.usable,
        .indicator_generation = indicator.capture_generation,
        .indicator_seen_ms = indicator.usable ? indicator.owner_heartbeat_ms : 0};
    bool speech = s_engine.state.mode == INPUT_KEYBOARD &&
        s_keyboard_submode == APP_KEYBOARD_SPOKEN && s_overlay == APP_OVERLAY_NONE;
    app_media_runtime_environment(&environment, s_engine.state.epoch, s_ui_revision, speech);
    app_media_controls_context_t context = {.epoch = s_engine.state.epoch,
        .ui_revision = s_ui_revision, .mode = s_engine.state.mode, .overlay = s_overlay,
        .keyboard_submode = s_keyboard_submode, .locked = s_engine.state.locked,
        .screen_off = !visible, .preview_characters = s_interaction.view.preview_length};
    app_media_controls_context(&context);
}

static void apply_media_effect(const app_media_controls_effect_t *effect)
{
    if (!effect || !effect->handled) return;
    if (effect->message[0]) {
        (void)snprintf(s_media_message, sizeof(s_media_message), "%s", effect->message);
        s_message = s_media_message;
    }
    if (effect->open_overlay) {
        s_confirmation = effect->confirmation;
        open_overlay(effect->overlay);
    }
    if (effect->preview_character && s_engine.state.mode == INPUT_KEYBOARD &&
        s_keyboard_submode == APP_KEYBOARD_SPOKEN && !s_engine.state.locked &&
        s_overlay == APP_OVERLAY_NONE && interaction_append(&s_interaction,
            (unsigned char)effect->preview_character)) {
        ++s_ui_revision;
        board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
    }
}

static void service_media(void)
{
    refresh_media_context();
    app_media_controls_effect_t effect = {0};
    app_media_controls_poll(s_now_ms, &effect);
    apply_media_effect(&effect);
}

static void refresh_connectivity_context(void)
{
    app_connectivity_controls_context_t context = {.epoch = s_engine.state.epoch,
        .ui_revision = s_ui_revision, .mode = s_engine.state.mode, .overlay = s_overlay,
        .settings_page = s_settings_page, .confirmation = s_confirmation,
        .locked = s_engine.state.locked, .screen_off = s_idle.asleep || s_idle.wake_capture,
        .release_pending = s_engine.state.release_pending,
        .transport_switch_pending = s_transport_switch_pending};
    app_connectivity_controls_context(&context);
}

static void apply_connectivity_effect(const app_connectivity_controls_effect_t *effect)
{
    if (!effect || !effect->handled) return;
    if (effect->message[0]) {
        (void)snprintf(s_connectivity_message, sizeof(s_connectivity_message), "%s", effect->message);
        s_message = s_connectivity_message;
    }
    if (effect->open_overlay) {
        s_confirmation = effect->confirmation;
        open_overlay(effect->overlay);
    } else if (effect->invalidate_ui && s_ui_revision != UINT64_MAX) ++s_ui_revision;
}

static void service_connectivity(void)
{
    refresh_connectivity_context();
    app_connectivity_controls_effect_t effect = {0};
    app_connectivity_controls_poll(s_now_ms, &effect);
    apply_connectivity_effect(&effect);
}

static bool set_motion(input_owner_t source, bool enable)
{
    if (enable && remote_update_maintenance()) return false;
    if (enable && !s_interaction.view.calibrated) {
        s_message = "Keep still to calibrate motion first.";
        return false;
    }
    if (!hid_engine_set_gyro_owned(&s_engine, source, enable)) return false;
    if (!interaction_set_motion_owned(&s_interaction, source, enable)) {
        (void)hid_engine_set_gyro_owned(&s_engine, source, false);
        return false;
    }
    s_message = enable ? "Motion on. Touch pauses motion." : "Motion off.";
    board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
    return true;
}
static void toggle_motion(void) { (void)set_motion(INPUT_LOCAL, !s_engine.state.gyro_enabled); }

static bool activate_primary(input_owner_t source)
{
    if (remote_update_maintenance() || s_overlay != APP_OVERLAY_NONE ||
        s_engine.state.locked || s_engine.state.owner != source) return false;
    if (s_engine.state.mode == INPUT_MOUSE) return set_motion(source, !s_engine.state.gyro_enabled);
    if (s_engine.state.mode == INPUT_KEYBOARD &&
        (s_keyboard_submode == APP_KEYBOARD_MOTION || s_keyboard_submode == APP_KEYBOARD_TOUCH)) {
        interaction_edit_t edit = {.kind = INTERACTION_EDIT_CONFIRM};
        if (interaction_edit_owned(&s_interaction, source, &edit)) {
            ++s_ui_revision; board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
            s_message = "Preview only; press Send when ready.";
            return true;
        }
    }
    return false;
}

static bool configure_motion_preferences(const device_settings_t *settings)
{
    interaction_config_t config = s_interaction.config;
    config.pointer_gain_x = (float)settings->motion_gain_x_centi / 100.0f;
    config.pointer_gain_y = (float)settings->motion_gain_y_centi / 100.0f;
    config.deadzone_dps = (float)settings->motion_deadzone_mdps / 1000.0f;
    config.filter_seconds = (float)settings->motion_filter_ms / 1000.0f;
    config.freeze_ms = settings->motion_freeze_ms;
    config.invert_x = settings->motion_invert_x;
    config.invert_y = settings->motion_invert_y;
    /* Preserve the established keyboard selection axes after the board owner
     * applies its proper mounting rotation to both physical sensors. */
    config.selector_axis_x = -3;
    config.selector_axis_y = 2;
    return interaction_configure(&s_interaction, &config);
}

static bool persist_settings(const device_settings_t *candidate)
{
    if (settings_store_request(candidate) == ESP_OK) {
        s_durable_settings = *candidate;
        return true;
    }
    s_message = "Setting applied; storage unavailable.";
    return false;
}

static bool update_settings_internal(const device_settings_t *candidate, bool persist)
{
    if (s_settings_revision == UINT64_MAX) return false;
    device_settings_apply_t result = device_settings_apply(&s_settings, candidate);
    if (result == DEVICE_SETTINGS_APPLY_INVALID) return false;
    if (result == DEVICE_SETTINGS_APPLY_UNCHANGED) {
        if (persist && !device_settings_equal(candidate, &s_durable_settings)) persist_settings(candidate);
        return true;
    }
    ++s_settings_revision;
    s_dark_theme = s_settings.theme == DEVICE_THEME_DARK;
    s_host_os = s_settings.host_modifier == DEVICE_HOST_COMMAND ? APP_HOST_COMMAND : APP_HOST_CTRL;
    interaction_touch_config_t touch = s_interaction.touch_config;
    touch.tap_to_click = s_settings.tap_to_click;
    (void)interaction_touch_configure(&s_interaction, &touch);
    (void)configure_motion_preferences(&s_settings);
    board_haptics_enable(s_settings.haptics_enabled);
    (void)device_idle_configure(&s_idle, s_settings.idle_timeout, s_now_ms);
    (void)device_idle_activity(&s_idle, s_now_ms);
    if (persist) persist_settings(candidate);
    ++s_ui_revision;
    return true;
}

static bool update_settings(const device_settings_t *candidate)
{
    if (!device_settings_validate(candidate)) return false;
    device_settings_t durable = s_durable_settings;
    /* A native single-field edit persists its own changed values only. It
     * cannot accidentally make an unrelated remote temporary override durable. */
#define COPY_CHANGED(field) do { if (candidate->field != s_settings.field) durable.field = candidate->field; } while (0)
    COPY_CHANGED(theme); COPY_CHANGED(host_modifier); COPY_CHANGED(haptics_enabled);
    COPY_CHANGED(brightness_percent); COPY_CHANGED(idle_timeout); COPY_CHANGED(preferred_transport);
    COPY_CHANGED(tap_to_click); COPY_CHANGED(motion_gain_x_centi); COPY_CHANGED(motion_gain_y_centi);
    COPY_CHANGED(motion_deadzone_mdps); COPY_CHANGED(motion_filter_ms); COPY_CHANGED(motion_freeze_ms);
    COPY_CHANGED(motion_invert_x); COPY_CHANGED(motion_invert_y);
#undef COPY_CHANGED
    if (strcmp(candidate->bluetooth_name, s_settings.bluetooth_name))
        memcpy(durable.bluetooth_name, candidate->bluetooth_name, sizeof(durable.bluetooth_name));
    bool changed = !device_settings_equal(&s_settings, candidate);
    if (!update_settings_internal(candidate, false)) return false;
    if (changed) persist_settings(&durable);
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
    /* Keep the acknowledged privacy marker readable until the codec confirms
     * stop. This does not manufacture user activity or change the saved idle
     * timeout; normal idle evaluation resumes immediately after capture. */
    if (!held && !capture_requires_display()) (void)device_idle_tick(&s_idle, s_now_ms);
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

static bool selected_wifi_record(app_wifi_scan_record_t *record)
{
    return s_wifi_selection_generation &&
        app_wifi_get_scan_record(s_wifi_selection_generation, s_wifi_selection_index, record) == ESP_OK &&
        record->availability == APP_WIFI_JOIN_AVAILABLE;
}

static void open_local_editor(app_local_edit_purpose_t purpose, bool remember)
{
    if (s_editor_sequence == UINT64_MAX) {
        s_message = "Editor session limit reached; restart to continue.";
        return;
    }
    open_overlay(APP_OVERLAY_LOCAL_EDITOR);
    s_local_editor.purpose = purpose;
    s_local_editor.session = ++s_editor_sequence;
    s_local_editor.submit_available = true;
    s_local_editor.wifi_scan_generation = s_wifi_selection_generation;
    s_local_editor.wifi_record_index = s_wifi_selection_index;
    s_local_editor.wifi_remember = remember;
    if (purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME)
        (void)snprintf(s_local_editor.bluetooth_initial, sizeof(s_local_editor.bluetooth_initial),
                       "%s", s_settings.bluetooth_name);
    s_editor_deadline_ms = s_now_ms > UINT64_MAX - APP_UI_LOCAL_EDIT_TIMEOUT_MS ? UINT64_MAX :
        s_now_ms + APP_UI_LOCAL_EDIT_TIMEOUT_MS;
}

static void service_local_editor(void)
{
    if (s_local_editor.purpose == APP_LOCAL_EDIT_NONE) return;
    bool expired = s_now_ms >= s_editor_deadline_ms;
    if (s_overlay != APP_OVERLAY_LOCAL_EDITOR || s_engine.state.locked || s_idle.asleep ||
        s_idle.clock_regressed || expired) {
        cancel_local_editor();
        if (s_overlay == APP_OVERLAY_LOCAL_EDITOR) {
            s_overlay = APP_OVERLAY_NONE;
            ++s_ui_revision;
            sync_interaction();
        }
        if (expired) s_message = "Local editor expired; its draft was cleared.";
        return;
    }
    /* Let the ordinary physical BEGIN/END lifecycle reach the coordinator first.
     * A bounded private slot cannot overtake an earlier touch or cancel event. */
    if (uxQueueMessagesWaiting(s_intents)) return;
    app_local_edit_request_t request;
    portENTER_CRITICAL(&s_control_lock);
    bool pending = s_local_submission_pending;
    request = s_local_submission;
    clear_private(&s_local_submission, sizeof(s_local_submission));
    s_local_submission_pending = false;
    portEXIT_CRITICAL(&s_control_lock);
    if (!pending) {
        clear_private(&request, sizeof(request));
        return;
    }
    app_intent_t touch = {.action = APP_KEY, .gesture = request.gesture};
    bool current = request.editor_session == s_local_editor.session &&
        request.purpose == s_local_editor.purpose && request.epoch == s_engine.state.epoch &&
        request.ui_revision == s_ui_revision && s_engine.state.owner == INPUT_LOCAL &&
        !s_transport_switch_pending && request.gesture != s_consumed_gesture && route_touch(&touch);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (current && request.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE) {
        if (request.value.wifi.scan_generation == s_local_editor.wifi_scan_generation &&
            request.value.wifi.record_index == s_local_editor.wifi_record_index &&
            request.value.wifi.remember == s_local_editor.wifi_remember && s_wifi_reserved.id &&
            s_wifi_reserved.scan_generation == request.value.wifi.scan_generation &&
            s_wifi_reserved.record_index == request.value.wifi.record_index &&
            s_wifi_reserved.record.ssid_length < APP_WIFI_SSID_CAPACITY) {
            app_wifi_join_request_t join = {.remember = request.value.wifi.remember,
                .scan_generation = request.value.wifi.scan_generation,
                .record_index = request.value.wifi.record_index, .selection_id = s_wifi_reserved.id};
            memcpy(join.ssid, s_wifi_reserved.record.ssid, s_wifi_reserved.record.ssid_length);
            memcpy(join.password, request.value.wifi.password, request.value.wifi.length);
            result = app_wifi_join(&join);
            clear_private(&join, sizeof(join));
        }
        s_message = result == ESP_OK ? "Wi-Fi join queued; wait for a LAN address." :
            "Wi-Fi join rejected; scan again and re-enter the password.";
        s_settings_page = APP_SETTINGS_WIFI;
    } else if (current && request.purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME) {
        if (!s_name_should_persist) {
            uint64_t request_id = 0;
            result = ble_hid_name_request(request.value.bluetooth.name,
                request.value.bluetooth.length, &request_id);
            if (result == ESP_OK) {
                s_name_request_id = request_id;
                s_name_should_persist = true;
            }
        }
        s_message = result == ESP_OK ? "Bluetooth name queued; waiting for radio acknowledgement." :
            "Bluetooth name rejected; current settings are preserved.";
        s_settings_page = APP_SETTINGS_BLUETOOTH;
    } else {
        s_message = "Local edit expired or changed; open a new editor.";
    }
    clear_private(&request, sizeof(request));
    open_overlay(APP_OVERLAY_SETTINGS);
}

static void service_bluetooth_name(void)
{
    if (!s_name_should_persist) return;
    ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
    if (name.request_id != s_name_request_id) return;
    if (name.status == BLE_HID_NAME_APPLIED && name.active_known) {
        device_settings_t candidate = s_settings;
        (void)snprintf(candidate.bluetooth_name, sizeof(candidate.bluetooth_name), "%s", name.active);
        s_name_should_persist = false;
        (void)update_settings(&candidate);
        s_message = "Bluetooth name applied; hosts may retain a cached name.";
    } else if (name.status == BLE_HID_NAME_FAILED || name.status == BLE_HID_NAME_UNCERTAIN) {
        s_name_should_persist = false;
        s_message = name.status == BLE_HID_NAME_UNCERTAIN ?
            "Bluetooth name uncertain; restart before another change." :
            "Bluetooth name change failed; check connection status.";
        ++s_ui_revision;
    }
}

static bool allowed_in_overlay(app_action_t action)
{
    if (s_overlay == APP_OVERLAY_NONE) return true;
    if (s_overlay == APP_OVERLAY_CONFIRM) return action == APP_CONFIRM;
    if (action == APP_CLOSE) return true;
    if (action == APP_MENU)
        return s_overlay == APP_OVERLAY_MODES || s_overlay == APP_OVERLAY_TARGET ||
            s_overlay == APP_OVERLAY_SETTINGS || s_overlay == APP_OVERLAY_WIFI_SETUP ||
            s_overlay == APP_OVERLAY_KEYBOARD || s_overlay == APP_OVERLAY_AUDIO ||
            s_overlay == APP_OVERLAY_WIFI_NETWORK || s_overlay == APP_OVERLAY_MEDIA_OPTIONS ||
            s_overlay == APP_OVERLAY_RECORDINGS || s_overlay == APP_OVERLAY_RECORDING_DETAIL ||
            s_overlay == APP_OVERLAY_RECORDING_START || s_overlay == APP_OVERLAY_RECORDING_STORAGE ||
            s_overlay == APP_OVERLAY_API_CONNECTION;
    if (s_overlay == APP_OVERLAY_MODES) return action == APP_MODE;
    if (s_overlay == APP_OVERLAY_KEYBOARD) return action == APP_KEYBOARD_SUBMODE;
    if (s_overlay == APP_OVERLAY_AUDIO) return action == APP_AUDIO_KIND;
    if (s_overlay == APP_OVERLAY_MEDIA_OPTIONS || s_overlay == APP_OVERLAY_RECORDINGS ||
        s_overlay == APP_OVERLAY_RECORDING_DETAIL || s_overlay == APP_OVERLAY_RECORDING_START)
        return action == APP_MEDIA_CONTROL;
    if (s_overlay == APP_OVERLAY_RECORDING_STORAGE) return action == APP_RECORDING_STORAGE_PREPARE_REQUEST;
    if (s_overlay == APP_OVERLAY_API_CONNECTION)
        return action == APP_API_COMMISSIONING || action == APP_API_INPUT_ALLOW_SET || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_API_REVIEW) return action == APP_API_COMMISSIONING;
    if (s_overlay == APP_OVERLAY_WIFI_PROFILES) return action == APP_WIFI_PROFILE_CONTROL || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_BLUETOOTH_TARGET) return action == APP_BLUETOOTH_TARGET_CONTROL || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_COMMON_KEYS) return action == APP_KEY;
    if (s_overlay == APP_OVERLAY_LOCAL_EDITOR) return false;
    if (s_overlay == APP_OVERLAY_WIFI_NETWORKS)
        return action == APP_WIFI_SCAN || action == APP_WIFI_SCAN_PAGE ||
            action == APP_WIFI_NETWORK_SELECT || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_WIFI_NETWORK)
        return action == APP_WIFI_CREDENTIAL_OPEN || action == APP_WIFI_SCAN || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_WIFI_SETUP)
        return action == APP_WIFI_SETUP || action == APP_SETTINGS_PAGE;
    if (s_overlay == APP_OVERLAY_TARGET)
        return action == APP_RECOVER || action == APP_PAIRING_REQUEST || action == APP_SETTINGS_PAGE;
    if (s_overlay != APP_OVERLAY_SETTINGS) return false;
    if (action == APP_SETTINGS_PAGE) return true;
    switch (s_settings_page) {
    case APP_SETTINGS_INPUT:
        return action == APP_HAPTICS || action == APP_HOST_OS || action == APP_CALIBRATE ||
            action == APP_TAP_TO_CLICK || action == APP_MOTION_CONFIG;
    case APP_SETTINGS_MOTION:
        return action == APP_MOTION_CONFIG || action == APP_CALIBRATE;
    case APP_SETTINGS_MOTION_ADVANCED:
        return action == APP_MOTION_CONFIG;
    case APP_SETTINGS_GESTURES:
        return action == APP_TAP_TO_CLICK;
    case APP_SETTINGS_DISPLAY:
        return action == APP_THEME || action == APP_BRIGHTNESS || action == APP_IDLE_TIMEOUT;
    case APP_SETTINGS_CONNECTIVITY:
        return action == APP_TRANSPORT_SELECT;
    case APP_SETTINGS_BLUETOOTH:
        return action == APP_PAIRING_REQUEST || action == APP_BLUETOOTH_PAGE ||
            action == APP_BLUETOOTH_NAME_EDIT || action == APP_BLUETOOTH_TARGET_CONTROL;
    case APP_SETTINGS_WIFI:
        return action == APP_WIFI_SETUP || action == APP_WIFI_FORGET_REQUEST || action == APP_WIFI_SCAN;
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
    if (!s_transport_switch_pending || !remote_transport_admit()) return;
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
    remote_transport_applied();
    device_settings_t candidate = s_settings;
    candidate.preferred_transport = s_input_transport == APP_INPUT_USB ?
        DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH;
    if (s_transport_switch_persist) (void)update_settings(&candidate);
    else (void)update_settings_internal(&candidate, false);
    ++s_ui_revision;
    s_message = old_disconnected ?
        "Connection selected; old host was disconnected. Pending input discarded." :
        "Connection selected; previous input released. Motion stays off.";
    sync_interaction();
}

static bool apply_touch_result(interaction_touch_result_t result)
{
    if (result.effect == INTERACTION_TOUCH_NONE) return result.allow_motion;
    if (result.effect == INTERACTION_TOUCH_RELEASE_REQUIRED) {
        cancel_input();
        return false;
    }
    hid_engine_enqueue_result_t admitted;
    if (result.effect == INTERACTION_TOUCH_CLICK)
        admitted = hid_engine_enqueue_mouse_click(&s_engine, s_engine.state.epoch, INPUT_LOCAL, 1);
    else
        admitted = hid_engine_enqueue_mouse_buttons(&s_engine, s_engine.state.epoch, INPUT_LOCAL,
            result.effect == INTERACTION_TOUCH_LEFT_DOWN ? 1U : 0U);
    interaction_touch_result_t next = interaction_touch_admit(&s_interaction, result.token,
        admitted == HID_ENGINE_ENQUEUE_OK);
    if (next.effect == INTERACTION_TOUCH_RELEASE_REQUIRED) {
        cancel_input();
        return false;
    }
    return admitted == HID_ENGINE_ENQUEUE_OK && result.allow_motion;
}

static void cancel_touch_recognition(void)
{
    (void)apply_touch_result(interaction_touch_cancel(&s_interaction));
}

static interaction_multitouch_context_t multitouch_context(void)
{
    return (interaction_multitouch_context_t){.epoch = s_engine.state.epoch,
        .ui_revision = s_ui_revision,
        .enabled = s_multitouch_available && s_multitouch_eligible && s_settings.tap_to_click &&
            !remote_update_maintenance() &&
            s_engine.state.mode == INPUT_MOUSE && s_engine.state.owner == INPUT_LOCAL &&
            !s_engine.state.locked && !s_engine.state.release_pending && s_engine.ready &&
            !s_engine.congested && s_overlay == APP_OVERLAY_NONE &&
            !s_transport_switch_pending && !s_idle.asleep && !s_idle.wake_capture,
        .drag_held = s_drag_requested || interaction_touch_holding(&s_interaction)};
}

static void apply_multitouch(interaction_multitouch_result_t result)
{
    s_multitouch_consuming = result.consume_pointer;
    if (result.cancel_single) cancel_touch_recognition();
    if (result.release_required) {
        cancel_input();
        interaction_multitouch_release_owned(&s_multitouch);
        return;
    }
    const interaction_multitouch_context_t context = multitouch_context();
    if (!context.enabled) return;
    if (result.right_click) (void)hid_engine_enqueue_mouse_click(&s_engine,
        s_engine.state.epoch, INPUT_LOCAL, 2);
    if (result.scroll_y) (void)hid_engine_enqueue_mouse_scroll(&s_engine,
        s_engine.state.epoch, INPUT_LOCAL, result.scroll_y);
}

static void handle_contacts(const coordinator_event_t *event)
{
    s_now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    const interaction_contact_snapshot_t *sample = &event->value.touch.sample;
    if (sample->epoch != s_engine.state.epoch || sample->ui_revision != s_ui_revision) return;
    s_multitouch_eligible = event->value.touch.eligible;
    interaction_multitouch_context_t context = multitouch_context();
    apply_multitouch(interaction_multitouch_input(&s_multitouch, sample, &context, s_now_ms));
}

static void service_multitouch(void)
{
    interaction_multitouch_context_t context = multitouch_context();
    apply_multitouch(interaction_multitouch_tick(&s_multitouch, &context, s_now_ms));
}

static bool process_touch_gesture(const app_intent_t *intent)
{
    if (s_multitouch_consuming && intent->surface == APP_SURFACE_TOUCHPAD) return false;
    if (intent->surface != APP_SURFACE_TOUCHPAD) {
        if (intent->action == APP_TOUCH_BEGIN) cancel_touch_recognition();
        return false;
    }
    interaction_touch_event_t event = {.gesture = intent->gesture, .epoch = intent->epoch,
        .ui_revision = intent->ui_revision, .timestamp_ms = intent->event_ms,
        .x = (float)intent->x, .y = (float)intent->y};
    switch (intent->action) {
    case APP_TOUCH_BEGIN: event.phase = INTERACTION_TOUCH_BEGIN; break;
    case APP_TOUCH_MOVE: event.phase = INTERACTION_TOUCH_MOVE; break;
    case APP_TOUCH_END: event.phase = INTERACTION_TOUCH_END; break;
    default: event.phase = INTERACTION_TOUCH_CANCEL; break;
    }
    bool allowed = s_overlay == APP_OVERLAY_NONE && !s_drag_requested &&
        !s_transport_switch_pending && !s_idle.asleep && !s_idle.wake_capture &&
        intent->gesture != s_consumed_gesture;
    return apply_touch_result(interaction_touch_input(&s_interaction, &event, s_now_ms, s_ui_revision, allowed));
}

/* Shared semantic transitions used by local controls and authenticated
 * coordinator commands. Neither caller can bypass the old-input release gate. */
static bool select_mode(input_mode_t mode)
{
    if (mode < INPUT_MOUSE || mode > INPUT_AUDIO) return false;
    if (mode == s_engine.state.mode) {
        if (s_overlay != APP_OVERLAY_NONE) {
            s_overlay = APP_OVERLAY_NONE; ++s_ui_revision; sync_interaction();
        }
        return true;
    }
    update_send(true);
    if (!hid_engine_set_mode(&s_engine, mode)) return false;
    s_drag_requested = false; s_overlay = APP_OVERLAY_NONE; ++s_ui_revision;
    sync_interaction();
    s_message = mode == INPUT_AUDIO ? "Audio routing is independent of the input connection." :
        "Mode changed; previous input released.";
    board_haptic_feedback(BOARD_FEEDBACK_MODE);
    return true;
}
static bool select_keyboard_submode(app_keyboard_submode_t submode)
{
    if (s_engine.state.mode != INPUT_KEYBOARD || submode < APP_KEYBOARD_MOTION || submode > APP_KEYBOARD_SHORTCUTS)
        return false;
    cancel_input(); s_keyboard_submode = submode;
    interaction_back(&s_interaction);
    interaction_set_keyboard_motion(&s_interaction, submode == APP_KEYBOARD_MOTION);
    if (s_overlay == APP_OVERLAY_KEYBOARD) { s_overlay = APP_OVERLAY_NONE; sync_interaction(); }
    return true;
}
static bool request_input_transport_internal(app_input_transport_t target, const ble_hid_snapshot_t *link, bool persist)
{
    if (target != APP_INPUT_BLUETOOTH && target != APP_INPUT_USB) return false;
    if (s_transport_switch_pending) return false;
    if (target == s_input_transport) return true;
    usb_hid_snapshot_t usb = usb_hid_snapshot();
    bool available = target == APP_INPUT_USB ? usb.initialized && !usb.failed : link->initialized && !link->failed;
    if (!available) { s_message = "Requested connection is unavailable."; return false; }
    cancel_input();
    s_transport_origin_generation = input_link_snapshot(link).generation;
    s_requested_transport = target; s_transport_switch_pending = true;
    s_transport_switch_persist = persist;
    s_transport_switch_deadline_ms = s_now_ms > UINT64_MAX - 5000U ? UINT64_MAX : s_now_ms + 5000U;
    s_message = "Releasing old input before switching connection.";
    return true;
}
static bool request_input_transport(app_input_transport_t target, const ble_hid_snapshot_t *link)
{ return request_input_transport_internal(target, link, true); }
static void recover_input(const ble_hid_snapshot_t *link)
{
    cancel_input(); input_link_t active = input_link_snapshot(link);
    hid_engine_set_link(&s_engine, active.connected, active.ready);
    s_message = "Link recovery requested; old input will not replay.";
}

static void handle_intent(const app_intent_t *intent, const ble_hid_snapshot_t *link)
{
    /* A callback may enqueue after the loop's initial clock sample. Observe the
     * actual clock rather than treating that legitimate event as future input. */
    s_now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    if (!route_touch(intent)) { cancel_touch_recognition(); sync_interaction(); return; }
    if (remote_update_maintenance() && (intent->action == APP_TOUCH_BEGIN ||
        intent->action == APP_TOUCH_END || intent->action == APP_TOUCH_CANCEL)) {
        cancel_touch_recognition(); sync_interaction(); return;
    }
    if (intent->action == APP_TOUCH_BEGIN) {
        s_touch_gesture = intent->gesture;
        bool remote_owned = s_engine.state.owner == INPUT_REMOTE;
        remote_local_override(false);
        if (remote_owned) {
            s_consumed_gesture = intent->gesture;
            update_send(true);
            (void)hid_engine_set_owner(&s_engine, INPUT_LOCAL);
            s_drag_requested = false;
            ++s_ui_revision;
            s_message = "Local control restored; remote work stopped.";
        }
        sync_interaction();
        (void)process_touch_gesture(intent);
        return;
    }
    if (intent->action == APP_TOUCH_END || intent->action == APP_TOUCH_CANCEL) {
        (void)process_touch_gesture(intent);
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
    if (remote_update_maintenance()) {
        s_message = "Firmware maintenance in progress. Input is paused.";
        return;
    }
    if (s_transport_switch_pending) return;
    if (s_engine.state.mode != INPUT_KEYBOARD &&
        (intent->action == APP_BACK || intent->action == APP_CASE ||
         intent->action == APP_SYMBOLS || intent->action == APP_DELETE ||
         intent->action == APP_SPACE || intent->action == APP_CLEAR ||
         intent->action == APP_SEND || intent->action == APP_STOP)) return;
    /* A modal and its settings page own focus. */
    if (!allowed_in_overlay(intent->action)) return;
    if (intent->action == APP_CONFIRM && s_confirmation == APP_CONFIRM_REMOTE && (intent->x == 0 || intent->x == 1) &&
        remote_device_confirm_local(intent->x != 0, &intent->remote_review)) return;
    if (intent->action == APP_API_INPUT_ALLOW_SET) {
        if (s_overlay != APP_OVERLAY_API_CONNECTION || (intent->x != 0 && intent->x != 1) ||
            !intent->api.revision || intent->api.revision != s_remote_allowance_generation) return;
        if (!set_remote_allowance(intent->x != 0)) {
            s_message = "Remote input unavailable; keep local control.";
            return;
        }
        /* Return to the core surface as part of granting permission. A second
         * local tap to close the dialog would immediately revoke the grant. */
        if (intent->x) open_overlay(APP_OVERLAY_NONE);
        else ++s_ui_revision;
        s_message = intent->x ? "Remote input allowed. Touch or press AI to take over." :
            "Remote input blocked. Independent media and API remain available.";
        return;
    }
    refresh_media_context();
    refresh_connectivity_context();
    app_connectivity_controls_effect_t connectivity_effect = {0};
    if (app_connectivity_controls_handle(intent, &connectivity_effect)) {
        apply_connectivity_effect(&connectivity_effect);
        sync_interaction();
        return;
    }
    app_media_controls_effect_t media_effect = {0};
    if (app_media_controls_handle(intent, &media_effect)) {
        apply_media_effect(&media_effect);
        sync_interaction();
        return;
    }
    hid_engine_enqueue_result_t result = HID_ENGINE_ENQUEUE_BLOCKED;
    bool edited = false;
    switch (intent->action) {
    case APP_MODE:
        (void)select_mode((input_mode_t)intent->x);
        return;
    case APP_MENU:
        if ((intent->x >= APP_OVERLAY_MODES && intent->x <= APP_OVERLAY_SETTINGS) ||
            (intent->x == APP_OVERLAY_KEYBOARD && s_overlay == APP_OVERLAY_NONE &&
             s_engine.state.mode == INPUT_KEYBOARD) ||
            (intent->x == APP_OVERLAY_AUDIO && s_overlay == APP_OVERLAY_NONE &&
             s_engine.state.mode == INPUT_AUDIO) ||
            (intent->x == APP_OVERLAY_COMMON_KEYS &&
             (s_overlay == APP_OVERLAY_NONE || s_overlay == APP_OVERLAY_KEYBOARD) &&
             s_engine.state.mode == INPUT_KEYBOARD) ||
            (intent->x == APP_OVERLAY_WIFI_NETWORKS && s_overlay == APP_OVERLAY_WIFI_NETWORK) ||
            (intent->x == APP_OVERLAY_API_REVIEW && s_overlay == APP_OVERLAY_API_CONNECTION) ||
            (s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_WIFI &&
             (intent->x == APP_OVERLAY_WIFI_PROFILES || intent->x == APP_OVERLAY_API_CONNECTION)) ||
            (s_engine.state.mode == INPUT_AUDIO &&
             (intent->x == APP_OVERLAY_MEDIA_OPTIONS || intent->x == APP_OVERLAY_RECORDINGS ||
              intent->x == APP_OVERLAY_RECORDING_START || intent->x == APP_OVERLAY_RECORDING_STORAGE))) {
            if (intent->x == APP_OVERLAY_SETTINGS) s_settings_page = APP_SETTINGS_HOME;
            open_overlay((app_overlay_t)intent->x);
        }
        return;
    case APP_CLOSE:
        if (s_overlay == APP_OVERLAY_WIFI_SETUP) (void)app_wifi_stop_setup();
        cancel_local_editor();
        s_overlay = APP_OVERLAY_NONE;
        ++s_ui_revision;
        sync_interaction();
        return;
    case APP_RECOVER:
        recover_input(link);
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
    case APP_TAP_TO_CLICK:
        if (s_overlay != APP_OVERLAY_SETTINGS ||
            (s_settings_page != APP_SETTINGS_INPUT && s_settings_page != APP_SETTINGS_GESTURES) ||
            (intent->x != 0 && intent->x != 1)) return;
        {
            cancel_input();
            device_settings_t candidate = s_settings;
            candidate.tap_to_click = intent->x != 0;
            (void)update_settings(&candidate);
        }
        return;
    case APP_MOTION_CONFIG: {
        if (s_overlay != APP_OVERLAY_SETTINGS ||
            (s_settings_page != APP_SETTINGS_INPUT && s_settings_page != APP_SETTINGS_MOTION &&
             s_settings_page != APP_SETTINGS_MOTION_ADVANCED) ||
            intent->motion_patch.expected_settings_revision != s_settings_revision ||
            s_settings_revision == UINT64_MAX) return;
        device_settings_t candidate = s_settings;
        const app_motion_patch_t *patch = &intent->motion_patch;
        switch (patch->field) {
        case APP_MOTION_FIELD_GAIN_X: candidate.motion_gain_x_centi = patch->value.gain_centi; break;
        case APP_MOTION_FIELD_GAIN_Y: candidate.motion_gain_y_centi = patch->value.gain_centi; break;
        case APP_MOTION_FIELD_DEADZONE: candidate.motion_deadzone_mdps = patch->value.deadzone_mdps; break;
        case APP_MOTION_FIELD_FILTER: candidate.motion_filter_ms = patch->value.milliseconds; break;
        case APP_MOTION_FIELD_FREEZE: candidate.motion_freeze_ms = patch->value.milliseconds; break;
        case APP_MOTION_FIELD_INVERT_X: candidate.motion_invert_x = patch->value.inverted; break;
        case APP_MOTION_FIELD_INVERT_Y: candidate.motion_invert_y = patch->value.inverted; break;
        default: return;
        }
        if (!device_settings_validate(&candidate)) return;
        /* Settings changes remain local and cannot append a host action. */
        if (update_settings(&candidate)) board_haptic_feedback(BOARD_FEEDBACK_CONFIRM);
        return;
    }
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
        if (s_overlay != APP_OVERLAY_SETTINGS ||
            (s_settings_page != APP_SETTINGS_INPUT && s_settings_page != APP_SETTINGS_MOTION)) return;
        cancel_input();
        interaction_calibrate(&s_interaction);
        s_message = "Keep the device still while calibrating.";
        return;
    case APP_SETTINGS_PAGE:
        if ((s_overlay == APP_OVERLAY_API_CONNECTION || s_overlay == APP_OVERLAY_WIFI_PROFILES) &&
            intent->x != APP_SETTINGS_WIFI) return;
        if (s_overlay == APP_OVERLAY_BLUETOOTH_TARGET && intent->x != APP_SETTINGS_BLUETOOTH) return;
        if (intent->x < APP_SETTINGS_HOME || intent->x > APP_SETTINGS_GESTURES) return;
        s_settings_page = (app_settings_page_t)intent->x;
        open_overlay(APP_OVERLAY_SETTINGS);
        return;
    case APP_TRANSPORT_SELECT:
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_CONNECTIVITY) return;
        (void)request_input_transport((app_input_transport_t)intent->x, link);
        return;
    case APP_BLUETOOTH_PAGE: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_BLUETOOTH || intent->x < 0) return;
        ble_hid_bond_list_t bonds = ble_hid_bond_list();
        unsigned pages = bonds.known && bonds.count ?
            ((unsigned)bonds.count + APP_UI_BONDS_PER_PAGE - 1U) / APP_UI_BONDS_PER_PAGE : 1U;
        s_bluetooth_page = (unsigned)intent->x < pages ? (unsigned)intent->x : pages - 1U;
        ++s_ui_revision;
        return;
    }
    case APP_BLUETOOTH_NAME_EDIT: {
        if (s_overlay != APP_OVERLAY_SETTINGS || s_settings_page != APP_SETTINGS_BLUETOOTH) return;
        ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
        if (!link->initialized || link->failed || s_name_should_persist ||
            name.status == BLE_HID_NAME_PENDING || name.status == BLE_HID_NAME_APPLYING ||
            name.status == BLE_HID_NAME_UNCERTAIN) {
            s_message = "Bluetooth name editing is temporarily unavailable.";
            return;
        }
        open_local_editor(APP_LOCAL_EDIT_BLUETOOTH_NAME, false);
        return;
    }
    case APP_WIFI_SCAN: {
        if (intent->x != 0 && intent->x != 1) return;
        app_wifi_scan_snapshot_t scan;
        app_wifi_get_scan_snapshot(&scan);
        if (intent->x == 0) {
            if (intent->wifi_scan_generation != scan.generation) return;
            s_message = app_wifi_scan_cancel() == ESP_OK ? "Wi-Fi scan cancellation queued." :
                "Wi-Fi scan cannot be cancelled now.";
            return;
        }
        esp_err_t scan_error = app_wifi_scan_start();
        if (scan_error != ESP_OK) {
            s_message = "Wi-Fi scan unavailable; wait for current network work.";
            return;
        }
        s_wifi_page = 0;
        s_wifi_selection_generation = 0;
        open_overlay(APP_OVERLAY_WIFI_NETWORKS);
        return;
    }
    case APP_WIFI_SCAN_PAGE: {
        if (s_overlay != APP_OVERLAY_WIFI_NETWORKS || intent->x < 0) return;
        app_wifi_scan_snapshot_t scan;
        app_wifi_get_scan_snapshot(&scan);
        if (scan.state != APP_WIFI_SCAN_READY || scan.generation != intent->wifi_scan_generation) return;
        unsigned pages = scan.count ? (scan.count + APP_UI_WIFI_ROWS_PER_PAGE - 1U) /
            APP_UI_WIFI_ROWS_PER_PAGE : 1U;
        if ((unsigned)intent->x >= pages) return;
        s_wifi_page = (unsigned)intent->x;
        ++s_ui_revision;
        return;
    }
    case APP_WIFI_NETWORK_SELECT: {
        if (s_overlay != APP_OVERLAY_WIFI_NETWORKS) return;
        app_wifi_scan_record_t record;
        if (app_wifi_get_scan_record(intent->wifi_scan_generation, intent->wifi_record_index, &record) != ESP_OK) {
            s_message = "Wi-Fi results expired; scan again.";
            return;
        }
        s_wifi_selection_generation = intent->wifi_scan_generation;
        s_wifi_selection_index = intent->wifi_record_index;
        open_overlay(APP_OVERLAY_WIFI_NETWORK);
        return;
    }
    case APP_WIFI_CREDENTIAL_OPEN: {
        if (s_overlay != APP_OVERLAY_WIFI_NETWORK || (intent->x != 0 && intent->x != 1) ||
            intent->wifi_scan_generation != s_wifi_selection_generation ||
            intent->wifi_record_index != s_wifi_selection_index) return;
        app_wifi_scan_record_t record;
        if (!selected_wifi_record(&record)) {
            s_message = "This network is unavailable or needs another scan.";
            return;
        }
        app_wifi_selection_t reserved;
        if (app_wifi_select_scan_record(s_wifi_selection_generation, s_wifi_selection_index, &reserved) != ESP_OK) {
            s_message = "Network selection expired; scan again.";
            return;
        }
        open_local_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, intent->x != 0);
        if (s_local_editor.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE) {
            s_wifi_reserved = reserved;
            uint64_t remaining = reserved.remaining_ms < APP_UI_LOCAL_EDIT_TIMEOUT_MS ?
                reserved.remaining_ms : APP_UI_LOCAL_EDIT_TIMEOUT_MS;
            s_editor_deadline_ms = s_now_ms > UINT64_MAX - remaining ? UINT64_MAX : s_now_ms + remaining;
        } else (void)app_wifi_release_selection(reserved.id);
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
        /* Observe the whole raw path even if release has already arrived at
         * the callback boundary. Dropping an out-and-back path could turn a
         * swipe into a tap. Host movement still requires a live contact. */
        bool allow_motion = process_touch_gesture(intent);
        portENTER_CRITICAL(&s_control_lock);
        bool touch_down = s_touch_down;
        uint32_t live_gesture = s_live_touch_gesture;
        portEXIT_CRITICAL(&s_control_lock);
        if (!touch_down || intent->gesture != live_gesture || intent->gesture != s_touch_gesture ||
            intent->surface != APP_SURFACE_TOUCHPAD) {
            if (interaction_touch_holding(&s_interaction)) cancel_input();
            return;
        }
        if (!allow_motion) return;
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
        (void)select_keyboard_submode((app_keyboard_submode_t)intent->x);
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
        s_message = "Use the current spoken-letter controls.";
        return;
    case APP_AUDIO_POWER: case APP_AUDIO_MUTE: case APP_AUDIO_VOLUME:
        s_message = "Use the current audio route controls.";
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
        if (s_overlay == APP_OVERLAY_COMMON_KEYS) {
            if (s_engine.state.mode != INPUT_KEYBOARD || intent->y != 0) return;
            switch (intent->x) {
            case 0x28: case 0x29: case 0x2a: case 0x2b: /* Enter, Esc, Backspace, Tab. */
            case 0x4a: case 0x4c: case 0x4d: /* Home, forward Delete, End. */
            case 0x4f: case 0x50: case 0x51: case 0x52: break; /* Arrows. */
            default: return;
            }
        }
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
    cancel_local_editor();
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
            app_media_runtime_stop(AE_STOP_PRIVACY);
            ble_hid_pairing_close();
            if (!setup_was_visible) (void)app_wifi_stop_setup();
            s_pair_operation = APP_CONFIRM_NONE;
            s_confirmation = APP_CONFIRM_NONE;
        }
    } else {
        hid_engine_cancel(&s_engine);
        s_message = "Pending input cancelled; releasing keys and buttons.";
    }
    if (!s_applying_remote_safety) remote_local_override(s_engine.state.locked);
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
        bool remote_owned = s_engine.state.owner == INPUT_REMOTE;
        remote_local_override(false);
        s_button_context_revision = s_ui_revision;
        s_button_context_epoch = s_engine.state.epoch;
        if (remote_owned) {
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
    (void)activate_primary(INPUT_LOCAL);
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
    /* The board adapter supplies the right-handed display frame for both
     * sensors. Host inversion belongs after gravity projection, never here. */
    interaction_imu_sample_t mapped = {
        .timestamp_us = sample.timestamp_us,
        .gx_dps = sample.x_dps, .gy_dps = sample.y_dps, .gz_dps = sample.z_dps,
        .ax_g = sample.x_g, .ay_g = sample.y_g, .az_g = sample.z_g
    };
    bool was_calibrated = s_interaction.view.calibrated;
    /* The remote adapter consumes the same physical sample and model. It
     * chooses an authorized source; it never starts another sensor reader. */
    bool consumed = app_remote_input_motion_sample(s_remote_input, &mapped, s_now_ms);
    input_delta_t delta = consumed ? (input_delta_t){0} : interaction_motion_sample(&s_interaction, &mapped);
    if (!was_calibrated && s_interaction.view.calibrated) {
        s_message = "Motion calibrated. Click AI to toggle motion.";
    }
    if (delta.x || delta.y) {
        (void)hid_engine_enqueue_mouse_move(&s_engine, s_engine.state.epoch,
            INPUT_LOCAL, delta.x, delta.y);
    }
}

static app_wifi_row_view_t wifi_row(const app_wifi_scan_record_t *record, uint16_t index)
{
    app_wifi_row_view_t row = {.record_index = index, .rssi_valid = true,
        .rssi_dbm = record->rssi_dbm, .current = record->current, .remembered = record->remembered};
    (void)snprintf(row.label, sizeof(row.label), "%s", record->label);
    switch (record->security) {
    case APP_WIFI_SECURITY_OPEN: row.security = APP_UI_SECURITY_OPEN; break;
    case APP_WIFI_SECURITY_WEP: row.security = APP_UI_SECURITY_WEP; break;
    case APP_WIFI_SECURITY_WPA: row.security = APP_UI_SECURITY_WPA; break;
    case APP_WIFI_SECURITY_WPA2: row.security = APP_UI_SECURITY_WPA2; break;
    case APP_WIFI_SECURITY_WPA_WPA2: row.security = APP_UI_SECURITY_WPA_WPA2; break;
    case APP_WIFI_SECURITY_WPA3: row.security = APP_UI_SECURITY_WPA3; break;
    case APP_WIFI_SECURITY_WPA2_WPA3: row.security = APP_UI_SECURITY_WPA2_WPA3; break;
    case APP_WIFI_SECURITY_ENTERPRISE: row.security = APP_UI_SECURITY_ENTERPRISE; break;
    case APP_WIFI_SECURITY_OWE: row.security = APP_UI_SECURITY_OWE; break;
    default: row.security = APP_UI_SECURITY_OTHER; break;
    }
    switch (record->availability) {
    case APP_WIFI_JOIN_AVAILABLE: row.availability = APP_UI_JOIN_AVAILABLE; break;
    case APP_WIFI_JOIN_HIDDEN_SSID: row.availability = APP_UI_JOIN_HIDDEN_SSID; break;
    case APP_WIFI_JOIN_UNSUPPORTED_SSID: row.availability = APP_UI_JOIN_UNSUPPORTED_SSID; break;
    case APP_WIFI_JOIN_UNSUPPORTED_SECURITY: row.availability = APP_UI_JOIN_UNSUPPORTED_SECURITY; break;
    default: row.availability = APP_UI_JOIN_UNAVAILABLE; break;
    }
    return row;
}

static void fill_local_management_view(app_view_t *view, const app_wifi_snapshot_t *wifi,
                                      const ble_hid_snapshot_t *link)
{
    app_wifi_scan_snapshot_t scan;
    app_wifi_get_scan_snapshot(&scan);
    switch (scan.state) {
    case APP_WIFI_SCAN_IDLE: view->wifi_scan.state = APP_UI_SCAN_IDLE; break;
    case APP_WIFI_SCAN_QUEUED: view->wifi_scan.state = APP_UI_SCAN_QUEUED; break;
    case APP_WIFI_SCAN_RUNNING: view->wifi_scan.state = APP_UI_SCAN_RUNNING; break;
    case APP_WIFI_SCAN_CANCELLING: view->wifi_scan.state = APP_UI_SCAN_CANCELLING; break;
    case APP_WIFI_SCAN_READY: view->wifi_scan.state = APP_UI_SCAN_READY; break;
    case APP_WIFI_SCAN_CANCELLED: view->wifi_scan.state = APP_UI_SCAN_CANCELLED; break;
    case APP_WIFI_SCAN_ERROR: view->wifi_scan.state = APP_UI_SCAN_ERROR; break;
    case APP_WIFI_SCAN_EXPIRED: view->wifi_scan.state = APP_UI_SCAN_EXPIRED; break;
    default: view->wifi_scan.state = APP_UI_SCAN_UNAVAILABLE; break;
    }
    if (!wifi->initialized) view->wifi_scan.state = APP_UI_SCAN_UNAVAILABLE;
    view->wifi_scan.generation = scan.generation;
    view->wifi_scan.capacity_reached = scan.capacity_reached;
    view->wifi_scan.cleanup_pending = scan.cleanup_pending;
    view->wifi_scan.seconds_left = (scan.remaining_ms + 999U) / 1000U;
    view->wifi_scan.retry_seconds = (scan.retry_in_ms + 999U) / 1000U;
    bool radio_idle = wifi->initialized && !wifi->operation_pending && !wifi->setup_active &&
        !wifi->cleanup_pending && !scan.cleanup_pending && wifi->state != APP_WIFI_CONNECTING &&
        wifi->state != APP_WIFI_OBTAINING_IP;
    view->wifi_scan.can_start = radio_idle && scan.retry_in_ms == 0 &&
        scan.state != APP_WIFI_SCAN_QUEUED && scan.state != APP_WIFI_SCAN_RUNNING &&
        scan.state != APP_WIFI_SCAN_CANCELLING;
    view->wifi_scan.can_cancel = scan.state == APP_WIFI_SCAN_QUEUED || scan.state == APP_WIFI_SCAN_RUNNING;
    if (scan.error != ESP_OK || scan.cleanup_error != ESP_OK)
        (void)snprintf(view->wifi_scan.message, sizeof(view->wifi_scan.message), "Scan: %s",
            esp_err_to_name(scan.cleanup_error != ESP_OK ? scan.cleanup_error : scan.error));
    if (scan.state == APP_WIFI_SCAN_READY && scan.count <= APP_WIFI_SCAN_CAPACITY) {
        view->wifi_scan.count = scan.count;
        unsigned pages = scan.count ? (scan.count + APP_UI_WIFI_ROWS_PER_PAGE - 1U) /
            APP_UI_WIFI_ROWS_PER_PAGE : 1U;
        if (s_wifi_page >= pages) s_wifi_page = pages - 1U;
        view->wifi_scan.page = s_wifi_page;
        for (unsigned i = 0; i < APP_UI_WIFI_ROWS_PER_PAGE; ++i) {
            uint16_t index = (uint16_t)(s_wifi_page * APP_UI_WIFI_ROWS_PER_PAGE + i);
            if (index >= scan.count) break;
            app_wifi_scan_record_t record;
            if (app_wifi_get_scan_record(scan.generation, index, &record) != ESP_OK) {
                view->wifi_scan.row_count = 0;
                view->wifi_scan.count = 0;
                view->wifi_scan.state = APP_UI_SCAN_EXPIRED;
                break;
            }
            view->wifi_scan.rows[view->wifi_scan.row_count++] = wifi_row(&record, index);
        }
    }
    app_wifi_scan_record_t selected;
    bool pinned = s_overlay == APP_OVERLAY_LOCAL_EDITOR &&
        s_local_editor.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE && s_wifi_reserved.id &&
        s_now_ms < s_editor_deadline_ms;
    bool selected_valid = pinned;
    if (pinned) selected = s_wifi_reserved.record;
    else selected_valid = s_wifi_selection_generation && app_wifi_get_scan_record(s_wifi_selection_generation,
        s_wifi_selection_index, &selected) == ESP_OK;
    if (selected_valid) {
        view->wifi_selection.valid = true;
        view->wifi_selection.generation = s_wifi_selection_generation;
        view->wifi_selection.network = wifi_row(&selected, s_wifi_selection_index);
        view->wifi_selection.can_join_once = radio_idle && selected.availability == APP_WIFI_JOIN_AVAILABLE;
        view->wifi_selection.can_remember = view->wifi_selection.can_join_once;
    }
    view->wifi_stored_known = wifi->stored_data_known;
    view->wifi_stored_present = wifi->stored_data_present;
    view->wifi_storage_uncertain = wifi->storage_uncertain;
    view->wifi_stored_valid = wifi->stored_data_known && wifi->stored_data_present &&
        !wifi->storage_uncertain && wifi->stored_ssid[0];
    if (view->wifi_stored_valid)
        (void)snprintf(view->wifi_stored_label, sizeof(view->wifi_stored_label), "%s", wifi->stored_ssid);
    view->wifi_lan_ready = wifi->lan_ready;
    view->wifi_lan_service = APP_UI_LAN_SERVICE_UNAVAILABLE;
    (void)snprintf(view->wifi_lan_service_message, sizeof(view->wifi_lan_service_message),
                   "LAN service state is not available.");
    ble_hid_name_snapshot_t name = ble_hid_name_snapshot();
    view->bluetooth_name.pending = name.status == BLE_HID_NAME_PENDING || name.status == BLE_HID_NAME_APPLYING;
    view->bluetooth_name.uncertain = name.status == BLE_HID_NAME_UNCERTAIN;
    view->bluetooth_name.available = link->initialized && !link->failed &&
        !view->bluetooth_name.pending && !view->bluetooth_name.uncertain && !s_name_should_persist;
    view->bluetooth_name.configured_valid = true;
    view->bluetooth_name.effective_valid = name.active_known;
    view->bluetooth_name.save_state = view->settings_save_state;
    if (view->settings_save_state == APP_SAVE_VOLATILE &&
        !strcmp(s_settings.bluetooth_name, s_durable_settings.bluetooth_name)) {
        settings_store_status_t stored = settings_store_status();
        view->bluetooth_name.save_state = stored.loaded || stored.saved_revision ? APP_SAVE_SAVED : APP_SAVE_DEFAULTS;
    }
    (void)snprintf(view->bluetooth_name.configured, sizeof(view->bluetooth_name.configured), "%s", s_settings.bluetooth_name);
    (void)snprintf(view->bluetooth_name.requested, sizeof(view->bluetooth_name.requested), "%s", name.requested);
    if (name.active_known)
        (void)snprintf(view->bluetooth_name.effective, sizeof(view->bluetooth_name.effective), "%s", name.active);
    if (name.error != ESP_OK)
        (void)snprintf(view->bluetooth_name.message, sizeof(view->bluetooth_name.message), "Name: %s", esp_err_to_name(name.error));
    view->tap_to_click_available = true;
    view->tap_to_click_enabled = s_settings.tap_to_click;
    const uint32_t motion_status = s_interaction.view.motion_status;
    view->two_finger_scroll_available = s_multitouch_available;
    view->swipe_scroll_available = s_multitouch_available;
    view->motion_settings = (app_motion_settings_view_t){
        .available = true, .editable = s_settings_revision != UINT64_MAX,
        .revision = s_settings_revision,
        .gain_x_centi = s_settings.motion_gain_x_centi,
        .gain_y_centi = s_settings.motion_gain_y_centi,
        .deadzone_mdps = s_settings.motion_deadzone_mdps,
        .filter_ms = s_settings.motion_filter_ms, .freeze_ms = s_settings.motion_freeze_ms,
        .invert_x = s_settings.motion_invert_x, .invert_y = s_settings.motion_invert_y,
        .reference = !board_capabilities().imu ? APP_REFERENCE_UNAVAILABLE :
            (motion_status & INTERACTION_MOTION_GEOMETRY_LIMITED) ? APP_REFERENCE_UNSTABLE :
            (motion_status & INTERACTION_MOTION_GRAVITY_READY) ? APP_REFERENCE_READY : APP_REFERENCE_ACQUIRING,
    };
    if (s_overlay == APP_OVERLAY_LOCAL_EDITOR && !s_engine.state.locked && !s_idle.asleep) {
        view->local_editor = s_local_editor;
        view->local_editor.seconds_left = s_now_ms < s_editor_deadline_ms ?
            (unsigned)((s_editor_deadline_ms - s_now_ms + 999U) / 1000U) : 0;
        portENTER_CRITICAL(&s_control_lock);
        view->local_editor.submit_available = !s_local_submission_pending && s_screen_accepts_input;
        portEXIT_CRITICAL(&s_control_lock);
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
    /* Only the coordinator builds this view. The presenter copies it before
     * return; keep the growing snapshot off the latency-sensitive task stack. */
    static EXT_RAM_BSS_ATTR app_view_t view;
    view = (app_view_t){
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
        .gain_divisor = interaction.touch_divisor,
        .dragging = s_drag_requested || interaction_touch_holding(&s_interaction),
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
    view.motion_reason = !capabilities.imu ? APP_MOTION_SENSOR_UNAVAILABLE :
        interaction.calibrating ? APP_MOTION_CALIBRATING :
        !interaction.calibrated ? APP_MOTION_CALIBRATION_REQUIRED :
        s_transport_switch_pending ? APP_MOTION_TRANSPORT_SWITCHING :
        view.transport_error ? APP_MOTION_TRANSPORT_ERROR :
        view.release_pending ? APP_MOTION_RELEASING :
        !active.initialized ? APP_MOTION_PROVIDER_UNAVAILABLE :
        !active.connected ? APP_MOTION_DISCONNECTED :
        !view.ready ? APP_MOTION_INPUT_NOT_READY : APP_MOTION_READY;
    (void)snprintf(view.transport_message, sizeof(view.transport_message), "%s",
        s_transport_switch_pending ? "Waiting for old-host releases to complete." :
        s_engine.state.mode == INPUT_AUDIO ? "Choose the audio route in audio options." :
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
    case APP_WIFI_OBTAINING_IP: view.wifi_state = APP_UI_WIFI_OBTAINING_IP; break;
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
        !device_settings_equal(&s_settings, &s_durable_settings) ? APP_SAVE_VOLATILE :
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
    fill_local_management_view(&view, &wifi, link);
    app_media_controls_fill(&view);
    app_connectivity_controls_fill(&view);
    view.api.remote_input_allowed = s_remote_allowed;
    view.api.remote_input_generation = s_remote_input && s_command_bridge &&
        s_remote_allowance_generation != UINT64_MAX ? s_remote_allowance_generation : 0;
    remote_device_review_view(&view.remote_review);
    (void)app_presenter_publish(&view);
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

#include "app_update_bindings.inc"
#include "app_remote_device_bindings.inc"
#include "app_remote_input_bindings.inc"

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
    s_intents = xQueueCreateStatic(INTENT_CAPACITY, sizeof(coordinator_event_t),
                                    s_intent_bytes, &s_intent_storage);
    configASSERT(s_intents);
    hid_engine_init(&s_engine);
    interaction_init(&s_interaction);
    interaction_multitouch_init(&s_multitouch);
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
    present_boot(APP_BOOT_UI_STARTING, "Loading preferences", "Microphone is off");
    esp_err_t storage_error = nvs_flash_init();
    if (storage_error == ESP_OK) storage_error = settings_store_init(&s_settings);
    if (storage_error != ESP_OK) {
        ESP_LOGW(TAG, "Settings storage unavailable: %s; existing data preserved", esp_err_to_name(storage_error));
        s_message = "Using device settings with a storage error; data preserved.";
    }
    s_durable_settings = s_settings;
    s_dark_theme = s_settings.theme == DEVICE_THEME_DARK;
    s_host_os = s_settings.host_modifier == DEVICE_HOST_COMMAND ? APP_HOST_COMMAND : APP_HOST_CTRL;
    interaction_touch_config_t touch_config = s_interaction.touch_config;
    touch_config.tap_to_click = s_settings.tap_to_click;
    (void)interaction_touch_configure(&s_interaction, &touch_config);
    (void)configure_motion_preferences(&s_settings);
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
    present_boot(APP_BOOT_UI_STARTING, "Starting Bluetooth", "Preparing local input");
    (void)ble_hid_name_request(s_settings.bluetooth_name,
        strlen(s_settings.bluetooth_name), NULL);
    error = ble_hid_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(error));
        s_message = "Bluetooth initialization failed. See device diagnostics.";
    }
    record_boot_memory("bluetooth");
    present_boot(APP_BOOT_UI_STARTING, "Starting Wi-Fi", "Local input works without a network");
    esp_err_t wifi_error = app_wifi_init();
    if (wifi_error != ESP_OK)
        ESP_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_error));
    record_boot_memory("wifi");
    present_boot(APP_BOOT_UI_STARTING, "Preparing audio", "Microphone remains off");
    app_recording_store_config_t recording_config = {.wall_time = recording_wall_time};
    ae_result_t recording_error = app_recording_store_init(&recording_config);
    if (recording_error != AE_OK)
        ESP_LOGW(TAG, "Recording store unavailable: %d", (int)recording_error);
    app_media_services_t media_services = {.store = app_recording_store_port(),
        .wall_time = recording_wall_time};
    ae_result_t media_error = app_media_runtime_init(&media_services);
    if (media_error != AE_OK)
        ESP_LOGW(TAG, "Media runtime unavailable: %d", (int)media_error);
    if (!app_media_controls_init()) ESP_LOGW(TAG, "Media controls unavailable");
    if (!board_display_lock(1000)) {
        report_boot_fault("display lock", ESP_ERR_TIMEOUT);
        return;
    }
    /* One atomic handoff, without a minimum logo duration or blank frame. */
    app_boot_ui_destroy();
    app_ui_create(board_display_get(), submit_intent);
    s_multitouch_available = board_touch_observe(submit_contacts) == ESP_OK;
    app_ui_set_local_edit_submit(submit_local_edit);
    esp_err_t presenter_error = app_presenter_init();
    esp_err_t indicator_error = app_presenter_bind_display(board_display_get());
    board_display_unlock();
    if (indicator_error != ESP_OK)
        ESP_LOGW(TAG, "Capture indicator unavailable: %s", esp_err_to_name(indicator_error));
    if (presenter_error != ESP_OK) {
        report_boot_fault("UI presentation", presenter_error);
        return;
    }
    record_boot_memory("ui");
    remote_control_init(usb_error == ESP_OK);
    if (!app_connectivity_controls_init()) ESP_LOGW(TAG, "Connectivity controls unavailable");
    input_link_t previous = {0};
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    int64_t last_health_us = esp_timer_get_time();
    hid_engine_step_result_t previous_step = HID_ENGINE_STEP_IDLE;
    unsigned ui_tick = 0;
    for (;;) {
        s_now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        remote_update_service();
        process_buttons();
        ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
        service_usb_selection();
        input_link_t link = input_link_snapshot(&bluetooth);
        observe_input_link(&link, &previous);
        sync_interaction();
        (void)process_priority();
        coordinator_event_t event;
        for (unsigned i = 0; i < 4 && xQueueReceive(s_intents, &event, 0) == pdTRUE; ++i) {
            if (event.contacts) handle_contacts(&event);
            else handle_intent(&event.value.intent, &bluetooth);
            if (process_priority()) {
                break;
            }
        }
        remote_control_service();
        service_local_editor();
        service_bluetooth_name();
        service_multitouch();
        (void)apply_touch_result(interaction_touch_tick(&s_interaction, s_now_ms, s_ui_revision));
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
        app_remote_input_service(s_remote_input, s_now_ms);
        update_send(false);
        service_pair_operation(&bluetooth);
        service_transport_switch();
        service_display();
        service_media();
        service_connectivity();
        if (s_engine.state.release_pending) s_drag_requested = false;
        if (++ui_tick >= 10) {
            ui_tick = 0;
            render_view(&bluetooth, error);
        }
        int64_t now = esp_timer_get_time();
        if (now - last_health_us >= 10000000) {
            last_health_us = now;
            report_memory_diagnostics();
            board_imu_status_t imu = board_imu_status();
            app_presenter_status_t presentation = app_presenter_status();
            ESP_LOGI(TAG, "Motion imu_available=%d imu_error=%s samples=%llu "
                     "sample_age_known=%d sample_age_us=%llu calibrated=%d calibrating=%d "
                     "calibration_samples=%u input_kind=%d input_connected=%d input_ready=%d",
                     imu.available, esp_err_to_name(imu.error),
                     (unsigned long long)imu.sample_count, imu.sample_age_known,
                     (unsigned long long)imu.sample_age_us, s_interaction.view.calibrated,
                     s_interaction.view.calibrating, (unsigned)s_interaction.view.calibration_samples,
                     (int)link.kind, link.connected, s_engine.ready);
            if (imu.fault_latched)
                ESP_LOGW(TAG, "First IMU fault stage=%s error=%s uptime_us=%llu "
                         "last_hardware_stage=%s last_hardware_error=%s ready_known=%d ready_bits=%u",
                         imu.first_fault_stage, esp_err_to_name(imu.first_fault_error),
                         (unsigned long long)imu.first_fault_us, imu.last_hardware_stage,
                         esp_err_to_name(imu.last_hardware_error), imu.data_ready_known,
                         (unsigned)imu.data_ready_bits);
            ESP_LOGI(TAG, "UI published=%llu applied=%llu pending=%d pending_age_ms=%llu "
                     "applying=%d last_apply_latency_ms=%llu",
                     (unsigned long long)presentation.published_count,
                     (unsigned long long)presentation.applied_count, presentation.pending,
                     (unsigned long long)presentation.pending_age_ms, presentation.applying,
                     (unsigned long long)presentation.last_apply_latency_ms);
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
