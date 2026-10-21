// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Include production helpers unchanged, preserving their actual static state.
 * Hardware interfaces are substituted only through the included public headers. */
#include "../../firmware/main/main.c"

typedef struct {
    app_input_transport_t transport;
    uint8_t id;
    size_t length;
    uint8_t bytes[8];
} report_t;

static report_t trace[1024];
static size_t trace_count;
static ble_hid_snapshot_t test_link;
static bool test_haptics_enabled;
static bool test_haptic_active;
static bool test_send_failure;
static board_capabilities_t test_capabilities;
static board_imu_sample_t test_sample;
static esp_err_t test_imu_result;
static bool test_display_available, test_display_locked;
static app_view_t test_view;
static char test_target[40];
static size_t test_render_count;
static const char *test_case;
static board_power_status_t test_power;
static esp_err_t test_power_error;
static board_brightness_status_t test_brightness;
static esp_err_t test_brightness_error;
static int test_brightness_requests[32];
static size_t test_brightness_count;
static settings_store_status_t test_storage;
static device_settings_t test_saved_request;
static esp_err_t test_storage_error;
static unsigned test_storage_requests;
static app_wifi_snapshot_t test_wifi;
static app_wifi_setup_info_t test_wifi_info;
static esp_err_t test_wifi_start_error, test_wifi_stop_error, test_wifi_forget_error;
static esp_err_t test_wifi_info_error;
static unsigned test_wifi_starts, test_wifi_stops, test_wifi_forgets, test_wifi_info_reads;
static bool test_pair_accept, test_disconnect_accept;
static unsigned test_pair_opens, test_pair_closes, test_pair_forgets, test_disconnects;
static uint64_t test_disconnect_generation, test_forget_generation;
static size_t test_disconnect_trace_count;
static ble_hid_bond_list_t test_bonds;
static usb_hid_snapshot_t test_usb;
static bool test_usb_send_failure, test_usb_recover_accept;
static unsigned test_usb_recovers, test_ble_send_attempts, test_usb_send_attempts;
static bool test_usb_enable_requests[32];
static size_t test_usb_enable_trace[32], test_usb_enable_count;
static input_link_t test_previous_link;
static void (*test_ble_after_accept)(void), (*test_usb_after_accept)(void);

usb_hid_snapshot_t usb_hid_snapshot(void) { return test_usb; }
void usb_hid_set_enabled(bool enabled)
{
    assert(test_usb_enable_count < 32);
    test_usb_enable_requests[test_usb_enable_count] = enabled;
    test_usb_enable_trace[test_usb_enable_count++] = trace_count;
    /* Documented adapter-boundary response: selection invalidates its session.
     * Endpoint readiness still requires an independent stack acknowledgement. */
    if (test_usb.enabled != enabled) {
        ++test_usb.generation;
        test_usb.ready = false;
    }
    test_usb.enabled = enabled;
    if (!enabled) test_usb.ready = false;
    /* Enabling alone never supplies a mounted/ready/completed acknowledgement. */
}
bool usb_hid_send_generation(uint64_t generation, uint8_t id,
                             const uint8_t *data, size_t length)
{
    ++test_usb_send_attempts;
    if (test_usb_send_failure || !test_usb.enabled || !test_usb.mounted || !test_usb.ready ||
        test_usb.failed || test_usb.suspended || test_usb.congested || generation != test_usb.generation ||
        !((id == 1 && length == 8) || (id == 2 && length == 4))) return false;
    assert(data && trace_count < sizeof trace / sizeof trace[0]);
    trace[trace_count] = (report_t){.transport = APP_INPUT_USB, .id = id, .length = length};
    memcpy(trace[trace_count].bytes, data, length);
    ++trace_count;
    if (test_usb_after_accept) {
        void (*hook)(void) = test_usb_after_accept;
        test_usb_after_accept = NULL;
        hook();
    }
    return true;
}
bool usb_hid_recover_generation(uint64_t generation)
{
    ++test_usb_recovers;
    return test_usb_recover_accept && generation == test_usb.generation;
}

esp_err_t board_power_read(board_power_status_t *status)
{
    assert(status);
    *status = test_power;
    return test_power_error;
}
esp_err_t board_brightness_set(int percent)
{
    assert(percent >= 0 && percent <= 100 && test_brightness_count < 32);
    test_brightness_requests[test_brightness_count++] = percent;
    /* Admission is deliberately not driver acknowledgement. */
    return test_brightness_error;
}
board_brightness_status_t board_brightness_status(void) { return test_brightness; }
esp_err_t settings_store_request(const device_settings_t *settings)
{
    assert(device_settings_validate(settings));
    ++test_storage_requests;
    test_saved_request = *settings;
    return test_storage_error;
}
settings_store_status_t settings_store_status(void) { return test_storage; }
void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot)
{
    assert(snapshot);
    *snapshot = test_wifi;
}
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info)
{
    assert(info);
    ++test_wifi_info_reads;
    if (test_wifi_info_error == ESP_OK) *info = test_wifi_info;
    return test_wifi_info_error;
}
esp_err_t app_wifi_start_setup(void) { ++test_wifi_starts; return test_wifi_start_error; }
esp_err_t app_wifi_stop_setup(void) { ++test_wifi_stops; return test_wifi_stop_error; }
esp_err_t app_wifi_forget_network(void) { ++test_wifi_forgets; return test_wifi_forget_error; }
bool ble_hid_pairing_open(uint32_t duration_ms)
{
    assert(duration_ms == BLE_HID_PAIRING_DEFAULT_MS);
    ++test_pair_opens;
    return test_pair_accept;
}
void ble_hid_pairing_close(void) { ++test_pair_closes; }
bool ble_hid_forget_bonds(uint64_t generation)
{
    ++test_pair_forgets;
    test_forget_generation = generation;
    return test_pair_accept;
}
ble_hid_bond_list_t ble_hid_bond_list(void) { return test_bonds; }

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                uint8_t *bytes, StaticQueue_t *storage)
{
    assert(capacity > 0 && item_size > 0 && bytes && storage);
    *storage = (StaticQueue_t){
        .bytes = bytes, .capacity = capacity, .item_size = item_size
    };
    return storage;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    assert(queue && item && timeout == 0);
    if (queue->count == queue->capacity) return pdFALSE;
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->bytes + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    assert(queue && item && timeout == 0);
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->bytes + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue);
    queue->head = 0;
    queue->count = 0;
    return pdPASS;
}

bool ble_hid_send_generation(uint64_t generation, uint8_t id,
                             const uint8_t *data, size_t length)
{
    ++test_ble_send_attempts;
    if (test_send_failure || generation != test_link.generation || !test_link.connected ||
        !test_link.ready || test_link.congested ||
        !((id == 1 && length == 8) || (id == 2 && length == 4))) return false;
    assert(data && trace_count < sizeof trace / sizeof trace[0]);
    trace[trace_count] = (report_t){.id = id, .length = length};
    memcpy(trace[trace_count].bytes, data, length);
    ++trace_count;
    if (test_ble_after_accept) {
        void (*hook)(void) = test_ble_after_accept;
        test_ble_after_accept = NULL;
        hook();
    }
    return true;
}

void board_haptic_feedback(board_feedback_t event)
{
    assert(event >= BOARD_FEEDBACK_CONFIRM && event <= BOARD_FEEDBACK_ERROR);
}

void board_haptics_enable(bool enabled) { test_haptics_enabled = enabled; }
bool board_haptics_enabled(void) { return test_haptics_enabled; }
bool board_haptics_active(void) { return test_haptic_active; }
esp_err_t board_imu_read(board_imu_sample_t *sample)
{
    if (test_imu_result == ESP_OK) *sample = test_sample;
    return test_imu_result;
}

void coordinator_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static _Noreturn void unexpected_boot_boundary(void)
{
    fputs("Unexpected boot/hardware operation in coordinator helper test\n", stderr);
    abort();
}

/* app_main is compiled, but not run. Boot-only boundaries must fail loudly
 * rather than supply success-shaped implementations for untested operations. */
esp_err_t usb_console_start(void) { unexpected_boot_boundary(); }
esp_err_t usb_hid_init(void) { unexpected_boot_boundary(); }
esp_err_t settings_store_init(device_settings_t *settings)
{
    (void)settings;
    unexpected_boot_boundary();
}
esp_err_t app_wifi_init(void) { unexpected_boot_boundary(); }
esp_err_t nvs_flash_init(void) { unexpected_boot_boundary(); }
const esp_app_desc_t *esp_app_get_description(void) { unexpected_boot_boundary(); }
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)mac;
    (void)type;
    unexpected_boot_boundary();
}
esp_err_t board_button_events_register(board_button_event_callback_t callback)
{
    (void)callback;
    unexpected_boot_boundary();
}
esp_err_t board_init(void (*callback)(void))
{
    (void)callback;
    unexpected_boot_boundary();
}
board_capabilities_t board_capabilities(void) { return test_capabilities; }
lv_display_t *board_display_get(void) { unexpected_boot_boundary(); }
bool board_display_lock(uint32_t timeout)
{
    assert(timeout == 0 && !test_display_locked);
    test_display_locked = test_display_available;
    return test_display_locked;
}
void board_display_unlock(void)
{
    assert(test_display_locked);
    test_display_locked = false;
}
void app_ui_create(lv_display_t *display, app_submit_fn submit)
{
    (void)display;
    (void)submit;
    unexpected_boot_boundary();
}
void app_ui_render(const app_view_t *view)
{
    assert(test_display_locked && view);
    test_view = *view;
    assert(strlen(view->target_name) < sizeof test_target);
    strcpy(test_target, view->target_name);
    test_view.target_name = test_target;
    ++test_render_count;
}
esp_err_t ble_hid_init(void) { unexpected_boot_boundary(); }
ble_hid_snapshot_t ble_hid_snapshot(void) { return test_link; }
bool ble_hid_disconnect_generation(uint64_t generation)
{
    ++test_disconnects;
    test_disconnect_generation = generation;
    test_disconnect_trace_count = trace_count;
    if (!test_disconnect_accept || !test_link.connected || generation != test_link.generation)
        return false;
    /* Production admission immediately revokes readiness and advances the
     * public generation; the asynchronous DISCONNECT event arrives later. */
    ++test_link.generation;
    if (!test_link.generation) ++test_link.generation;
    test_link.ready = false;
    return true;
}
const char *esp_err_to_name(esp_err_t error)
{
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_FAIL) return "ESP_FAIL";
    if (error == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
    return "TEST_UNKNOWN_ERROR";
}
int64_t esp_timer_get_time(void) { return (int64_t)(s_now_ms * 1000U); }
esp_err_t esp_task_wdt_add(void *task)
{
    (void)task;
    unexpected_boot_boundary();
}
esp_err_t esp_task_wdt_reset(void) { unexpected_boot_boundary(); }
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    unexpected_boot_boundary();
}
UBaseType_t uxTaskGetStackHighWaterMark(void *task)
{
    (void)task;
    unexpected_boot_boundary();
}
size_t heap_caps_get_free_size(uint32_t capabilities)
{
    (void)capabilities;
    unexpected_boot_boundary();
}
size_t heap_caps_get_minimum_free_size(uint32_t capabilities)
{
    (void)capabilities;
    unexpected_boot_boundary();
}
size_t heap_caps_get_largest_free_block(uint32_t capabilities)
{
    (void)capabilities;
    unexpected_boot_boundary();
}

static hid_engine_step_result_t step(void)
{
    const size_t before = trace_count;
    input_link_t active = input_link_snapshot(&test_link);
    const hid_engine_step_result_t result =
        hid_engine_step(&s_engine, send_report, &active.generation);
    assert(trace_count - before <= 1);
    return result;
}

static void expect_report(uint8_t id, const uint8_t *bytes, size_t length)
{
    const size_t before = trace_count;
    assert(step() == HID_ENGINE_STEP_SENT && trace_count == before + 1);
    const report_t *report = &trace[before];
    assert(report->id == id && report->length == length);
    if (memcmp(report->bytes, bytes, length) != 0) {
        fprintf(stderr, "%s: report %zu mismatch; actual:", test_case, before);
        for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", report->bytes[i]);
        fputs(" expected:", stderr);
        for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", bytes[i]);
        fputc('\n', stderr);
    }
    assert(memcmp(report->bytes, bytes, length) == 0);
}

static void release_barrier(void)
{
    static const uint8_t zero[8] = {0};
    assert(s_engine.state.release_pending);
    expect_report(1, zero, 8);
    assert(s_engine.state.release_pending);
    expect_report(2, zero, 4);
    assert(!s_engine.state.release_pending);
}

static void expect_idle(void)
{
    const size_t before = trace_count;
    hid_engine_step_result_t result = step();
    if (result != HID_ENGINE_STEP_IDLE || trace_count != before)
        fprintf(stderr, "%s: expected idle at report %zu; step=%d reports=%zu\n",
            test_case, before, (int)result, trace_count);
    assert(result == HID_ENGINE_STEP_IDLE && trace_count == before);
}

static void expect_empty_intents(void)
{
    app_intent_t intent;
    assert(xQueueReceive(s_intents, &intent, 0) == pdFALSE);
}

static app_intent_t make_intent(app_action_t action, int x, int y, const char *text)
{
    app_intent_t intent = {
        .action = action, .epoch = s_engine.state.epoch,
        .ui_revision = s_ui_revision, .x = x, .y = y
    };
    if (text) {
        assert(strlen(text) < sizeof intent.text);
        memcpy(intent.text, text, strlen(text) + 1);
    }
    return intent;
}

static void consume_one(void)
{
    app_intent_t intent;
    assert(xQueueReceive(s_intents, &intent, 0) == pdTRUE);
    handle_intent(&intent, &test_link);
}

static void deliver(app_intent_t intent)
{
    assert(submit_intent(&intent));
    consume_one();
}

static void setup(input_mode_t mode)
{
    s_intents = xQueueCreateStatic(INTENT_CAPACITY, sizeof(app_intent_t),
                                   s_intent_bytes, &s_intent_storage);
    s_priority_requests = 0;
    memset(s_touch_emergency, 0, sizeof s_touch_emergency);
    memset(s_touch_emergency_pending, 0, sizeof s_touch_emergency_pending);
    s_overlay = APP_OVERLAY_NONE;
    s_keyboard_submode = APP_KEYBOARD_MOTION;
    s_host_os = APP_HOST_CTRL;
    s_audio_kind = APP_AUDIO_VOICE;
    s_dark_theme = true;
    s_ui_revision = 1;
    s_now_ms = 1000;
    device_settings_defaults(&s_settings);
    device_idle_init(&s_idle, s_now_ms);
    s_settings_page = APP_SETTINGS_HOME;
    s_bluetooth_page = 0;
    s_input_transport = s_requested_transport = APP_INPUT_BLUETOOTH;
    s_transport_switch_pending = false;
    s_transport_switch_deadline_ms = 0;
    s_transport_origin_generation = 0;
    s_confirmation = APP_CONFIRM_NONE;
    s_confirmation_generation = 0;
    memset(s_confirmation_ssid, 0, sizeof s_confirmation_ssid);
    memset(s_confirmation_stored_ssid, 0, sizeof s_confirmation_stored_ssid);
    s_confirmation_storage_known = s_confirmation_storage_present = false;
    s_confirmation_storage_uncertain = false;
    s_pair_operation = APP_CONFIRM_NONE;
    s_pair_deadline_ms = 0;
    s_pair_origin_generation = 0;
    s_pair_disconnect_requested = false;
    s_screen_accepts_input = true;
    s_brightness_requested = -1;
    s_touch_token = s_ai_token = 0;
    s_idle_touch_gesture = 0;
    s_user_activity_ms = s_seen_activity_ms = 0;
    memset(s_firmware_version, 0, sizeof s_firmware_version);
    memset(s_build_id, 0, sizeof s_build_id);
    memset(s_device_id, 0, sizeof s_device_id);
    s_send_token = 0;
    s_send_epoch = 0;
    s_send_base = 0;
    s_send_batch = false;
    s_drag_requested = false;
    s_touch_down = false;
    s_physical_down = false;
    s_touch_gesture = 0;
    s_consumed_gesture = 0;
    s_live_touch_gesture = 0;
    s_touch_transition_ms = 0;
    s_last_imu_us = 0;
    s_button_head = 0;
    s_button_count = 0;
    s_button_overflow = false;
    s_button_resync = false;
    s_button_context_revision = 0;
    s_button_context_epoch = 0;
    memset(s_button_edges, 0, sizeof s_button_edges);
    hid_engine_init(&s_engine);
    interaction_init(&s_interaction);
    test_link = (ble_hid_snapshot_t){
        .initialized = true, .connected = true, .ready = true,
        .encrypted = true, .generation = 17
    };
    test_haptics_enabled = true;
    test_haptic_active = false;
    test_send_failure = false;
    test_capabilities = (board_capabilities_t){
        .name = "Offline board boundary", .display = true, .touch = true,
        .lock_button = true, .imu = true, .haptics = true,
        .brightness = true, .battery = true
    };
    test_power = (board_power_status_t){0};
    test_power_error = ESP_FAIL;
    test_brightness = (board_brightness_status_t){.applied_known = true, .applied_percent = 70};
    test_brightness_error = ESP_OK;
    test_brightness_count = 0;
    test_storage = (settings_store_status_t){.available = true};
    test_storage_error = ESP_OK;
    test_storage_requests = 0;
    test_wifi = (app_wifi_snapshot_t){0};
    test_wifi_info = (app_wifi_setup_info_t){0};
    test_wifi_start_error = test_wifi_stop_error = test_wifi_forget_error = ESP_ERR_INVALID_STATE;
    test_wifi_info_error = ESP_ERR_INVALID_STATE;
    test_wifi_starts = test_wifi_stops = test_wifi_forgets = test_wifi_info_reads = 0;
    test_pair_accept = test_disconnect_accept = false;
    test_pair_opens = test_pair_closes = test_pair_forgets = test_disconnects = 0;
    test_disconnect_generation = test_forget_generation = 0;
    test_disconnect_trace_count = 0;
    test_bonds = (ble_hid_bond_list_t){0};
    test_usb = (usb_hid_snapshot_t){0};
    test_usb_send_failure = test_usb_recover_accept = false;
    test_usb_recovers = test_usb_send_attempts = test_ble_send_attempts = 0;
    test_usb_enable_count = 0;
    test_ble_after_accept = test_usb_after_accept = NULL;
    test_sample = (board_imu_sample_t){0};
    test_imu_result = ESP_FAIL;
    test_display_available = true;
    test_display_locked = false;
    test_render_count = 0;
    trace_count = 0;
    hid_engine_set_link(&s_engine, true, true);
    assert(hid_engine_set_mode(&s_engine, mode));
    release_barrier();
    sync_interaction();
    trace_count = 0;
    expect_idle();
    assert(!process_priority());
    test_previous_link = input_link_snapshot(&test_link);
}

static void fill_intents(void)
{
    app_intent_t intent = make_intent(APP_KEY, 5, 0, NULL);
    /* Fill the boundary queue without requesting overflow cancellation, so
     * the explicit emergency request below is solely responsible for safety. */
    while (s_intents->count < s_intents->capacity) assert(submit_intent(&intent));
    assert(xQueueSend(s_intents, &intent, 0) == pdFALSE);
}

static void press(input_mode_t mode)
{
    if (mode == INPUT_KEYBOARD) {
        const uint8_t key[] = {2, 0, 4, 0, 0, 0, 0, 0};
        deliver(make_intent(APP_KEY, 4, 2, NULL));
        expect_report(1, key, sizeof key);
    } else {
        const uint8_t click[] = {1, 0, 0, 0};
        deliver(make_intent(APP_CLICK, 1, 0, NULL));
        expect_report(2, click, sizeof click);
    }
}

static void saturated_emergency_admission(void)
{
    const app_action_t emergencies[] = {APP_LOCK, APP_RELEASE};
    for (size_t i = 0; i < sizeof emergencies / sizeof emergencies[0]; ++i) {
        setup(INPUT_KEYBOARD);
        press(INPUT_KEYBOARD);
        deliver(make_intent(APP_KEY, 5, 0, NULL));
        fill_intents();
        app_intent_t emergency = make_intent(emergencies[i], 0, 0, NULL);
        emergency.epoch = 0; /* Emergency stop remains effective from an old UI. */
        assert(submit_intent(&emergency));
        assert(process_priority());
        assert(s_engine.state.locked == (emergencies[i] == APP_LOCK));
        expect_empty_intents();
        release_barrier();
        expect_idle();
        assert(trace_count == 3); /* Original press, then two releases only. */
        assert(!process_priority());
    }

    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    fill_intents();
    app_intent_t overflow = make_intent(APP_MOVE, 99, 99, NULL);
    assert(!submit_intent(&overflow));
    assert(process_priority());
    assert(!s_engine.state.locked);
    expect_empty_intents();
    release_barrier();
    expect_idle();
    assert(trace_count == 3); /* Overflow causes release, never queued movement. */
}

static void stale_ui_epoch(void)
{
    setup(INPUT_MOUSE);
    app_intent_t stale_key = make_intent(APP_KEY, 5, 0, NULL);
    app_intent_t stale_mode = make_intent(APP_MODE, INPUT_AUDIO, 0, NULL);
    app_intent_t stale_recover = make_intent(APP_RECOVER, 0, 0, NULL);
    assert(submit_intent(&stale_key));
    assert(submit_intent(&stale_mode));
    assert(submit_intent(&stale_recover));
    const app_intent_t change = make_intent(APP_MODE, INPUT_KEYBOARD, 0, NULL);
    handle_intent(&change, &test_link);
    const uint64_t epoch = s_engine.state.epoch;
    assert(epoch != stale_key.epoch);
    release_barrier();
    consume_one();
    consume_one();
    consume_one();
    assert(s_engine.state.mode == INPUT_KEYBOARD && s_engine.state.epoch == epoch);
    expect_idle();
    const uint8_t key[] = {0, 0, 4, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    deliver(make_intent(APP_KEY, 4, 0, NULL));
    expect_report(1, key, sizeof key);
    expect_report(1, zero, sizeof zero);
    expect_idle();
}

static void touch_cannot_unlock(void)
{
    setup(INPUT_KEYBOARD);
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    const app_action_t actions[] = {
        APP_LOCK, APP_RELEASE, APP_RECOVER, APP_MODE, APP_KEY,
        APP_MOVE, APP_CLICK, APP_SCROLL, APP_TEXT, APP_HAPTICS, (app_action_t)999
    };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
        app_intent_t touch = make_intent(actions[i], 1, 0, "a");
        assert(submit_intent(&touch));
        if (actions[i] == APP_LOCK || actions[i] == APP_RELEASE) {
            assert(process_priority());
        } else {
            consume_one();
        }
        assert(s_engine.state.locked && !s_engine.state.gyro_enabled);
        if (s_engine.state.release_pending) release_barrier();
        expect_idle();
    }
    for (size_t i = 0; i < trace_count; ++i)
        for (size_t byte = 0; byte < trace[i].length; ++byte)
            assert(trace[i].bytes[byte] == 0); /* Touch never emits a press while locked. */
}

static void physical_hold_toggle(void)
{
    setup(INPUT_MOUSE);
    assert(hid_engine_set_gyro(&s_engine, true));
    physical_lock();
    physical_lock(); /* Coalesced holds toggle once, not twice. */
    assert(process_priority() && s_engine.state.locked);
    assert(!s_engine.state.gyro_enabled);
    release_barrier();
    assert(!process_priority() && s_engine.state.locked);
    physical_lock();
    assert(process_priority() && !s_engine.state.locked);
    assert(s_engine.state.mode == INPUT_MOUSE && !s_engine.state.gyro_enabled);
    assert(s_engine.state.owner == INPUT_LOCAL);
    release_barrier();
    expect_idle();
    assert(!process_priority() && !s_engine.state.locked);
}

static void simultaneous_lock_wins(void)
{
    for (unsigned int initially_locked = 0; initially_locked < 2; ++initially_locked) {
        for (unsigned int physical_first = 0; physical_first < 2; ++physical_first) {
            setup(INPUT_MOUSE);
            if (initially_locked) {
                physical_lock();
                assert(process_priority());
                release_barrier();
            }
            app_intent_t touch_lock = make_intent(APP_LOCK, 0, 0, NULL);
            if (physical_first) physical_lock();
            assert(submit_intent(&touch_lock));
            if (!physical_first) physical_lock();
            assert(process_priority());
            assert(s_engine.state.locked);
            if (s_engine.state.release_pending) release_barrier();
            expect_idle();
            assert(!process_priority() && s_engine.state.locked);
            physical_lock();
            assert(process_priority() && !s_engine.state.locked);
            release_barrier();
        }
    }
}

static void lock_releases_before_later_work(void)
{
    const input_mode_t modes[] = {INPUT_MOUSE, INPUT_KEYBOARD};
    static const uint8_t zero[8] = {0};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        const input_mode_t mode = modes[i];
        setup(mode);
        press(mode);
        app_intent_t old = make_intent(mode == INPUT_MOUSE ? APP_CLICK : APP_KEY, 5, 0, NULL);
        deliver(old); /* Already admitted behind the currently held press. */
        app_intent_t waiting = make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 7, 3, NULL);
        assert(submit_intent(&waiting)); /* A separate UI action is also pending. */
        app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
        assert(submit_intent(&lock) && process_priority());
        expect_empty_intents();
        assert(s_engine.state.locked);
        release_barrier();
        expect_idle();
        assert(trace_count == 3);
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        expect_idle();
        physical_lock();
        assert(process_priority() && !s_engine.state.locked);
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        release_barrier(); /* Fresh requests before the barrier cannot bypass it. */
        expect_idle();
        deliver(old); /* Old accepted work is not revived by unlock. */
        expect_idle();
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        if (mode == INPUT_MOUSE) {
            const uint8_t move[] = {0, 6, 0, 0};
            expect_report(2, move, sizeof move);
        } else {
            const uint8_t key[] = {0, 0, 6, 0, 0, 0, 0, 0};
            expect_report(1, key, sizeof key);
            expect_report(1, zero, sizeof zero);
        }
        expect_idle();
    }
}

static void action(app_action_t kind, int x)
{
    deliver(make_intent(kind, x, 0, NULL));
}

static void touch(app_action_t kind, uint32_t gesture, int dx, int dy)
{
    app_intent_t intent = make_intent(kind, dx, dy, NULL);
    intent.gesture = gesture;
    intent.surface = APP_SURFACE_TOUCHPAD;
    deliver(intent);
}

static void buttons_at(uint64_t now_ms)
{
    s_now_ms = now_ms;
    process_buttons();
    (void)process_priority();
    sync_interaction();
}

static void button_edge(bool down, uint64_t now_ms)
{
    physical_button(down ? BOARD_BUTTON_DOWN : BOARD_BUTTON_UP, now_ms * 1000U);
    buttons_at(now_ms);
}

static void sample_after(uint64_t delay_ms, float x_dps, float y_dps)
{
    s_now_ms += delay_ms;
    test_sample = (board_imu_sample_t){
        .timestamp_us = s_now_ms * 1000U, .x_dps = y_dps, .z_dps = -x_dps, .z_g = 1
    };
    test_imu_result = ESP_OK;
    sync_interaction();
    process_motion();
}

static void calibrate(void)
{
    for (unsigned i = 0; i < 70; ++i) {
        sample_after(10, 0, 0);
        expect_idle();
    }
    assert(s_interaction.view.calibrated && !s_interaction.view.calibrating);
    interaction_config_t config = s_interaction.config;
    config.filter_seconds = 0;
    config.pointer_gain = 10;
    assert(interaction_configure(&s_interaction, &config));
}

static void stale_context_and_modal_focus(void)
{
    setup(INPUT_KEYBOARD);
    app_intent_t stale = make_intent(APP_GRID, 1, 0, NULL);
    action(APP_GRID, 0); /* Change from groups to letters without changing HID epoch. */
    assert(s_interaction.view.group == 0 && s_interaction.view.preview_length == 0);
    assert(stale.epoch == s_engine.state.epoch && stale.ui_revision != s_ui_revision);
    deliver(stale);
    assert(s_interaction.view.preview_length == 0);
    action(APP_GRID, 0);
    assert(strcmp(s_interaction.view.preview, "A") == 0);
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    release_barrier();
    sync_interaction();
    action(APP_GRID, 0);
    action(APP_SPACE, 0);
    action(APP_SEND, 0);
    assert(strcmp(s_interaction.view.preview, "A") == 0 && !s_send_token);
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 300);
    assert(s_overlay == APP_OVERLAY_SETTINGS);
    assert(strcmp(s_interaction.view.preview, "A") == 0);
    expect_idle();
    action(APP_CLOSE, 0);
    assert(s_overlay == APP_OVERLAY_NONE);
}

static void preview_editing_and_full_send(void)
{
    setup(INPUT_KEYBOARD);
    action(APP_GRID, 0);
    action(APP_GRID, 0);
    action(APP_CASE, 0);
    action(APP_GRID, 0);
    action(APP_GRID, 1);
    action(APP_SPACE, 0);
    assert(strcmp(s_interaction.view.preview, "Ab ") == 0);
    action(APP_DELETE, 0);
    assert(strcmp(s_interaction.view.preview, "Ab") == 0);
    action(APP_CLEAR, 0);
    assert(s_interaction.view.preview_length == 0);
    action(APP_CASE, 0);
    for (unsigned i = 0; i < 64; ++i) {
        action(APP_GRID, 0);
        action(APP_GRID, (int)(i % 2));
    }
    assert(s_interaction.view.preview_length == 64);
    action(APP_SPACE, 0);
    assert(s_interaction.view.preview_length == 64);
    expect_idle(); /* Selection/editing has emitted no HID reports. */
    assert(trace_count == 0);
    action(APP_SEND, 0);
    assert(s_send_token && s_engine.text_pending == 32);
    action(APP_CLEAR, 0);
    assert(s_interaction.view.preview_length == 64);
    for (size_t i = 0; i < 128; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
        const report_t *report = &trace[i];
        assert(report->id == 1 && report->length == 8);
        const uint8_t expected[8] = {
            i % 2 == 0 ? 2 : 0, 0,
            i % 2 == 0 ? (uint8_t)(4 + (i / 2) % 2) : 0, 0, 0, 0, 0, 0
        };
        assert(memcmp(report->bytes, expected, sizeof expected) == 0);
        if (i == 63) assert(s_engine.text_pending == 32); /* Second batch admitted. */
    }
    assert(!s_send_token && !s_interaction.view.sending);
    assert(s_interaction.view.preview_length == 0 && s_interaction.view.send_accepted == 64);
    assert(trace_count == 128);
    expect_idle(); /* No implicit Enter or replay. */
}

static void fill_preview(size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        action(APP_GRID, 0);
        action(APP_GRID, (int)(i % 2));
    }
}

static void interrupted_send_retains_only_unaccepted_suffix(void)
{
    const size_t steps[] = {0, 1, 2, 3, 63, 64, 65};
    for (size_t scenario = 0; scenario < sizeof steps / sizeof steps[0]; ++scenario) {
        setup(INPUT_KEYBOARD);
        fill_preview(64);
        action(APP_SEND, 0);
        for (size_t i = 0; i < steps[scenario]; ++i) {
            assert(step() == HID_ENGINE_STEP_SENT);
            update_send(false);
        }
        const size_t accepted = (steps[scenario] + 1) / 2;
        action(APP_STOP, 0);
        assert(!s_send_token && !s_interaction.view.sending);
        assert(s_interaction.view.preview_length == 64 - accepted);
        for (size_t i = 0; i < 64 - accepted; ++i)
            assert(s_interaction.view.preview[i] == ((i + accepted) % 2 ? 'B' : 'A'));
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == steps[scenario] + 2);
    }
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT); /* A accepted; failed key-up cannot restore A. */
    update_send(false);
    test_send_failure = true;
    assert(step() == HID_ENGINE_STEP_SEND_FAILED);
    update_send(false);
    assert(!s_send_token && strcmp(s_interaction.view.preview, "BAB") == 0);
    test_send_failure = false;
    release_barrier();
    sync_interaction();
    expect_idle();
}

static void touch_precision_and_drag_boundaries(void)
{
    for (int divisor = 4; divisor <= 8; divisor *= 2) {
        setup(INPUT_MOUSE);
        action(APP_GAIN, divisor);
        touch(APP_TOUCH_BEGIN, 7, 100, 100);
        for (int i = 1; i <= divisor; ++i) {
            touch(APP_TOUCH_MOVE, 7, 1, -1);
            if (i < divisor) expect_idle();
        }
        const uint8_t delta[] = {0, 1, 0xff, 0};
        expect_report(2, delta, sizeof delta);
        touch(APP_TOUCH_MOVE, 99, 80, 80); /* Wrong gesture must not move. */
        expect_idle();
    }
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        setup(INPUT_MOUSE);
        action(APP_DRAG, 0);
        const uint8_t held[] = {1, 0, 0, 0};
        expect_report(2, held, sizeof held);
        touch(APP_TOUCH_BEGIN, 11, 10, 10);
        touch(APP_TOUCH_MOVE, 11, 3, -2);
        const uint8_t moved[] = {1, 3, 0xfe, 0};
        expect_report(2, moved, sizeof moved);
        touch(APP_TOUCH_END, 11, 13, 8);
        if (boundary == 0) action(APP_MENU, APP_OVERLAY_SETTINGS);
        else if (boundary == 1) action(APP_MODE, INPUT_KEYBOARD);
        else if (boundary == 2) {
            /* Invoke production cancellation and link setters at the real
             * transport boundary; full polling-loop scheduling is separate. */
            cancel_input();
            test_link.connected = false;
            test_link.ready = false;
            ++test_link.generation;
            hid_engine_set_link(&s_engine, false, false);
            sync_interaction();
            assert(step() == HID_ENGINE_STEP_BLOCKED);
            test_link.connected = test_link.ready = true;
            hid_engine_set_link(&s_engine, true, true);
        } else action(APP_DRAG, 0);
        assert(!s_drag_requested && s_engine.state.release_pending);
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == 4);
    }
}

static void raw_button_timing_and_takeover(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 299);
    assert(!s_engine.state.gyro_enabled);
    buttons_at(start + 300);
    assert(s_engine.state.gyro_enabled);
    buttons_at(start + 500);
    assert(s_engine.state.gyro_enabled); /* Exactly one single-click. */

    setup(INPUT_MOUSE);
    calibrate();
    uint64_t time = s_now_ms + 10;
    button_edge(true, time);
    button_edge(false, time + 20);
    button_edge(true, time + 200);
    button_edge(false, time + 220);
    assert(s_overlay == APP_OVERLAY_MODES && !s_engine.state.gyro_enabled);
    release_barrier();
    buttons_at(time + 600);
    assert(!s_engine.state.gyro_enabled && s_overlay == APP_OVERLAY_MODES);

    setup(INPUT_MOUSE);
    time = s_now_ms + 10;
    button_edge(true, time);
    buttons_at(time + 899);
    assert(!s_engine.state.locked);
    buttons_at(time + 900);
    assert(s_engine.state.locked);
    release_barrier();
    buttons_at(time + 2000);
    assert(s_engine.state.locked); /* Holding cannot repeatedly toggle. */
    button_edge(false, time + 2010);
    buttons_at(time + 2300);
    assert(s_engine.state.locked);
    button_edge(true, time + 2500);
    buttons_at(time + 3400);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();

    setup(INPUT_MOUSE);
    calibrate();
    assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
    release_barrier();
    sync_interaction();
    time = s_now_ms + 10;
    button_edge(true, time);
    assert(s_engine.state.owner == INPUT_LOCAL);
    release_barrier();
    button_edge(false, time + 20);
    buttons_at(time + 300);
    assert(!s_engine.state.gyro_enabled && s_overlay == APP_OVERLAY_NONE);
    expect_idle();
}

static void motion_gates_and_local_keyboard_focus(void)
{
    setup(INPUT_MOUSE);
    action(APP_MOTION, 0);
    assert(!s_engine.state.gyro_enabled); /* Uncalibrated. */
    calibrate();
    action(APP_MOTION, 0);
    assert(s_engine.state.gyro_enabled);
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
    process_motion(); /* Repeated board snapshot cannot duplicate movement. */
    expect_idle();
    test_haptic_active = true;
    sample_after(10, 100, 0);
    expect_idle();
    test_haptic_active = false;
    sample_after(10, 100, 0);
    expect_idle();
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 100, 0); expect_idle(); }
    sample_after(10, 0, 0);
    expect_idle(); /* Suppressed movement was discarded, not banked. */
    sample_after(10, 25, 0);
    expect_report(2, moved, sizeof moved);
    sample_after(100, 200, 0);
    expect_idle(); /* Long sampling gap produces no catch-up burst. */
    sample_after(10, 0, 0);
    expect_idle();
    sample_after(10, NAN, 0);
    expect_idle();
    sample_after(10, 25, 0);
    expect_idle(); /* First valid sample reestablishes time. */
    sample_after(10, 25, 0);
    expect_report(2, moved, sizeof moved);
    test_capabilities.imu = false;
    test_imu_result = ESP_FAIL;
    process_motion();
    assert(!s_engine.state.gyro_enabled && !s_interaction.view.motion_enabled);

    setup(INPUT_KEYBOARD);
    calibrate();
    for (unsigned i = 0; i < 20; ++i) { sample_after(10, 50, 0); expect_idle(); }
    assert(s_interaction.view.focus != 0 && s_interaction.view.preview_length == 0);
    assert(trace_count == 0); /* Selection motion never reaches host or confirms. */
    action(APP_MODE, INPUT_AUDIO);
    release_barrier();
    sync_interaction();
    for (unsigned i = 0; i < 20; ++i) { sample_after(10, 50, 0); expect_idle(); }
    assert(!s_engine.state.gyro_enabled && s_interaction.view.preview_length == 0);
}

static void rendered_state_is_truthful(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    render_view(&test_link, ESP_OK);
    assert(test_render_count == 1 && !test_display_locked);
    assert(strcmp(test_view.preview, "AB") == 0 && test_view.microphone_muted);
    assert(!test_view.audio_available && !test_view.audio_active &&
           !test_view.recognition_available && !test_view.listening);
    assert(test_view.keyboard_cell_count == 9 && !test_view.motion_available);
    test_link.peer_known = true;
    const uint8_t peer[] = {2, 3, 4, 5, 6, 7};
    memcpy(test_link.peer_address, peer, sizeof peer);
    render_view(&test_link, ESP_OK);
    assert(strcmp(test_target, "Host 02:03:04:05:06:07") == 0);
    test_link.connected = false;
    test_link.ready = false;
    hid_engine_set_link(&s_engine, false, false);
    sync_interaction();
    render_view(&test_link, ESP_FAIL);
    assert(!test_view.connected && !test_view.ready && test_view.transport_error);
    assert(strcmp(test_target, "No Bluetooth host") == 0);
    test_display_available = false;
    render_view(&test_link, ESP_OK);
    assert(test_render_count == 3 && !test_display_locked);
}

static void touch_lifecycle_rejects_ended_gesture(void)
{
    setup(INPUT_MOUSE);
    action(APP_GAIN, 4);
    touch(APP_TOUCH_BEGIN, 12, 0, 0);
    touch(APP_TOUCH_MOVE, 12, 3, 0);
    expect_idle();
    touch(APP_TOUCH_END, 12, 3, 0);
    touch(APP_TOUCH_MOVE, 12, 1, 0);
    expect_idle(); /* An ended gesture cannot emit during the gyro release freeze. */
    touch(APP_TOUCH_BEGIN, 13, 20, 20);
    touch(APP_TOUCH_MOVE, 13, 1, 0);
    expect_idle(); /* A new finger contact cannot inherit fractional movement. */
}

static void touch_release_has_one_freeze_interval(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    touch(APP_TOUCH_BEGIN, 17, 0, 0);
    sample_after(10, 25, 0);
    expect_idle();
    touch(APP_TOUCH_END, 17, 0, 0);
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved); /* One 150 ms interval, not two stacked freezes. */
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
}

static void physical_down_freezes_before_classification(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    /* Snapshot gate acts immediately, even before queued down is classified. */
    physical_button(BOARD_BUTTON_DOWN, (s_now_ms + 10) * 1000U);
    sample_after(10, 25, 0);
    expect_idle();
    process_buttons();
    button_edge(false, s_now_ms + 10);
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void same_or_invalid_mode_preserves_active_send(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT);
    update_send(false);
    const uint64_t token = s_send_token;
    const uint64_t epoch = s_engine.state.epoch;
    assert(token && strcmp(s_interaction.view.preview, "BAB") == 0);
    action(APP_MODE, INPUT_KEYBOARD);
    action(APP_MODE, 999);
    assert(s_send_token == token && s_engine.state.epoch == epoch);
    assert(s_interaction.view.sending && s_engine.text_pending == 4);
    for (unsigned i = 0; i < 7; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
    }
    assert(!s_send_token && !s_interaction.view.sending);
    assert(s_interaction.view.send_accepted == 4 && s_interaction.view.preview_length == 0);
    assert(trace_count == 8);
    expect_idle();
}

static void short_congestion_preserves_preview_send(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(64);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT);
    update_send(false);
    const uint64_t token = s_send_token;
    test_link.congested = true;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, true));
    sync_interaction();
    update_send(false);
    assert(s_send_token == token && s_interaction.view.sending);
    assert(strcmp(s_interaction.view.preview + 61, "AB") == 0);
    assert(s_interaction.view.preview_length == 63);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    s_now_ms += 20;
    test_link.congested = false;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, false));
    sync_interaction();
    for (unsigned i = 0; i < 127; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
    }
    assert(!s_send_token && s_interaction.view.preview_length == 0);
    assert(s_interaction.view.send_accepted == 64 && trace_count == 128);
    expect_idle();
}

static void short_congestion_preserves_motion_intent(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    test_link.congested = true;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, true));
    sync_interaction();
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
    sample_after(10, 100, 0);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 0);
    test_link.congested = false;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, false));
    sync_interaction();
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void modal_button_press_is_not_retargeted_after_close(void)
{
    setup(INPUT_KEYBOARD);
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    release_barrier();
    sync_interaction();
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 100);
    action(APP_CLOSE, 0);
    assert(s_interaction.view.group == -1);
    buttons_at(start + 300);
    assert(s_interaction.view.group == -1 && s_interaction.view.preview_length == 0);
    expect_idle();
}

static void local_touch_takeover_consumes_its_gesture(void)
{
    setup(INPUT_MOUSE);
    assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
    release_barrier();
    sync_interaction();
    touch(APP_TOUCH_BEGIN, 31, 0, 0);
    assert(s_engine.state.owner == INPUT_LOCAL && s_engine.state.release_pending);
    release_barrier();
    sync_interaction();
    app_intent_t click = make_intent(APP_CLICK, 1, 0, NULL);
    click.gesture = 31;
    deliver(click);
    touch(APP_TOUCH_MOVE, 31, 40, 30);
    expect_idle();
    touch(APP_TOUCH_END, 31, 0, 0);
    touch(APP_TOUCH_BEGIN, 32, 0, 0);
    click = make_intent(APP_CLICK, 1, 0, NULL);
    click.gesture = 32;
    deliver(click);
    const uint8_t down[] = {1, 0, 0, 0}, up[] = {0, 0, 0, 0};
    expect_report(2, down, sizeof down);
    expect_report(2, up, sizeof up);
    expect_idle();
}

static void preview_survives_mode_modal_and_link_cancellation(void)
{
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        setup(INPUT_KEYBOARD);
        fill_preview(4);
        action(APP_SEND, 0);
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        if (boundary == 0) action(APP_MODE, INPUT_MOUSE);
        else if (boundary == 1) action(APP_MENU, APP_OVERLAY_SETTINGS);
        else {
            cancel_input();
            test_link.ready = test_link.connected = false;
            ++test_link.generation;
            hid_engine_set_link(&s_engine, false, false);
            sync_interaction();
            update_send(false);
            assert(step() == HID_ENGINE_STEP_BLOCKED);
            test_link.ready = test_link.connected = true;
            hid_engine_set_link(&s_engine, true, true);
        }
        assert(!s_send_token && !s_interaction.view.sending);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0);
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == 3); /* A down and two zero releases, with no suffix replay. */
    }
}

static void physical_confirm_uses_visible_keyboard_submode(void)
{
    for (int submode = APP_KEYBOARD_MOTION; submode <= APP_KEYBOARD_SHORTCUTS; ++submode) {
        setup(INPUT_KEYBOARD);
        action(APP_KEYBOARD_SUBMODE, submode);
        release_barrier();
        sync_interaction();
        uint64_t start = s_now_ms + 10;
        button_edge(true, start);
        button_edge(false, start + 20);
        buttons_at(start + 300);
        const bool selects_grid = submode == APP_KEYBOARD_MOTION || submode == APP_KEYBOARD_TOUCH;
        assert(s_interaction.view.group == (selects_grid ? 4 : -1));
        assert(s_interaction.view.preview_length == 0);
        start = s_now_ms + 10;
        button_edge(true, start);
        button_edge(false, start + 20);
        buttons_at(start + 300);
        assert(strcmp(s_interaction.view.preview, selects_grid ? "R" : "") == 0);
        expect_idle();
        assert(trace_count == 2); /* Only the submode-change release barrier. */
    }
}

static void shortcuts_use_selected_host_modifier_and_preserve_preview(void)
{
    for (int host = APP_HOST_CTRL; host <= APP_HOST_COMMAND; ++host) {
        setup(INPUT_KEYBOARD);
        fill_preview(2);
        action(APP_MENU, APP_OVERLAY_SETTINGS);
        action(APP_SETTINGS_PAGE, APP_SETTINGS_INPUT);
        action(APP_HOST_OS, host);
        release_barrier();
        action(APP_CLOSE, 0);
        sync_interaction();
        action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_SHORTCUTS);
        release_barrier();
        sync_interaction();
        trace_count = 0;
        action(APP_SHORTCUT, APP_SHORTCUT_COPY);
        const uint8_t key[] = {host == APP_HOST_COMMAND ? 8 : 1, 0, 6, 0, 0, 0, 0, 0};
        static const uint8_t zero[8] = {0};
        expect_report(1, key, sizeof key);
        expect_report(1, zero, sizeof zero);
        assert(strcmp(s_interaction.view.preview, "AB") == 0 && !s_send_token);
        expect_idle();
    }
}

static void lost_button_edges_cannot_unlock(void)
{
    for (unsigned final_down = 0; final_down < 2; ++final_down) {
        setup(INPUT_MOUSE);
        button_edge(true, 1010);
        for (unsigned i = 0; i < 8; ++i)
            physical_button(BOARD_BUTTON_DOWN, (1020U + i) * 1000U);
        physical_button(final_down ? BOARD_BUTTON_DOWN : BOARD_BUTTON_UP, 1030000);
        buttons_at(1040);
        assert(s_engine.state.locked && s_button_count == 0);
        release_barrier();
        buttons_at(3000);
        assert(s_engine.state.locked); /* The lost UP must never turn into a hold/unlock. */
        button_edge(false, 3010);
        buttons_at(3300);
        assert(s_engine.state.locked);
        button_edge(true, 3400);
        buttons_at(4299);
        assert(s_engine.state.locked);
        buttons_at(4300);
        assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
        release_barrier();
        expect_idle();
    }
}

static void theme_changes_are_settings_scoped(void)
{
    setup(INPUT_MOUSE);
    action(APP_THEME, 0);
    assert(s_dark_theme); /* Hidden settings controls cannot change state. */
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    action(APP_THEME, 0);
    assert(s_dark_theme); /* The Settings home is not the Display page. */
    action(APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    release_barrier();
    sync_interaction();
    app_intent_t old_theme = make_intent(APP_THEME, 1, 0, NULL);
    action(APP_THEME, 0);
    assert(!s_dark_theme && s_engine.state.mode == INPUT_MOUSE);
    render_view(&test_link, ESP_OK);
    assert(!test_view.dark_theme && test_view.overlay == APP_OVERLAY_SETTINGS);
    deliver(old_theme);
    assert(!s_dark_theme); /* A change invalidates the previously rendered context. */
    action(APP_THEME, 2);
    assert(!s_dark_theme);
    action(APP_CLOSE, 0);
    action(APP_MENU, APP_OVERLAY_MODES);
    release_barrier();
    sync_interaction();
    action(APP_THEME, 1);
    assert(!s_dark_theme && s_overlay == APP_OVERLAY_MODES);
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    action(APP_THEME, 1);
    assert(!s_dark_theme && s_engine.state.locked);
}

static void queued_double_click_then_hold_keeps_edge_time(void)
{
    setup(INPUT_MOUSE);
    physical_button(BOARD_BUTTON_DOWN, 1010000);
    physical_button(BOARD_BUTTON_UP, 1030000);
    physical_button(BOARD_BUTTON_DOWN, 1100000);
    physical_button(BOARD_BUTTON_UP, 1120000);
    physical_button(BOARD_BUTTON_DOWN, 1200000);
    buttons_at(2101);
    assert(s_now_ms == 2101 && s_engine.state.locked);
    assert(s_overlay == APP_OVERLAY_NONE && !s_engine.state.gyro_enabled);
    release_barrier();
    button_edge(false, 2110);
    buttons_at(2500);
    assert(s_engine.state.locked);
    button_edge(true, 2600);
    buttons_at(3500);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();
    expect_idle();
}

static void completed_quick_touch_still_freezes_motion(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    begin.gesture = 41;
    app_intent_t end = begin;
    end.action = APP_TOUCH_END;
    s_now_ms += 10;
    assert(submit_intent(&begin));
    s_now_ms += 10;
    assert(submit_intent(&end));
    consume_one();
    consume_one(); /* Both callbacks happened before this coordinator poll. */
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void settings_page(app_settings_page_t page)
{
    action(APP_SETTINGS_PAGE, (int)page);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == page);
    release_barrier();
    sync_interaction();
}

static void display_at(uint64_t milliseconds)
{
    s_now_ms = milliseconds;
    service_display();
    sync_interaction();
}

static void brightness_applied(uint8_t percent)
{
    test_brightness = (board_brightness_status_t){
        .applied_known = true, .applied_percent = percent,
        .applied_at_us = s_now_ms * 1000U
    };
    service_display();
}

static void settings_pages_validate_and_persist_changes(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_HOME);
    const app_action_t hidden[] = {APP_THEME, APP_BRIGHTNESS, APP_IDLE_TIMEOUT,
        APP_HAPTICS, APP_HOST_OS, APP_CALIBRATE, APP_WIFI_SETUP, APP_PAIRING_REQUEST};
    for (size_t i = 0; i < sizeof hidden / sizeof hidden[0]; ++i) action(hidden[i], 0);
    assert(test_storage_requests == 0 && test_wifi_starts == 0 && test_pair_opens == 0);
    settings_page(APP_SETTINGS_DISPLAY);
    const uint64_t revision = s_ui_revision;
    action(APP_THEME, 1);
    action(APP_BRIGHTNESS, 70);
    action(APP_IDLE_TIMEOUT, 300);
    assert(test_storage_requests == 0 && s_ui_revision == revision);
    action(APP_BRIGHTNESS, 0);
    action(APP_BRIGHTNESS, 75);
    action(APP_BRIGHTNESS, 110);
    action(APP_IDLE_TIMEOUT, 120);
    action(APP_IDLE_TIMEOUT, -60);
    action(APP_IDLE_TIMEOUT, 61);
    assert(test_storage_requests == 0 && s_settings.brightness_percent == 70);
    app_intent_t stale = make_intent(APP_BRIGHTNESS, 20, 0, NULL);
    action(APP_BRIGHTNESS, 30);
    deliver(stale);
    assert(s_settings.brightness_percent == 30 && test_storage_requests == 1);
    assert(test_saved_request.brightness_percent == 30);
    action(APP_THEME, 0);
    assert(!s_dark_theme && test_storage_requests == 2);
    settings_page(APP_SETTINGS_INPUT);
    action(APP_THEME, 1);
    action(APP_HAPTICS, 0);
    assert(!s_dark_theme && !test_haptics_enabled && !s_settings.haptics_enabled);
    action(APP_HOST_OS, APP_HOST_COMMAND);
    release_barrier();
    assert(s_host_os == APP_HOST_COMMAND && test_storage_requests == 4);
    test_storage_error = ESP_FAIL;
    action(APP_HAPTICS, 1);
    assert(test_storage_requests == 5 && s_settings.haptics_enabled && test_haptics_enabled);
    assert(test_saved_request.host_modifier == DEVICE_HOST_COMMAND);
    settings_page(APP_SETTINGS_DISPLAY);
    test_capabilities.brightness = false;
    action(APP_BRIGHTNESS, 100);
    action(APP_IDLE_TIMEOUT, 0);
    assert(s_settings.brightness_percent == 30 && s_settings.idle_timeout == DEVICE_IDLE_5_MIN);
    assert(test_storage_requests == 5);
}

static void idle_deadlines_ignore_passive_status_updates(void)
{
    const int seconds[] = {60, 300, 600, 1800, 0};
    for (size_t choice = 0; choice < sizeof seconds / sizeof seconds[0]; ++choice) {
        setup(INPUT_MOUSE);
        assert(s_settings.idle_timeout == DEVICE_IDLE_5_MIN);
        settings_page(APP_SETTINGS_DISPLAY);
        action(APP_IDLE_TIMEOUT, seconds[choice]);
        action(APP_CLOSE, 0);
        service_display();
        assert(test_brightness_count == 1 && test_brightness_requests[0] == 70);
        const uint64_t epoch = s_engine.state.epoch;
        const unsigned writes = test_storage_requests;
        const uint64_t deadline = 1000U + (uint64_t)seconds[choice] * 1000U;
        const uint64_t before = seconds[choice] ? deadline - 1 : UINT64_C(5000000000);
        for (uint64_t now = 1000; now < 2000; now += 10) {
            display_at(now);
            test_wifi.revision++;
            test_link.rssi_dbm = -60;
            render_view(&test_link, ESP_OK);
        }
        display_at(before);
        assert(!s_idle.asleep && test_brightness_count == 1);
        display_at(seconds[choice] ? deadline : before + 1000000U);
        assert(s_idle.asleep == (seconds[choice] != 0));
        if (seconds[choice]) {
            assert(test_brightness_count == 2 && test_brightness_requests[1] == 0);
            brightness_applied(0);
            assert(!s_screen_accepts_input);
        }
        for (unsigned i = 0; i < 1000; ++i) {
            ++s_now_ms;
            service_display();
            render_view(&test_link, ESP_OK);
        }
        assert(test_brightness_count == (seconds[choice] ? 2U : 1U));
        assert(test_storage_requests == writes && s_engine.state.epoch == epoch);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_wifi_starts == 0);
        expect_idle();
    }
}

static void display_sleep_preserves_hid_work_and_draft(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    const uint64_t token = s_send_token, epoch = s_engine.state.epoch;
    assert(token);
    display_at(301000);
    assert(s_idle.asleep && s_send_token == token && s_engine.state.epoch == epoch);
    static const uint8_t zero[8] = {0};
    const uint8_t keys[] = {4, 5, 4, 5};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        const uint8_t key = keys[i];
        const uint8_t down[] = {2, 0, key, 0, 0, 0, 0, 0};
        expect_report(1, down, sizeof down);
        update_send(false);
        expect_report(1, zero, sizeof zero);
        update_send(false);
    }
    assert(!s_send_token && s_interaction.view.preview_length == 0 && s_idle.asleep);
    assert(test_storage_requests == 0 && test_disconnects == 0);
    expect_idle();

    setup(INPUT_MOUSE);
    action(APP_DRAG, 1);
    const uint8_t held[] = {1, 0, 0, 0};
    expect_report(2, held, sizeof held);
    display_at(301000);
    assert(s_idle.asleep && s_drag_requested);
    const size_t count = trace_count;
    expect_idle();
    assert(trace_count == count);
    /* A physical hold still requires a separate wake gesture before locking. */
    button_edge(true, 301010);
    buttons_at(302000);
    assert(!s_engine.state.locked && s_drag_requested);
    button_edge(false, 302010);
    brightness_applied(70);
    buttons_at(302290);
    button_edge(true, 302300);
    buttons_at(303200);
    assert(s_engine.state.locked && !s_drag_requested);
    release_barrier();
}

static void touch_wake_consumes_entire_gesture_until_driver_ack(void)
{
    for (unsigned locked = 0; locked < 2; ++locked) {
        setup(INPUT_KEYBOARD);
        fill_preview(2);
        if (locked) {
            physical_lock();
            assert(process_priority());
            release_barrier();
        } else {
            assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
            release_barrier();
            sync_interaction();
        }
        display_at(301000);
        brightness_applied(0);
        trace_count = 0;
        touch(APP_TOUCH_BEGIN, 501, 0, 0);
        assert(!s_idle.asleep && s_idle.wake_capture);
        service_display();
        assert(test_brightness_requests[test_brightness_count - 1] == 70);
        touch(APP_TOUCH_MOVE, 501, 20, 30);
        touch(APP_TOUCH_END, 501, 0, 0);
        touch(APP_KEY, 501, 4, 0);
        assert(strcmp(s_interaction.view.preview, "AB") == 0);
        assert(s_engine.state.locked == (locked != 0));
        assert(s_engine.state.owner == (locked ? INPUT_LOCAL : INPUT_REMOTE));
        expect_idle();
        /* Fresh contacts before illumination is acknowledged are also consumed. */
        touch(APP_TOUCH_BEGIN, 502, 0, 0);
        brightness_applied(70);
        touch(APP_TOUCH_END, 502, 0, 0);
        touch(APP_KEY, 502, 5, 0);
        expect_idle();
        touch(APP_TOUCH_BEGIN, 503, 0, 0);
        touch(APP_TOUCH_END, 503, 0, 0);
        touch(APP_KEY, 503, 6, 0);
        if (!locked) {
            assert(s_engine.state.owner == INPUT_LOCAL);
            release_barrier(); /* A later fresh gesture takes over, and is consumed too. */
        }
        expect_idle();
        assert(test_storage_requests == 0 && strcmp(s_interaction.view.preview, "AB") == 0);
    }
}

static void ai_wake_double_and_hold_are_consumed(void)
{
    setup(INPUT_MOUSE);
    physical_lock();
    assert(process_priority());
    release_barrier();
    display_at(301000);
    brightness_applied(0);
    button_edge(true, 301010);
    button_edge(false, 301030);
    brightness_applied(70);
    button_edge(true, 301100);
    buttons_at(302000);
    assert(s_engine.state.locked && s_overlay == APP_OVERLAY_NONE);
    button_edge(false, 302010);
    buttons_at(302290);
    display_at(302290);
    assert(s_engine.state.locked && !s_idle.wake_capture);
    button_edge(true, 302300);
    buttons_at(303199);
    assert(s_engine.state.locked);
    buttons_at(303200);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();
    assert(test_storage_requests == 0);
}

static void display_activity_clock_and_optional_driver(void)
{
    setup(INPUT_MOUSE);
    service_display();
    display_at(300999);
    /* Physical callbacks still renew idle if a safety queue reset drops their intents. */
    app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    begin.gesture = 601;
    assert(submit_intent(&begin));
    app_intent_t end = begin;
    end.action = APP_TOUCH_END;
    assert(submit_intent(&end));
    xQueueReset(s_intents);
    display_at(301000);
    assert(!s_idle.asleep);
    display_at(600998);
    assert(!s_idle.asleep);
    display_at(600999);
    assert(s_idle.asleep);
    display_at(500000); /* Backward time wakes and rebases; no subtraction underflow. */
    assert(!s_idle.asleep);
    display_at(799999);
    assert(!s_idle.asleep);
    display_at(800000);
    assert(s_idle.asleep);

    setup(INPUT_MOUSE);
    test_capabilities.brightness = false;
    test_brightness = (board_brightness_status_t){.error = ESP_FAIL};
    display_at(UINT64_C(5000000000));
    assert(!s_idle.asleep && s_screen_accepts_input && test_brightness_count == 0);
    touch(APP_TOUCH_BEGIN, 701, 0, 0);
    touch(APP_TOUCH_END, 701, 0, 0);
    touch(APP_CLICK, 701, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
}

static void timeout_changes_start_a_full_new_interval(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_DISPLAY);
    action(APP_IDLE_TIMEOUT, 1800);
    display_at(100000);
    action(APP_IDLE_TIMEOUT, 60);
    display_at(159999);
    assert(!s_idle.asleep);
    display_at(160000);
    assert(s_idle.asleep);
    brightness_applied(0);
    /* A hidden timeout request cannot change or wake a sleeping display. */
    action(APP_IDLE_TIMEOUT, 0);
    assert(s_idle.asleep && s_settings.idle_timeout == DEVICE_IDLE_1_MIN);
    touch(APP_TOUCH_BEGIN, 801, 0, 0);
    touch(APP_TOUCH_END, 801, 0, 0);
    brightness_applied(70);
    display_at(160010);
    action(APP_IDLE_TIMEOUT, 1800);
    assert(!s_idle.asleep);
    display_at(1960009);
    assert(!s_idle.asleep);
    display_at(1960010);
    assert(s_idle.asleep);
    touch(APP_TOUCH_BEGIN, 802, 0, 0);
    touch(APP_TOUCH_END, 802, 0, 0);
    brightness_applied(70);
    action(APP_IDLE_TIMEOUT, 0);
    display_at(UINT64_C(5000000000));
    assert(!s_idle.asleep && s_settings.idle_timeout == DEVICE_IDLE_OFF);
    action(APP_IDLE_TIMEOUT, 60);
    display_at(UINT64_C(5000059999));
    assert(!s_idle.asleep);
    display_at(UINT64_C(5000060000));
    assert(s_idle.asleep);
    assert(test_storage_requests == 5);
}

static void failed_wake_is_blocked_until_a_fresh_acknowledged_gesture(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    test_brightness_error = ESP_FAIL;
    test_brightness.error = ESP_FAIL;
    touch(APP_TOUCH_BEGIN, 901, 0, 0);
    touch(APP_TOUCH_END, 901, 0, 0);
    service_display();
    const size_t attempts = test_brightness_count;
    for (unsigned i = 0; i < 1000; ++i) {
        ++s_now_ms;
        service_display();
    }
    assert(!s_screen_accepts_input && test_brightness_count == attempts);
    touch(APP_TOUCH_BEGIN, 902, 0, 0);
    touch(APP_TOUCH_END, 902, 0, 0);
    touch(APP_CLICK, 902, 1, 0);
    assert(test_brightness_count == attempts + 1 && !s_screen_accepts_input);
    expect_idle();
    test_brightness_error = ESP_OK;
    touch(APP_TOUCH_BEGIN, 903, 0, 0);
    assert(test_brightness_count == attempts + 2);
    brightness_applied(70);
    touch(APP_TOUCH_END, 903, 0, 0);
    touch(APP_CLICK, 903, 1, 0);
    expect_idle();
    touch(APP_TOUCH_BEGIN, 904, 0, 0);
    touch(APP_TOUCH_END, 904, 0, 0);
    touch(APP_CLICK, 904, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
    assert(test_storage_requests == 0);
}

static void wifi_setup_lifecycle_and_secret_scope(void)
{
    setup(INPUT_MOUSE);
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 0);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 1 && s_overlay == APP_OVERLAY_SETTINGS);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 2 && s_overlay == APP_OVERLAY_WIFI_SETUP);
    release_barrier();
    render_view(&test_link, ESP_OK);
    assert(!test_view.wifi_setup_active && test_view.wifi_setup_passcode[0] == '\0');
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .setup_active = true,
        .state = APP_WIFI_IDLE, .setup_remaining_ms = 599001};
    test_wifi_info = (app_wifi_setup_info_t){.generation = 4,
        .ssid = "TEST-SETUP", .password = "synthetic-password", .ip = "192.0.2.1"};
    test_wifi_info_error = ESP_OK;
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_active && strcmp(test_view.wifi_setup_passcode, "synthetic-password") == 0);
    assert(strcmp(test_view.wifi_setup_url, "http://192.0.2.1") == 0);
    assert(test_view.wifi_setup_seconds_left == 600);
    app_intent_t stale_start = make_intent(APP_WIFI_SETUP, 1, 0, NULL);
    action(APP_CLOSE, 0);
    assert(test_wifi_stops == 1 && s_overlay == APP_OVERLAY_NONE);
    const unsigned reads = test_wifi_info_reads;
    render_view(&test_link, ESP_OK);
    assert(test_wifi_info_reads == reads && test_view.wifi_setup_passcode[0] == '\0');
    assert(test_view.wifi_setup_ssid[0] == '\0' && test_view.wifi_setup_url[0] == '\0');
    deliver(stale_start);
    assert(test_wifi_starts == 2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1); /* Existing AP opens its page without another start. */
    release_barrier();
    assert(test_wifi_starts == 2);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_HOME);
    release_barrier();
    assert(test_wifi_stops == 2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    assert(test_wifi_stops == 3 && s_engine.state.locked);
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_passcode[0] == '\0');
    assert(test_storage_requests == 0 && test_pair_forgets == 0);
}

static void wifi_forget_requires_current_explicit_confirmation(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_FORGET_REQUEST, 0);
    assert(s_overlay == APP_OVERLAY_SETTINGS && test_wifi_forgets == 0);
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .configured = true,
        .ssid = "TEST-NETWORK-A", .state = APP_WIFI_IDLE};
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_WIFI_FORGET);
    app_intent_t old_confirm = make_intent(APP_CONFIRM, 1, 0, NULL);
    action(APP_THEME, 0);
    action(APP_KEY, 4);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    assert(s_overlay == APP_OVERLAY_CONFIRM && test_storage_requests == 0);
    action(APP_CONFIRM, 0);
    release_barrier();
    deliver(old_confirm);
    assert(test_wifi_forgets == 0);
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    strcpy(test_wifi.ssid, "TEST-NETWORK-B");
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(test_wifi_forgets == 0);
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    test_wifi.operation_pending = true;
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(test_wifi_forgets == 0);
    test_wifi.operation_pending = false;
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    test_wifi_forget_error = ESP_OK;
    app_intent_t confirm = make_intent(APP_CONFIRM, 1, 0, NULL);
    deliver(confirm);
    release_barrier();
    deliver(confirm);
    assert(test_wifi_forgets == 1 && test_storage_requests == 0 && test_pair_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
    expect_idle();
}

static void pairing_confirmation_releases_before_disconnect_and_forget(void)
{
    for (int operation = APP_PAIR_OPEN; operation <= APP_PAIR_FORGET_ALL; ++operation) {
        setup(INPUT_KEYBOARD);
        fill_preview(2);
        press(INPUT_KEYBOARD);
        action(APP_MENU, APP_OVERLAY_TARGET);
        release_barrier();
        action(APP_PAIRING_REQUEST, operation);
        release_barrier();
        assert(s_overlay == APP_OVERLAY_CONFIRM && test_pair_opens == 0 && test_pair_forgets == 0);
        app_intent_t approve = make_intent(APP_CONFIRM, 1, 0, NULL);
        deliver(approve);
        assert(test_pair_closes == 1);
        service_pair_operation(&test_link);
        assert(test_disconnects == 0); /* Approval itself created another release barrier. */
        static const uint8_t zero[8] = {0};
        expect_report(1, zero, 8);
        service_pair_operation(&test_link);
        assert(test_disconnects == 0); /* Both keyboard and mouse must be released. */
        expect_report(2, zero, 4);
        if (operation == APP_PAIR_CLOSE) {
            assert(s_pair_operation == APP_CONFIRM_NONE);
            service_pair_operation(&test_link);
            assert(test_disconnects == 0 && test_pair_opens == 0 && test_pair_forgets == 0);
            continue;
        }
        test_disconnect_accept = true;
        const uint64_t disconnect_generation = test_link.generation;
        service_pair_operation(&test_link);
        assert(test_disconnects == 1 && test_disconnect_generation == disconnect_generation);
        assert(test_link.generation == disconnect_generation + 1 && test_link.connected && !test_link.ready);
        assert(test_disconnect_trace_count == trace_count);
        service_pair_operation(&test_link);
        assert(test_disconnects == 1 && test_pair_forgets == 0 && test_pair_opens == 0);
        assert(s_pair_operation != APP_CONFIRM_NONE);
        test_link.connected = test_link.ready = false;
        ++test_link.generation;
        hid_engine_set_link(&s_engine, false, false);
        test_pair_accept = true;
        service_pair_operation(&test_link);
        assert(s_pair_operation == APP_CONFIRM_NONE);
        assert(test_pair_forgets == (operation == APP_PAIR_FORGET_ALL ? 1U : 0U));
        assert(test_pair_opens == (operation == APP_PAIR_OPEN ? 1U : 0U));
        if (operation == APP_PAIR_FORGET_ALL) assert(test_forget_generation == test_link.generation);
        deliver(approve);
        service_pair_operation(&test_link);
        assert(test_pair_closes == 1 && test_storage_requests == 0 && test_wifi_forgets == 0);
        assert(strcmp(s_interaction.view.preview, "AB") == 0);
    }
}

static void pairing_stale_generation_failure_and_lock_cancel(void)
{
    setup(INPUT_MOUSE);
    action(APP_MENU, APP_OVERLAY_TARGET);
    release_barrier();
    action(APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL);
    release_barrier();
    ++test_link.generation;
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(s_pair_operation == APP_CONFIRM_NONE && test_pair_closes == 0);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL);
    release_barrier();
    action(APP_CONFIRM, 1);
    release_barrier();
    test_link.congested = true;
    test_send_failure = true;
    /* Refused close does not become success and expires at a bounded deadline. */
    s_now_ms = 5999;
    service_pair_operation(&test_link);
    assert(s_pair_operation != APP_CONFIRM_NONE && test_pair_forgets == 0);
    s_now_ms = 6000;
    service_pair_operation(&test_link);
    assert(s_pair_operation == APP_CONFIRM_NONE && test_pair_forgets == 0);
    action(APP_PAIRING_REQUEST, APP_PAIR_OPEN);
    action(APP_CONFIRM, 1);
    physical_lock();
    assert(process_priority());
    assert(s_engine.state.locked && s_pair_operation == APP_CONFIRM_NONE);
    test_link.connected = false;
    test_pair_accept = true;
    service_pair_operation(&test_link);
    assert(test_pair_opens == 0 && test_pair_forgets == 0);
}

static void extended_status_views_do_not_invent_evidence_or_writes(void)
{
    setup(INPUT_AUDIO);
    render_view(&test_link, ESP_OK);
    assert(!test_view.battery_percent_valid && test_view.battery_state == APP_BATTERY_UNKNOWN);
    assert(!test_view.wifi_rssi_valid);
    assert(test_view.wifi_state == APP_UI_WIFI_UNAVAILABLE && test_view.paired_host_count == -1);
    assert(test_view.paired_host_capacity == 0);
    assert(test_view.settings_save_state == APP_SAVE_DEFAULTS);
    assert(test_view.brightness_percent == 70 && test_view.idle_timeout_seconds == 300);
    assert(!test_view.audio_available && test_view.microphone_muted);
    test_power_error = ESP_OK;
    test_power = (board_power_status_t){.gauge_available = true,
        .battery_present = BOARD_TRUTH_YES, .percent_valid = true, .percent = 63,
        .charging = BOARD_TRUTH_YES, .current_valid = true, .current_ma = 120};
    test_link.rssi_valid = true;
    test_link.rssi_dbm = -82;
    test_link.bond_count_known = true;
    test_link.bond_count = 2;
    test_link.bond_capacity = 32;
    test_link.pairing_open = true;
    test_link.pairing_remaining_ms = 1001;
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .configured = true,
        .state = APP_WIFI_CONNECTED, .rssi_valid = true, .rssi_dbm = -41,
        .ssid = "TEST-NETWORK", .ip = "192.0.2.14"};
    test_storage.pending = true;
    test_storage.requested_revision = 2;
    test_storage.saved_revision = 1;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_percent_valid && test_view.battery_percent == 63);
    assert(test_view.battery_state == APP_BATTERY_CHARGING);
    assert(test_view.wifi_rssi_valid && test_view.wifi_rssi_dbm == -41);
    assert(strcmp(test_view.wifi_ssid, "TEST-NETWORK") == 0 && strcmp(test_view.wifi_ipv4, "192.0.2.14") == 0);
    assert(test_view.paired_host_count == 2 && test_view.pairing_state == APP_PAIRING_OPEN);
    assert(test_view.paired_host_capacity == 32);
    assert(test_view.pairing_seconds_left == 2 && test_view.settings_save_state == APP_SAVE_PENDING);
    test_power.charging = BOARD_TRUTH_NO;
    test_power.current_ma = -30;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_state == APP_BATTERY_DISCHARGING);
    test_power.full = BOARD_TRUTH_YES;
    test_power.percent = 100;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_state == APP_BATTERY_FULL && test_view.battery_percent == 100);
    /* Board read failures publish an invalidated cache, never an invented 0% battery. */
    test_power_error = ESP_FAIL;
    test_power = (board_power_status_t){.error = ESP_FAIL};
    test_link.connected = false;
    test_wifi.state = APP_WIFI_BACKOFF;
    test_wifi.retry_in_ms = 1001;
    test_storage.pending = false;
    test_storage.error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(!test_view.battery_percent_valid && test_view.battery_state == APP_BATTERY_UNKNOWN);
    assert(!test_view.wifi_rssi_valid);
    assert(test_view.wifi_state == APP_UI_WIFI_RETRYING && test_view.settings_save_state == APP_SAVE_ERROR);
    test_storage.error = ESP_OK;
    test_storage.saved_revision = 2;
    render_view(&test_link, ESP_OK);
    assert(test_view.settings_save_state == APP_SAVE_SAVED);
    test_storage.available = false;
    test_link.bond_forget_state = BLE_HID_FORGET_FAILED;
    test_wifi.state = APP_WIFI_ERROR;
    test_wifi.last_error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(test_view.settings_save_state == APP_SAVE_UNAVAILABLE && test_view.pairing_state == APP_PAIRING_ERROR);
    assert(test_view.wifi_state == APP_UI_WIFI_ERROR && test_view.wifi_message[0]);
    const uint64_t revision = s_ui_revision, epoch = s_engine.state.epoch;
    for (unsigned i = 0; i < 1000; ++i) render_view(&test_link, ESP_OK);
    assert(s_ui_revision == revision && s_engine.state.epoch == epoch);
    assert(test_storage_requests == 0 && test_brightness_count == 0 && test_wifi_info_reads == 0);
    expect_idle();
}

static void all_settings_pages_own_focus_in_every_mode(void)
{
    const app_action_t background[] = {APP_GRID, APP_KEY, APP_CLICK, APP_MOVE,
        APP_SEND, APP_CLEAR, APP_SHORTCUT, APP_MODE, APP_MOTION, APP_DRAG, APP_VOICE_START};
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        for (int page = APP_SETTINGS_HOME; page <= APP_SETTINGS_WIFI; ++page) {
            setup((input_mode_t)mode);
            settings_page((app_settings_page_t)page);
            const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
            const size_t reports = trace_count;
            for (size_t i = 0; i < sizeof background / sizeof background[0]; ++i)
                action(background[i], 1);
            assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == (app_settings_page_t)page);
            assert(s_engine.state.mode == (input_mode_t)mode && s_engine.state.epoch == epoch);
            assert(s_ui_revision == revision && test_storage_requests == 0 && trace_count == reports);
            expect_idle();
        }
    }
}

static void physical_modal_exit_stops_wifi_setup(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    test_wifi.setup_active = true;
    button_edge(true, 1010);
    button_edge(false, 1030);
    button_edge(true, 1100);
    button_edge(false, 1120);
    assert(s_overlay == APP_OVERLAY_NONE);
    assert(test_wifi_stops == 1); /* Leaving setup must close its AP on every navigation path. */
    release_barrier();
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_passcode[0] == '\0');
}

static void wake_gesture_cannot_enter_the_priority_lock_path(void)
{
    const app_action_t controls[] = {APP_LOCK, APP_RELEASE};
    for (size_t i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
        setup(INPUT_MOUSE);
        display_at(301000);
        brightness_applied(0);
        touch(APP_TOUCH_BEGIN, 1001, 0, 0);
        brightness_applied(70);
        touch(APP_TOUCH_END, 1001, 0, 0);
        service_display();
        const uint64_t epoch = s_engine.state.epoch;
        app_intent_t old_control = make_intent(controls[i], 0, 0, NULL);
        old_control.gesture = 1001;
        assert(submit_intent(&old_control));
        if (!process_priority()) {
            consume_one();
            (void)process_priority();
        }
        assert(!s_engine.state.locked && s_engine.state.epoch == epoch);
        expect_idle(); /* Consumed wake gestures cannot trigger either priority control. */
    }
}

static void release_priority_stops_wifi_setup(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    app_intent_t release = make_intent(APP_RELEASE, 0, 0, NULL);
    assert(submit_intent(&release) && process_priority());
    assert(s_overlay == APP_OVERLAY_NONE && test_wifi_stops == 1);
    release_barrier();
}

static void priority_reset_reconciles_a_queued_touch_release(void)
{
    setup(INPUT_MOUSE);
    touch(APP_TOUCH_BEGIN, 1101, 0, 0);
    s_now_ms = 1010;
    app_intent_t end = make_intent(APP_TOUCH_END, 0, 0, NULL);
    end.gesture = 1101;
    assert(submit_intent(&end));
    assert(!s_touch_down); /* Callback evidence arrived; its queued END has not. */
    app_intent_t release = make_intent(APP_RELEASE, 0, 0, NULL);
    assert(submit_intent(&release) && process_priority());
    expect_empty_intents();
    release_barrier();
    display_at(301009);
    assert(!s_idle.asleep);
    display_at(301010);
    assert(s_idle.asleep); /* A discarded END must not keep the device awake forever. */
    assert(!s_idle.down[DEVICE_INPUT_TOUCH]);
}

static void overflow_reconciles_a_captured_wake_touch_release(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    touch(APP_TOUCH_BEGIN, 1201, 0, 0);
    brightness_applied(70);
    assert(s_idle.wake_capture);
    fill_intents();
    s_now_ms = 301010;
    app_intent_t end = make_intent(APP_TOUCH_END, 0, 0, NULL);
    end.gesture = 1201;
    assert(!submit_intent(&end)); /* Full queue loses END but retains callback state. */
    assert(!s_touch_down && process_priority());
    release_barrier();
    service_display();
    assert(!s_idle.wake_capture && !s_idle.down[DEVICE_INPUT_TOUCH]);
    const uint64_t epoch = s_engine.state.epoch;
    touch(APP_CLICK, 1201, 1, 0);
    assert(s_engine.state.epoch == epoch);
    expect_idle(); /* Reconciliation must retain the consumed gesture watermark. */
    touch(APP_TOUCH_BEGIN, 1202, 0, 0);
    touch(APP_TOUCH_END, 1202, 0, 0);
    touch(APP_CLICK, 1202, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
    display_at(601010);
    assert(s_idle.asleep);
}

static void fresh_touch_safety_during_ai_guard(app_action_t control)
{
    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    button_edge(true, 301010);
    button_edge(false, 301030);
    brightness_applied(70);
    assert(s_idle.wake_capture && !s_screen_accepts_input);
    s_now_ms = 301040;
    touch(APP_TOUCH_BEGIN, 1301, 0, 0);
    touch(APP_TOUCH_END, 1301, 0, 0);
    service_display();
    assert(s_idle.wake_capture); /* The AI-only double-click guard still exists. */
    app_intent_t safety = make_intent(control, 0, 0, NULL);
    safety.gesture = 1301;
    assert(submit_intent(&safety));
    if (!process_priority()) {
        consume_one();
        (void)process_priority();
    }
    assert(s_engine.state.release_pending);
    assert(s_engine.state.locked == (control == APP_LOCK));
    release_barrier();
    expect_idle();
}

static void fresh_touch_lock_survives_ai_wake_guard(void)
{
    fresh_touch_safety_during_ai_guard(APP_LOCK);
}

static void fresh_touch_release_survives_ai_wake_guard(void)
{
    fresh_touch_safety_during_ai_guard(APP_RELEASE);
}

static void bluetooth_bond_views_preserve_unknown_and_verified_identity(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_BLUETOOTH);
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    test_bonds = (ble_hid_bond_list_t){.known = true, .revision = 1};
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    test_bonds.count = 2;
    test_bonds.bonds[0] = (ble_hid_bond_t){.address = {2, 3, 4, 5, 6, 7},
        .address_type_known = true, .address_type = 1, .is_current = true};
    test_bonds.bonds[1] = (ble_hid_bond_t){.address = {2, 8, 9, 10, 11, 12},
        .address_type = 0};
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_host_count == 2);
    assert(strcmp(test_view.bluetooth_hosts[0].identity, "02:03:04:05:06:07/1") == 0);
    assert(strcmp(test_view.bluetooth_hosts[1].identity, "02:08:09:0A:0B:0C/?0") == 0);
    assert(test_view.bluetooth_hosts[0].name[0] == '\0' && test_view.bluetooth_hosts[1].name[0] == '\0');
    assert(!test_view.bluetooth_hosts[0].connected); /* A row flag alone cannot prove an active bond. */
    test_bonds.current_match_known = true;
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_hosts[0].connected && !test_view.bluetooth_hosts[1].connected);
    test_link.connected = false;
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts[0].connected);
    test_bonds.known = false;
    test_bonds.error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    assert(test_view.bluetooth_hosts[0].identity[0] == '\0');
    assert(test_storage_requests == 0 && test_pair_forgets == 0 && test_disconnects == 0);
}

static void bluetooth_pagination_is_bounded_and_contextual(void)
{
    setup(INPUT_KEYBOARD);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 32, .revision = 8};
    for (uint8_t i = 0; i < 32; ++i) {
        test_bonds.bonds[i].address[0] = 2;
        test_bonds.bonds[i].address[5] = i;
        test_bonds.bonds[i].address_type_known = true;
    }
    action(APP_BLUETOOTH_PAGE, 7);
    assert(s_bluetooth_page == 0); /* Hidden pagination has no effect. */
    settings_page(APP_SETTINGS_BLUETOOTH);
    app_intent_t stale = make_intent(APP_BLUETOOTH_PAGE, 0, 0, NULL);
    action(APP_BLUETOOTH_PAGE, 7);
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_page == 7 && test_view.bluetooth_host_count == 32);
    assert(strcmp(test_view.bluetooth_hosts[31].identity, "02:00:00:00:00:1F/0") == 0);
    deliver(stale);
    assert(s_bluetooth_page == 7);
    action(APP_BLUETOOTH_PAGE, -1);
    action(APP_BLUETOOTH_PAGE, 8);
    assert(s_bluetooth_page == 7);
    test_bonds.count = 1;
    ++test_bonds.revision;
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_page == 0 && test_view.bluetooth_host_count == 1);
    assert(test_view.bluetooth_hosts[1].identity[0] == '\0');
    action(APP_BLUETOOTH_PAGE, 1);
    assert(s_bluetooth_page == 0);
    action(APP_THEME, 0);
    action(APP_KEY, 4);
    action(APP_WIFI_SETUP, 1);
    assert(test_storage_requests == 0 && test_wifi_starts == 0);
    expect_idle();
}

static void disconnect_current_host_preserves_saved_pairings(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 4};
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_DISCONNECT);
    action(APP_CONFIRM, 1);
    service_pair_operation(&test_link);
    assert(test_disconnects == 0);
    release_barrier();
    test_disconnect_accept = true;
    service_pair_operation(&test_link);
    assert(test_disconnects == 1 && test_pair_opens == 0 && test_pair_forgets == 0);
    service_pair_operation(&test_link);
    assert(test_disconnects == 1);
    test_link.connected = test_link.ready = false;
    ++test_link.generation;
    hid_engine_set_link(&s_engine, false, false);
    service_pair_operation(&test_link);
    assert(s_pair_operation == APP_CONFIRM_NONE && test_pair_opens == 0 && test_pair_forgets == 0);
    assert(test_storage_requests == 0 && test_wifi_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_host_count == 2 && test_bonds.revision == 4);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    assert(s_overlay == APP_OVERLAY_SETTINGS && test_disconnects == 1);
}

static void saturated_queue_preserves_real_touch_lock(void)
{
    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    touch(APP_TOUCH_BEGIN, 1401, 0, 0);
    touch(APP_TOUCH_END, 1401, 0, 0);
    fill_intents();
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    lock.gesture = 1401; /* The real UI path carries its physical gesture identity. */
    (void)submit_intent(&lock);
    assert(process_priority());
    assert(s_engine.state.locked); /* Queue pressure cannot downgrade Lock to Release. */
    expect_empty_intents();
    release_barrier();
    expect_idle();
}

static void nine_cell_grid_reaches_every_group_and_rejects_empty_children(void)
{
    const char *groups[] = {"ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"};
    const char last[] = "DHLPTX159";
    for (int group = 0; group < 9; ++group) {
        setup(INPUT_KEYBOARD);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_cell_count == 9 && test_view.keyboard_focus == 4);
        for (size_t cell = 0; cell < 9; ++cell)
            assert(strcmp(test_view.keyboard_cells[cell], groups[cell]) == 0);
        action(APP_GRID, group);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_group == group && test_view.keyboard_cell_count == 4);
        assert(test_view.keyboard_focus == 1);
        const uint64_t revision = s_ui_revision;
        for (int cell = 4; cell < 9; ++cell) {
            assert(test_view.keyboard_cells[cell][0] == '\0');
            action(APP_GRID, cell);
        }
        assert(s_ui_revision == revision && s_interaction.view.preview_length == 0);
        action(APP_GRID, 3);
        assert(s_interaction.view.preview_length == 1 && s_interaction.view.preview[0] == last[group]);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_group == -1 && test_view.keyboard_focus == 4);
        action(APP_GRID, 9);
        action(APP_GRID, -1);
        assert(s_interaction.view.group == -1 && s_interaction.view.preview_length == 1);
        expect_idle();
    }
    setup(INPUT_KEYBOARD);
    action(APP_SYMBOLS, 0);
    action(APP_GRID, 8);
    render_view(&test_link, ESP_OK);
    assert(test_view.keyboard_cell_count == 3 && test_view.keyboard_focus == 1);
    for (int cell = 3; cell < 9; ++cell) action(APP_GRID, cell);
    assert(s_interaction.view.preview_length == 0);
    action(APP_GRID, 2);
    assert(strcmp(s_interaction.view.preview, "^") == 0);
    expect_idle();
}

static void saturated_queue_rejects_consumed_wake_emergencies(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    touch(APP_TOUCH_BEGIN, 1501, 0, 0);
    touch(APP_TOUCH_END, 1501, 0, 0);
    brightness_applied(70);
    fill_intents();
    const uint64_t epoch = s_engine.state.epoch;
    app_intent_t control = make_intent(APP_LOCK, 0, 0, NULL);
    control.gesture = 1501;
    assert(submit_intent(&control));
    control.action = APP_RELEASE;
    assert(submit_intent(&control));
    assert(!process_priority());
    assert(!s_engine.state.locked && s_engine.state.epoch == epoch);
    expect_idle();
    assert(!process_priority()); /* Rejected emergency slots must not replay later. */
}

static void saturated_emergency_waits_for_its_queued_begin(void)
{
    for (unsigned asleep = 0; asleep < 2; ++asleep) {
        setup(INPUT_MOUSE);
        if (asleep) {
            display_at(301000);
            brightness_applied(0);
        }
        app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
        begin.gesture = 1601;
        assert(submit_intent(&begin));
        app_intent_t end = begin;
        end.action = APP_TOUCH_END;
        assert(submit_intent(&end));
        fill_intents();
        app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
        lock.gesture = 1601;
        assert(submit_intent(&lock));
        assert(!process_priority());
        assert(!s_engine.state.locked); /* A not-yet-routed BEGIN proves neither wake nor admission. */
        consume_one();
        if (!asleep) {
            assert(process_priority() && s_engine.state.locked);
            expect_empty_intents();
            release_barrier();
            assert(!s_idle.down[DEVICE_INPUT_TOUCH]);
        } else {
            assert(!process_priority() && !s_engine.state.locked);
            while (s_intents->count) consume_one();
            brightness_applied(70);
            assert(!s_idle.wake_capture && !s_idle.down[DEVICE_INPUT_TOUCH]);
            assert(!process_priority());
        }
        expect_idle();
    }
}

static void transport_setup(input_mode_t mode, app_input_transport_t selected)
{
    setup(mode);
    test_usb = (usb_hid_snapshot_t){.initialized = true, .mounted = true,
        .enabled = selected == APP_INPUT_USB, .ready = selected == APP_INPUT_USB,
        .generation = 71};
    if (selected == APP_INPUT_USB) {
        s_input_transport = s_requested_transport = selected;
        s_settings.preferred_transport = DEVICE_TRANSPORT_USB;
        hid_engine_set_link(&s_engine, false, false);
        hid_engine_set_link(&s_engine, true, true);
        release_barrier();
    }
    trace_count = 0;
    test_ble_send_attempts = test_usb_send_attempts = 0;
    test_previous_link = input_link_snapshot(&test_link);
}

static void acknowledge_selected_transport(void)
{
    if (s_input_transport == APP_INPUT_USB) {
        assert(test_usb.enabled);
        test_usb.ready = true; /* Explicit synthetic stack acknowledgement, not selection admission. */
    }
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
}

static void transport_switch_waits_for_both_releases_and_completion(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (int mode = INPUT_MOUSE; mode <= INPUT_KEYBOARD; ++mode) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup((input_mode_t)mode, from);
            if (mode == INPUT_MOUSE) {
                action(APP_DRAG, 1);
                const uint8_t held[] = {1, 0, 0, 0};
                expect_report(2, held, sizeof held);
            } else {
                fill_preview(2);
                press(INPUT_KEYBOARD);
            }
            assert(trace_count == 1 && trace[0].transport == from);
            action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            assert(s_transport_switch_pending && s_input_transport == from);
            assert(test_storage_requests == 0 && test_usb_enable_count == 0);
            service_transport_switch();
            assert(s_transport_switch_pending);
            static const uint8_t zero[8] = {0};
            expect_report(1, zero, 8);
            service_transport_switch();
            assert(s_transport_switch_pending && test_usb_enable_count == 0);
            expect_report(2, zero, 4);
            assert(trace[1].transport == from && trace[2].transport == from);
            if (from == APP_INPUT_USB) test_usb.congested = true;
            else test_link.congested = true;
            service_transport_switch();
            assert(s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
            if (from == APP_INPUT_USB) test_usb.congested = false;
            else test_link.congested = false;
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == to);
            assert(test_usb_enable_count == 1 && test_usb_enable_requests[0] == (to == APP_INPUT_USB));
            assert(test_usb_enable_trace[0] == 3 && test_storage_requests == 1);
            assert(test_saved_request.preferred_transport ==
                (to == APP_INPUT_USB ? DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH));
            assert(s_engine.state.mode == (input_mode_t)mode && !s_engine.state.gyro_enabled && !s_drag_requested);
            acknowledge_selected_transport();
            release_barrier();
            assert(trace[3].transport == to && trace[4].transport == to);
            action(APP_CLOSE, 0);
            if (mode == INPUT_KEYBOARD) {
                assert(strcmp(s_interaction.view.preview, "AB") == 0);
                action(APP_KEY, 4);
                const uint8_t down[] = {0, 0, 4, 0, 0, 0, 0, 0};
                expect_report(1, down, sizeof down);
                expect_report(1, zero, 8);
            } else {
                action(APP_CLICK, 1);
                const uint8_t down[] = {1, 0, 0, 0};
                expect_report(2, down, sizeof down);
                expect_report(2, zero, 4);
            }
            assert(trace[5].transport == to && trace[6].transport == to);
            assert(test_pair_forgets == 0 && test_wifi_forgets == 0 && test_disconnects == 0);
        }
    }
}

static void transport_selection_is_page_scoped_and_failure_visible(void)
{
    transport_setup(INPUT_AUDIO, APP_INPUT_BLUETOOTH);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending && test_usb_enable_count == 0);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, -1);
    action(APP_TRANSPORT_SELECT, 2);
    action(APP_TRANSPORT_SELECT, APP_INPUT_BLUETOOTH);
    assert(!s_transport_switch_pending && test_storage_requests == 0);
    test_usb.initialized = false;
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    test_usb.initialized = true;
    test_usb.failed = true;
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    test_usb.failed = false;
    app_intent_t stale = make_intent(APP_TRANSPORT_SELECT, APP_INPUT_USB, 0, NULL);
    settings_page(APP_SETTINGS_DISPLAY);
    deliver(stale);
    assert(!s_transport_switch_pending);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(s_transport_switch_pending && test_storage_requests == 0);
    test_usb.failed = true; /* Availability can disappear after admission. */
    service_transport_switch();
    assert(!s_transport_switch_pending && s_input_transport == APP_INPUT_BLUETOOTH);
    assert(s_requested_transport == APP_INPUT_BLUETOOTH && test_usb_enable_count == 0);
    assert(test_storage_requests == 0 && s_engine.state.mode == INPUT_AUDIO);
    test_usb.failed = false;
    service_transport_switch();
    assert(s_input_transport == APP_INPUT_BLUETOOTH); /* Late readiness cannot revive a cancelled switch. */
    release_barrier();
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    release_barrier();
    test_storage_error = ESP_FAIL;
    service_transport_switch();
    assert(s_input_transport == APP_INPUT_USB && test_storage_requests == 1);
    assert(s_engine.state.mode == INPUT_AUDIO && !s_engine.state.gyro_enabled);
    test_storage = (settings_store_status_t){.available = true, .error = ESP_FAIL};
    render_view(&test_link, ESP_OK);
    assert(test_view.input_transport == APP_INPUT_USB && test_view.settings_save_state == APP_SAVE_ERROR);
    assert(!test_view.audio_available && test_view.microphone_muted);
}

static void transport_blocked_old_host_times_out_without_persisting(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_MOUSE, from);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            release_barrier();
            if (from == APP_INPUT_USB) {
                test_usb.suspended = fault == 0;
                test_usb.failed = fault == 1;
                test_usb.congested = fault == 2;
                if (fault < 2) test_usb.ready = false;
            } else {
                test_link.suspended = fault == 0;
                test_link.failed = fault == 1;
                test_link.congested = fault == 2;
                if (fault < 2) test_link.ready = false;
            }
            s_now_ms = 5999;
            service_transport_switch();
            assert(s_transport_switch_pending && s_input_transport == from);
            s_now_ms = 6000;
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from && s_requested_transport == from);
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
            test_usb.suspended = test_usb.failed = test_usb.congested = false;
            test_link.suspended = test_link.failed = test_link.congested = false;
            service_transport_switch();
            assert(s_input_transport == from && test_storage_requests == 0);
        }
    }
}

static void transport_lock_or_release_cancels_late_switch_completion(void)
{
    const app_action_t controls[] = {APP_LOCK, APP_RELEASE};
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (size_t i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_KEYBOARD, from);
            fill_preview(2);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            app_intent_t stale = make_intent(APP_TRANSPORT_SELECT, (int)to, 0, NULL);
            app_intent_t control = make_intent(controls[i], 0, 0, NULL);
            assert(submit_intent(&control) && process_priority());
            release_barrier();
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(s_engine.state.locked == (controls[i] == APP_LOCK));
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
            deliver(stale);
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(strcmp(s_interaction.view.preview, "AB") == 0);
        }
    }
}

static void disconnected_old_transport_allows_explicit_offline_selection(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        fill_preview(2);
        test_link.connected = test_link.ready = false;
        hid_engine_set_link(&s_engine, false, false);
        test_usb.mounted = test_usb.ready = false;
        action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        const size_t before = trace_count;
        service_transport_switch();
        assert(s_input_transport == to && !s_transport_switch_pending);
        assert(trace_count == before && test_storage_requests == 1);
        assert(test_usb.enabled == (to == APP_INPUT_USB));
        input_link_t active = input_link_snapshot(&test_link);
        assert(!active.connected && !active.ready && active.kind == to);
        render_view(&test_link, ESP_OK);
        assert(!test_view.connected && !test_view.ready && test_view.input_transport == to);
        assert(test_view.usb_available && !test_view.usb_connected);
        assert(test_view.bluetooth_available && !test_view.bluetooth_connected);
        assert(strcmp(s_interaction.view.preview, "AB") == 0);
        action(APP_CLOSE, 0);
        action(APP_SEND, 0);
        assert(trace_count == before && !s_send_token);
    }
}

static void inactive_bluetooth_events_do_not_retarget_selected_usb(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    const input_link_t original = input_link_snapshot(&test_link);
    const uint64_t epoch = s_engine.state.epoch;
    action(APP_KEY, 4);
    test_link.generation += 10;
    test_link.connected = test_link.ready = false;
    test_link.failed = test_link.congested = test_link.suspended = true;
    test_link.keyboard_leds = 7;
    test_usb.keyboard_leds = 2;
    const input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(active.kind == original.kind && active.generation == original.generation);
    assert(s_engine.state.epoch == epoch);
    assert(active.connected && active.ready && !active.failed && !active.congested && !active.suspended);
    render_view(&test_link, ESP_FAIL);
    assert(test_view.input_transport == APP_INPUT_USB && test_view.connected && test_view.ready);
    assert(!test_view.transport_error && test_view.keyboard_leds == 2);
    assert(!test_view.bluetooth_available && !test_view.bluetooth_connected);
    const unsigned ble_attempts = test_ble_send_attempts;
    const uint8_t down[] = {0, 0, 4, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    expect_report(1, down, sizeof down);
    expect_report(1, zero, sizeof zero);
    assert(trace[0].transport == APP_INPUT_USB && trace[1].transport == APP_INPUT_USB);
    assert(test_ble_send_attempts == ble_attempts && test_usb_send_attempts == 2);
    assert(test_storage_requests == 0 && test_usb_enable_count == 0);
}

static void transport_replaced_generation_and_refused_release_cannot_replay(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        fill_preview(4);
        action(APP_SEND, 0);
        const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
        expect_report(1, a, sizeof a);
        update_send(false);
        action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0 && !s_send_token);
        if (from == APP_INPUT_USB) test_usb_send_failure = true;
        else test_send_failure = true;
        assert(step() == HID_ENGINE_STEP_SEND_FAILED);
        service_transport_switch();
        assert(s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
        test_usb_send_failure = test_send_failure = false;
        const input_link_t old_snapshot = input_link_snapshot(&test_link);
        if (from == APP_INPUT_USB) ++test_usb.generation;
        else ++test_link.generation;
        static const uint8_t zero[8] = {0};
        uint64_t obsolete = old_snapshot.generation;
        assert(!send_report(&obsolete, 1, zero, 8));
        assert(trace_count == 1);
        const input_link_t replacement = input_link_snapshot(&test_link);
        observe_input_link(&replacement, &test_previous_link);
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
        assert(test_usb_enable_count == 0); /* Connected origin replacement requires fresh intent. */
        action(APP_TRANSPORT_SELECT, (int)to);
        assert(s_transport_switch_pending);
        release_barrier();
        service_transport_switch();
        assert(s_input_transport == to && test_storage_requests == 1);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0 && !s_send_token);
        assert(trace_count == 3 && trace[1].transport == from && trace[2].transport == from);
        acknowledge_selected_transport();
        release_barrier();
        assert(trace_count == 5 && trace[3].transport == to && trace[4].transport == to);
        expect_idle(); /* No discarded suffix can leak to the replacement target. */
    }
}

static void transport_unavailable_targets_never_enable_or_persist(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_MOUSE, from);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            if (to == APP_INPUT_USB) {
                test_usb.initialized = fault != 0;
                test_usb.failed = fault == 1;
            } else {
                test_link.initialized = fault != 0;
                test_link.failed = fault == 1;
            }
            action(APP_TRANSPORT_SELECT, (int)to);
            assert(s_transport_switch_pending == (fault == 2));
            if (fault == 2) {
                if (to == APP_INPUT_USB) test_usb.failed = true;
                else test_link.failed = true;
                service_transport_switch();
            }
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
        }
    }
}

static void wifi_unknown_storage_recovery_binds_complete_confirmation(void)
{
    for (unsigned changed = 0; changed < 6; ++changed) {
        setup(INPUT_MOUSE);
        settings_page(APP_SETTINGS_WIFI);
        test_wifi = (app_wifi_snapshot_t){.initialized = true, .state = APP_WIFI_ERROR,
            .stored_data_known = true, .stored_data_present = true,
            .storage_uncertain = true, .storage_error = ESP_FAIL};
        assert(!test_wifi.configured);
        action(APP_WIFI_FORGET_REQUEST, 0);
        release_barrier();
        assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_WIFI_FORGET);
        if (changed == 0) strcpy(test_wifi.ssid, "TEST-RUNTIME");
        if (changed == 1) strcpy(test_wifi.stored_ssid, "TEST-STORED");
        if (changed == 2) test_wifi.stored_data_known = false;
        if (changed == 3) test_wifi.stored_data_present = false;
        if (changed == 4) test_wifi.storage_uncertain = false;
        test_wifi_forget_error = ESP_OK;
        action(APP_CONFIRM, 1);
        release_barrier();
        assert(test_wifi_forgets == (changed == 5 ? 1U : 0U));
        assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_WIFI);
        assert(test_storage_requests == 0 && test_pair_forgets == 0);
    }
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .state = APP_WIFI_IDLE,
        .storage_uncertain = true, .storage_error = ESP_FAIL};
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_saved_network && !test_view.wifi_operation_pending);
    assert(test_view.wifi_message[0] && !test_view.wifi_ssid[0]);
    action(APP_WIFI_FORGET_REQUEST, 0); /* Unknown presence also permits explicit recovery. */
    release_barrier();
    action(APP_CONFIRM, 1); /* Configurable service rejects the request. */
    release_barrier();
    assert(test_wifi_forgets == 1 && test_wifi.storage_uncertain);
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_saved_network && test_view.wifi_message[0]);
    test_wifi.cleanup_pending = true;
    test_wifi.cleanup_error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_operation_pending && test_view.wifi_message[0]);
    assert(test_view.wifi_setup_passcode[0] == '\0');
}

static void usb_removal_never_falls_back_or_replays_to_bluetooth(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    fill_preview(2);
    action(APP_SEND, 0);
    const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
    expect_report(1, a, sizeof a);
    update_send(false);
    test_usb.mounted = test_usb.ready = false;
    ++test_usb.generation;
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
    service_transport_switch();
    render_view(&test_link, ESP_OK);
    assert(s_input_transport == APP_INPUT_USB && !s_transport_switch_pending);
    assert(!test_view.connected && test_view.bluetooth_connected && test_view.bluetooth_ready);
    assert(strcmp(s_interaction.view.preview, "B") == 0 && !s_send_token);
    assert(test_usb_enable_count == 0 && test_storage_requests == 0 && test_ble_send_attempts == 0);
    action(APP_SEND, 0);
    assert(trace_count == 1 && !s_send_token);
    test_usb.mounted = true;
    ++test_usb.generation;
    acknowledge_selected_transport();
    release_barrier();
    service_transport_switch();
    expect_idle();
    assert(trace_count == 3 && trace[1].transport == APP_INPUT_USB && trace[2].transport == APP_INPUT_USB);
    assert(strcmp(s_interaction.view.preview, "B") == 0 && test_ble_send_attempts == 0);
}

static void audio_mode_remains_unavailable_across_both_transport_choices(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_AUDIO, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        release_barrier();
        service_transport_switch();
        assert(s_input_transport == to && s_engine.state.mode == INPUT_AUDIO);
        acknowledge_selected_transport();
        release_barrier();
        render_view(&test_link, ESP_OK);
        assert(test_view.mode == INPUT_AUDIO && test_view.input_transport == to);
        assert(!test_view.audio_available && !test_view.audio_active && test_view.microphone_muted);
        action(APP_CLOSE, 0);
        action(APP_AUDIO_POWER, 1);
        action(APP_VOICE_START, 1);
        expect_idle();
        assert(test_storage_requests == 1 && !s_engine.state.gyro_enabled);
    }
}

static void usb_selection_rearms_only_a_reset_cleared_selected_adapter(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    for (unsigned i = 0; i < 100; ++i) service_usb_selection();
    assert(test_usb_enable_count == 0 && !test_usb.enabled);
    transport_setup(INPUT_MOUSE, APP_INPUT_USB);
    test_usb.failed = true;
    test_usb.enabled = test_usb.ready = false;
    for (unsigned i = 0; i < 100; ++i) service_usb_selection();
    assert(test_usb_enable_count == 0 && !test_usb.enabled);
    test_usb.initialized = false;
    test_usb.failed = false;
    service_usb_selection();
    assert(test_usb_enable_count == 0);
    /* A synthetic real reset has cleared failure; the selected adapter is
     * initialized again, with a fresh session but no input enable/readiness. */
    test_usb.initialized = true;
    ++test_usb.generation;
    const uint64_t before_enable = test_usb.generation;
    service_usb_selection();
    assert(test_usb_enable_count == 1 && test_usb_enable_requests[0] && test_usb.enabled);
    assert(test_usb.generation > before_enable && !test_usb.ready);
    for (unsigned i = 0; i < 1000; ++i) service_usb_selection();
    assert(test_usb_enable_count == 1 && test_storage_requests == 0);
    assert(s_input_transport == APP_INPUT_USB && trace_count == 0);
}

static void usb_reset_rearm_discards_stale_work_before_fresh_reports(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    fill_preview(4);
    action(APP_SEND, 0);
    const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
    expect_report(1, a, sizeof a);
    update_send(false);
    fill_intents();
    const uint64_t first_epoch = s_engine.state.epoch;
    test_usb.failed = true;
    test_usb.enabled = test_usb.ready = false;
    ++test_usb.generation;
    service_usb_selection();
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 0 && s_engine.state.epoch != first_epoch);
    expect_empty_intents();
    assert(!s_send_token && strcmp(s_interaction.view.preview, "BAB") == 0);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    fill_intents(); /* Old producer work also exists immediately before reset. */
    const uint64_t failed_epoch = s_engine.state.epoch;
    test_usb.failed = false;
    ++test_usb.generation;
    service_usb_selection();
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 1 && test_usb.enabled && !test_usb.ready);
    assert(s_engine.state.epoch != failed_epoch && s_engine.state.release_pending);
    expect_empty_intents();
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    test_usb.ready = true;
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
    release_barrier();
    assert(trace_count == 3 && trace[1].transport == APP_INPUT_USB && trace[2].transport == APP_INPUT_USB);
    expect_idle();
    service_usb_selection();
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 1 && !s_send_token && test_ble_send_attempts == 0);
    assert(strcmp(s_interaction.view.preview, "BAB") == 0);
    action(APP_KEY, 5);
    const uint8_t fresh[] = {0, 0, 5, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    expect_report(1, fresh, sizeof fresh);
    expect_report(1, zero, sizeof zero);
    assert(trace_count == 5 && test_storage_requests == 0);
}

static void mark_ble_admission_in_flight(void) { test_link.congested = true; }
static void mark_usb_admission_in_flight(void) { test_usb.congested = true; }
static void fail_bluetooth_target_after_usb_admission(void)
{
    test_link.failed = true;
    test_link.ready = false;
}

static void final_release_admission_refreshes_transport_completion_evidence(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        static const uint8_t zero[8] = {0};
        expect_report(1, zero, 8);
        const input_link_t before_final_send = input_link_snapshot(&test_link);
        assert(!before_final_send.congested);
        if (from == APP_INPUT_USB) test_usb_after_accept = mark_usb_admission_in_flight;
        else test_ble_after_accept = mark_ble_admission_in_flight;
        expect_report(2, zero, 4);
        assert(!s_engine.state.release_pending);
        assert(from == APP_INPUT_USB ? test_usb.congested : test_link.congested);
        service_transport_switch();
        assert(s_transport_switch_pending && s_input_transport == from);
        assert(test_usb_enable_count == 0 && test_storage_requests == 0);
        if (from == APP_INPUT_USB) test_usb.congested = false;
        else test_link.congested = false;
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == to);
        assert(test_usb_enable_count == 1 && test_storage_requests == 1);
    }
}

static void ble_target_failure_after_final_usb_release_aborts_selection(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_USB);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, APP_INPUT_BLUETOOTH);
    static const uint8_t zero[8] = {0};
    expect_report(1, zero, 8);
    service_transport_switch();
    assert(s_transport_switch_pending); /* Establish a valid origin before injecting target failure. */
    const ble_hid_snapshot_t before_final_send = test_link;
    assert(before_final_send.initialized && !before_final_send.failed);
    test_usb_after_accept = fail_bluetooth_target_after_usb_admission;
    expect_report(2, zero, 4);
    service_transport_switch();
    assert(!s_transport_switch_pending && s_input_transport == APP_INPUT_USB);
    assert(s_requested_transport == APP_INPUT_USB && test_usb.enabled);
    assert(test_usb_enable_count == 0 && test_storage_requests == 0);
}

static void pairing_disconnect_waits_for_fresh_final_release_completion(void)
{
    setup(INPUT_KEYBOARD);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    action(APP_CONFIRM, 1);
    static const uint8_t zero[8] = {0};
    expect_report(1, zero, 8);
    const ble_hid_snapshot_t before_final_send = test_link;
    assert(!before_final_send.congested);
    test_disconnect_accept = true;
    test_ble_after_accept = mark_ble_admission_in_flight;
    expect_report(2, zero, 4);
    assert(!s_engine.state.release_pending && test_link.congested);
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 0 && s_pair_operation == APP_CONFIRM_DISCONNECT);
    test_link.congested = false;
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 1 && test_disconnect_trace_count == trace_count);
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 1 && test_pair_forgets == 0);
}

static void confirmed_disconnect_cannot_retarget_a_replacement_peer(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    action(APP_CONFIRM, 1);
    release_barrier();
    const ble_hid_snapshot_t original = test_link;
    ++test_link.generation;
    test_disconnect_accept = true;
    service_pair_operation(&original);
    assert(test_disconnects == 0); /* Confirmation authorized the original current peer only. */
    assert(s_pair_operation == APP_CONFIRM_NONE);
}

static void origin_disconnect_allows_explicit_switch_without_release_claim(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_MOUSE, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        service_transport_switch();
        assert(s_transport_switch_pending);
        const size_t before = trace_count;
        if (from == APP_INPUT_USB) {
            test_usb.mounted = test_usb.ready = false;
            ++test_usb.generation;
        } else {
            test_link.connected = test_link.ready = false;
            ++test_link.generation;
        }
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == to);
        assert(test_storage_requests == 1 && trace_count == before);
        assert(strstr(s_message, "disconnected") != NULL);
    }
}

static void start_confirmed_pair_change(app_pairing_operation_t operation)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, (int)operation);
    release_barrier();
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(s_pair_operation != APP_CONFIRM_NONE && !s_pair_disconnect_requested);
    test_disconnect_accept = test_pair_accept = true;
}

static void assert_pair_change_completion(app_pairing_operation_t operation, bool completed)
{
    assert(s_pair_operation == APP_CONFIRM_NONE);
    assert(test_pair_opens == (completed && operation == APP_PAIR_OPEN ? 1U : 0U));
    assert(test_pair_forgets == (completed && operation == APP_PAIR_FORGET_ALL ? 1U : 0U));
    assert(test_disconnects == 1 && test_storage_requests == 0 && test_wifi_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
}

static void pairing_admission_generation_change_waits_for_actual_link_down(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_OPEN, APP_PAIR_FORGET_ALL, APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        start_confirmed_pair_change(operations[i]);
        const ble_hid_snapshot_t original = test_link;
        const app_confirmation_t pending = s_pair_operation;
        const size_t reports = trace_count;
        service_pair_operation(&original);
        assert(test_disconnects == 1 && test_disconnect_generation == original.generation);
        assert(test_link.generation == original.generation + 1 && test_link.connected && !test_link.ready);
        assert(s_pair_disconnect_requested && s_pair_operation == pending);
        input_link_t active = input_link_snapshot(&test_link);
        observe_input_link(&active, &test_previous_link);
        assert(!s_engine.ready);
        for (unsigned poll = 0; poll < 50; ++poll) {
            s_now_ms += 10;
            service_pair_operation(&original);
            assert(s_pair_operation == pending && test_disconnects == 1);
            assert(test_pair_opens == 0 && test_pair_forgets == 0);
        }
        assert(trace_count == reports);
        test_link.connected = false;
        ++test_link.generation;
        active = input_link_snapshot(&test_link);
        observe_input_link(&active, &test_previous_link);
        service_pair_operation(&original);
        assert_pair_change_completion(operations[i], true);
        if (operations[i] == APP_PAIR_FORGET_ALL) assert(test_forget_generation == test_link.generation);
        service_pair_operation(&original);
        assert_pair_change_completion(operations[i], true);
    }
}

static void pairing_replacement_before_acceptance_aborts_all_operation_kinds(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_OPEN, APP_PAIR_FORGET_ALL, APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        start_confirmed_pair_change(operations[i]);
        const ble_hid_snapshot_t original = test_link;
        ++test_link.generation;
        service_pair_operation(&original);
        assert(s_pair_operation == APP_CONFIRM_NONE && !s_pair_disconnect_requested);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_pair_forgets == 0);
        test_link.connected = test_link.ready = false;
        ++test_link.generation;
        service_pair_operation(&original);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_pair_forgets == 0);
        assert(test_storage_requests == 0 && strcmp(s_interaction.view.preview, "AB") == 0);
    }
}

static void pairing_replacement_after_acceptance_never_issues_another_disconnect(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_OPEN, APP_PAIR_FORGET_ALL, APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        for (unsigned link_down = 0; link_down < 2; ++link_down) {
            start_confirmed_pair_change(operations[i]);
            const ble_hid_snapshot_t original = test_link;
            const app_confirmation_t pending = s_pair_operation;
            service_pair_operation(&original);
            assert(s_pair_disconnect_requested && test_disconnects == 1);
            ++test_link.generation; /* Replacement beyond our own admission-induced increment. */
            test_link.ready = true;
            test_link.peer_known = true;
            test_link.peer_address[0] = 2;
            test_link.peer_address[5] = 99;
            input_link_t active = input_link_snapshot(&test_link);
            observe_input_link(&active, &test_previous_link);
            s_now_ms = 5999;
            service_pair_operation(&original);
            assert(s_pair_operation == pending && test_disconnects == 1);
            assert(test_pair_opens == 0 && test_pair_forgets == 0);
            if (link_down) {
                test_link.connected = test_link.ready = false;
                ++test_link.generation;
            } else {
                s_now_ms = 6000;
            }
            service_pair_operation(&original);
            assert_pair_change_completion(operations[i], link_down != 0);
            test_link.connected = test_link.ready = false;
            ++test_link.generation;
            service_pair_operation(&original);
            assert_pair_change_completion(operations[i], link_down != 0);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    unsigned passed = 0;
#define RUN(test) do { if (argc == 1 || strcmp(argv[1], #test) == 0) { \
    test_case = #test; test(); ++passed; } } while (0)
    RUN(saturated_emergency_admission);
    RUN(stale_ui_epoch);
    RUN(touch_cannot_unlock);
    RUN(physical_hold_toggle);
    RUN(simultaneous_lock_wins);
    RUN(lock_releases_before_later_work);
    RUN(stale_context_and_modal_focus);
    RUN(preview_editing_and_full_send);
    RUN(interrupted_send_retains_only_unaccepted_suffix);
    RUN(touch_precision_and_drag_boundaries);
    RUN(raw_button_timing_and_takeover);
    RUN(motion_gates_and_local_keyboard_focus);
    RUN(rendered_state_is_truthful);
    RUN(touch_lifecycle_rejects_ended_gesture);
    RUN(touch_release_has_one_freeze_interval);
    RUN(physical_down_freezes_before_classification);
    RUN(same_or_invalid_mode_preserves_active_send);
    RUN(short_congestion_preserves_preview_send);
    RUN(short_congestion_preserves_motion_intent);
    RUN(modal_button_press_is_not_retargeted_after_close);
    RUN(local_touch_takeover_consumes_its_gesture);
    RUN(preview_survives_mode_modal_and_link_cancellation);
    RUN(physical_confirm_uses_visible_keyboard_submode);
    RUN(shortcuts_use_selected_host_modifier_and_preserve_preview);
    RUN(lost_button_edges_cannot_unlock);
    RUN(theme_changes_are_settings_scoped);
    RUN(queued_double_click_then_hold_keeps_edge_time);
    RUN(completed_quick_touch_still_freezes_motion);
    RUN(settings_pages_validate_and_persist_changes);
    RUN(idle_deadlines_ignore_passive_status_updates);
    RUN(display_sleep_preserves_hid_work_and_draft);
    RUN(touch_wake_consumes_entire_gesture_until_driver_ack);
    RUN(ai_wake_double_and_hold_are_consumed);
    RUN(display_activity_clock_and_optional_driver);
    RUN(timeout_changes_start_a_full_new_interval);
    RUN(failed_wake_is_blocked_until_a_fresh_acknowledged_gesture);
    RUN(wifi_setup_lifecycle_and_secret_scope);
    RUN(wifi_forget_requires_current_explicit_confirmation);
    RUN(pairing_confirmation_releases_before_disconnect_and_forget);
    RUN(pairing_stale_generation_failure_and_lock_cancel);
    RUN(extended_status_views_do_not_invent_evidence_or_writes);
    RUN(all_settings_pages_own_focus_in_every_mode);
    RUN(physical_modal_exit_stops_wifi_setup);
    RUN(wake_gesture_cannot_enter_the_priority_lock_path);
    RUN(release_priority_stops_wifi_setup);
    RUN(priority_reset_reconciles_a_queued_touch_release);
    RUN(overflow_reconciles_a_captured_wake_touch_release);
    RUN(fresh_touch_lock_survives_ai_wake_guard);
    RUN(fresh_touch_release_survives_ai_wake_guard);
    RUN(bluetooth_bond_views_preserve_unknown_and_verified_identity);
    RUN(bluetooth_pagination_is_bounded_and_contextual);
    RUN(disconnect_current_host_preserves_saved_pairings);
    RUN(saturated_queue_preserves_real_touch_lock);
    RUN(nine_cell_grid_reaches_every_group_and_rejects_empty_children);
    RUN(saturated_queue_rejects_consumed_wake_emergencies);
    RUN(saturated_emergency_waits_for_its_queued_begin);
    RUN(transport_switch_waits_for_both_releases_and_completion);
    RUN(transport_selection_is_page_scoped_and_failure_visible);
    RUN(transport_blocked_old_host_times_out_without_persisting);
    RUN(transport_lock_or_release_cancels_late_switch_completion);
    RUN(disconnected_old_transport_allows_explicit_offline_selection);
    RUN(inactive_bluetooth_events_do_not_retarget_selected_usb);
    RUN(transport_replaced_generation_and_refused_release_cannot_replay);
    RUN(transport_unavailable_targets_never_enable_or_persist);
    RUN(wifi_unknown_storage_recovery_binds_complete_confirmation);
    RUN(usb_removal_never_falls_back_or_replays_to_bluetooth);
    RUN(audio_mode_remains_unavailable_across_both_transport_choices);
    RUN(usb_selection_rearms_only_a_reset_cleared_selected_adapter);
    RUN(usb_reset_rearm_discards_stale_work_before_fresh_reports);
    RUN(final_release_admission_refreshes_transport_completion_evidence);
    RUN(ble_target_failure_after_final_usb_release_aborts_selection);
    RUN(pairing_disconnect_waits_for_fresh_final_release_completion);
    RUN(confirmed_disconnect_cannot_retarget_a_replacement_peer);
    RUN(origin_disconnect_allows_explicit_switch_without_release_claim);
    RUN(pairing_admission_generation_change_waits_for_actual_link_down);
    RUN(pairing_replacement_before_acceptance_aborts_all_operation_kinds);
    RUN(pairing_replacement_after_acceptance_never_issues_another_disconnect);
#undef RUN
    assert(passed);
    if (argc == 2) printf("PASS: coordinator case %s\n", argv[1]);
    else printf("PASS: %u production coordinator cases covering safety, preview/send, touch/drag, "
                "physical timing, motion, settings, display/wake, connectivity and views\n", passed);
    return 0;
}
