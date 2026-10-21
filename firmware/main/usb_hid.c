// SPDX-License-Identifier: Apache-2.0
#include "usb_hid.h"
#include "usb_console.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_engine.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#if CFG_TUD_HID != 1 || CFG_TUD_CDC != 1
#error "ESP32-INPUT requires one HID interface and one CDC function in one TinyUSB stack"
#endif

enum { USB_SLOT_TIMEOUT_US = 100000, USB_MONITOR_US = 10000, USB_RETRY_US = 2000 };

typedef struct {
    uint64_t generation;
    uint64_t sequence;
} usb_dispatch_token_t;

static portMUX_TYPE s_usb_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_hid_snapshot_t s_usb;
static bool s_prepared;
static bool s_endpoint_ready;
static bool s_sync_needed;
static bool s_queued;
static bool s_inflight;
static bool s_dispatching;
static bool s_posted;
static uint8_t s_wire[1 + HID_ENGINE_KEYBOARD_REPORT_LENGTH];
static uint16_t s_wire_length;
static uint64_t s_report_generation;
static uint64_t s_admitted_us;
static uint64_t s_posted_us;
static uint64_t s_retry_after_us;
static usb_dispatch_token_t s_token;
static TaskHandle_t s_usb_worker;
static esp_timer_handle_t s_usb_monitor;
static uint8_t s_keyboard[HID_ENGINE_KEYBOARD_REPORT_LENGTH];
static uint8_t s_mouse_buttons;

static bool ready_locked(void)
{
    return s_usb.initialized && s_usb.mounted && !s_usb.suspended && s_usb.enabled && !s_usb.failed;
}

static void clear_reports_locked(void)
{
    memset(s_keyboard, 0, sizeof(s_keyboard));
    s_mouse_buttons = 0;
    s_usb.keyboard_leds = 0;
}

static void invalidate_locked(void)
{
    s_queued = false;
    clear_reports_locked();
    if (s_usb.generation == UINT64_MAX) {
        s_usb.failed = true;
        s_usb.error = ESP_ERR_INVALID_STATE;
        s_usb.enabled = false;
    } else {
        ++s_usb.generation;
    }
}

static void fault_locked(esp_err_t error)
{
    if (!s_usb.failed) {
        invalidate_locked();
    }
    s_usb.failed = true;
    s_usb.error = error;
    s_usb.enabled = false;
    s_endpoint_ready = false;
    /* Retain ownership of an outstanding transfer until its actual completion
     * or the stack reset barrier. Never attribute a late callback to new work. */
}

static void monitor_deadlines(void *argument)
{
    (void)argument;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_usb_lock);
    if (s_usb.initialized && !s_usb.failed) {
        const bool report_expired = (s_queued || s_inflight || s_dispatching) &&
            (now_us < s_admitted_us || now_us - s_admitted_us >= USB_SLOT_TIMEOUT_US);
        const bool dispatch_expired = s_posted &&
            (now_us < s_posted_us || now_us - s_posted_us >= USB_SLOT_TIMEOUT_US);
        if (report_expired || dispatch_expired) {
            fault_locked(ESP_ERR_TIMEOUT);
        }
    }
    portEXIT_CRITICAL(&s_usb_lock);
}

static void usb_dispatch(void *argument)
{
    const usb_dispatch_token_t token = *(const usb_dispatch_token_t *)argument;
    portENTER_CRITICAL(&s_usb_lock);
    if (!s_posted || token.sequence != s_token.sequence) {
        portEXIT_CRITICAL(&s_usb_lock);
        return;
    }
    s_posted = false;
    if (token.generation != s_usb.generation || !s_usb.initialized || s_usb.failed) {
        portEXIT_CRITICAL(&s_usb_lock);
        return;
    }
    if (s_sync_needed) {
        /* This runs on the USB task after earlier lifecycle/reset events. Read
         * only; no HID endpoint submission occurs during synchronization. */
        const bool mounted = tud_mounted();
        const bool suspended = tud_suspended();
        if (mounted != s_usb.mounted || suspended != s_usb.suspended) {
            invalidate_locked();
            s_usb.mounted = mounted;
            s_usb.suspended = suspended;
        }
        s_endpoint_ready = mounted && !suspended && tud_hid_n_ready(0);
        s_sync_needed = false;
        portEXIT_CRITICAL(&s_usb_lock);
        return;
    }
    if (!s_queued || s_report_generation != s_usb.generation || !ready_locked()) {
        s_queued = false;
        portEXIT_CRITICAL(&s_usb_lock);
        return;
    }
    if (!tud_hid_n_ready(0)) {
        s_endpoint_ready = false;
        s_retry_after_us = (uint64_t)esp_timer_get_time() + USB_RETRY_US;
        portEXIT_CRITICAL(&s_usb_lock);
        return;
    }
    /* The USB task serializes this operation with all class/lifecycle
     * callbacks. A concurrent coordinator disable quarantines dispatching,
     * rather than pretending the committed operation can be recalled. */
    s_queued = false;
    s_inflight = true;
    s_dispatching = true;
    s_endpoint_ready = false;
    const uint8_t report_id = s_wire[0];
    const uint16_t length = (uint16_t)(s_wire_length - 1);
    portEXIT_CRITICAL(&s_usb_lock);

    const bool accepted = tud_hid_n_report(0, report_id, s_wire + 1, length);

    portENTER_CRITICAL(&s_usb_lock);
    s_dispatching = false;
    if (accepted) {
        if (s_usb.submitted != UINT64_MAX) {
            ++s_usb.submitted;
        }
    } else {
        s_inflight = false;
        fault_locked(ESP_FAIL);
    }
    portEXIT_CRITICAL(&s_usb_lock);
}

static bool post_dispatch(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_usb_lock);
    bool post = s_usb.initialized && !s_usb.failed && !s_posted && !s_inflight &&
        !s_dispatching && (s_sync_needed || (s_queued && now_us >= s_retry_after_us));
    if (post && s_token.sequence == UINT64_MAX) {
        fault_locked(ESP_ERR_INVALID_STATE);
        post = false;
    }
    if (post) {
        s_token.generation = s_usb.generation;
        ++s_token.sequence;
        s_posted = true;
        s_posted_us = now_us;
    }
    portEXIT_CRITICAL(&s_usb_lock);
    if (post) {
        /* Pinned TinyUSB can wait on its private event queue. Only this worker
         * calls the defer API. Coordinator admission never enters that queue,
         * and the independent monitor still revokes a stalled dispatch. */
        usbd_defer_func(usb_dispatch, &s_token, false);
    }
    return post;
}

static void usb_worker(void *argument)
{
    (void)argument;
    TickType_t ticks = pdMS_TO_TICKS(2);
    if (!ticks) {
        ticks = 1;
    }
    for (;;) {
        (void)post_dispatch();
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
}

esp_err_t usb_hid_prepare(void)
{
    if (s_prepared) {
        return s_usb.failed ? s_usb.error : ESP_OK;
    }
    s_prepared = true;
    const esp_timer_create_args_t timer = {
        .callback = monitor_deadlines,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "usb_hid_deadline",
        .skip_unhandled_events = true,
    };
    esp_err_t error = esp_timer_create(&timer, &s_usb_monitor);
    if (error == ESP_OK) {
        error = esp_timer_start_periodic(s_usb_monitor, USB_MONITOR_US);
    }
    if (error == ESP_OK && xTaskCreate(usb_worker, "usb_hid", 3072, NULL,
                                     tskIDLE_PRIORITY + 1, &s_usb_worker) != pdPASS) {
        error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_usb_lock);
        fault_locked(error);
        portEXIT_CRITICAL(&s_usb_lock);
    }
    return error;
}

esp_err_t usb_hid_init(void)
{
    return usb_console_start();
}

void usb_hid_stack_started(esp_err_t error)
{
    portENTER_CRITICAL(&s_usb_lock);
    if (error == ESP_OK && !s_usb.failed) {
        s_usb.initialized = true;
        s_sync_needed = true;
    } else {
        fault_locked(error == ESP_OK ? ESP_FAIL : error);
    }
    portEXIT_CRITICAL(&s_usb_lock);
    if (s_usb_worker) {
        xTaskNotifyGive(s_usb_worker);
    }
}

void usb_hid_stack_event(bool mounted, bool suspended)
{
    portENTER_CRITICAL(&s_usb_lock);
    const bool changed = mounted != s_usb.mounted || suspended != s_usb.suspended;
    if (changed) {
        const bool uncertain = s_inflight || s_dispatching;
        invalidate_locked();
        if (suspended && uncertain) {
            fault_locked(ESP_ERR_INVALID_STATE);
        }
        s_usb.mounted = mounted;
        s_usb.suspended = mounted && suspended;
    }
    s_endpoint_ready = mounted && !suspended && !s_usb.failed && tud_hid_n_ready(0);
    portEXIT_CRITICAL(&s_usb_lock);
}

usb_hid_snapshot_t usb_hid_snapshot(void)
{
    portENTER_CRITICAL(&s_usb_lock);
    usb_hid_snapshot_t snapshot = s_usb;
    snapshot.ready = ready_locked();
    snapshot.congested = s_queued || s_inflight || s_dispatching || s_posted ||
        (snapshot.ready && !s_endpoint_ready);
    portEXIT_CRITICAL(&s_usb_lock);
    return snapshot;
}

void usb_hid_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_usb_lock);
    if (s_usb.enabled != enabled && (!enabled || !s_usb.failed)) {
        const bool uncertain = s_inflight || s_dispatching;
        invalidate_locked();
        s_usb.enabled = enabled && !s_usb.failed;
        if (!enabled && uncertain) {
            fault_locked(ESP_ERR_INVALID_STATE);
        }
        s_sync_needed = s_usb.initialized && !s_usb.failed;
        s_endpoint_ready = false;
    }
    portEXIT_CRITICAL(&s_usb_lock);
    if (s_usb_worker) {
        xTaskNotifyGive(s_usb_worker);
    }
}

static bool report_valid(uint8_t id, const uint8_t *data, size_t length)
{
    if (!data) {
        return false;
    }
    if (id == HID_ENGINE_KEYBOARD_REPORT_ID && length == HID_ENGINE_KEYBOARD_REPORT_LENGTH) {
        if (data[1]) {
            return false;
        }
        for (size_t i = 2; i < length; ++i) {
            if (data[i] > HID_ENGINE_KEY_USAGE_MAX || (data[i] && data[i] < 4)) {
                return false;
            }
        }
        return true;
    }
    if (id == HID_ENGINE_MOUSE_REPORT_ID && length == HID_ENGINE_MOUSE_REPORT_LENGTH) {
        return !(data[0] & ~HID_ENGINE_MOUSE_BUTTON_MASK) && data[1] != 0x80 &&
            data[2] != 0x80 && data[3] != 0x80;
    }
    return false;
}

bool usb_hid_send_generation(uint64_t generation, uint8_t id, const uint8_t *data, size_t length)
{
    if (!report_valid(id, data, length)) {
        return false;
    }
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_usb_lock);
    bool accepted = ready_locked() && s_endpoint_ready && generation == s_usb.generation &&
        !s_queued && !s_inflight && !s_dispatching && !s_posted && !s_sync_needed;
    if (accepted && s_usb.accepted == UINT64_MAX) {
        fault_locked(ESP_ERR_INVALID_STATE);
        accepted = false;
    }
    if (accepted) {
        s_wire[0] = id;
        memcpy(s_wire + 1, data, length);
        s_wire_length = (uint16_t)(length + 1);
        s_report_generation = generation;
        s_admitted_us = now_us;
        s_retry_after_us = 0;
        s_queued = true;
        ++s_usb.accepted;
    }
    portEXIT_CRITICAL(&s_usb_lock);
    if (accepted) {
        xTaskNotifyGive(s_usb_worker);
    }
    return accepted;
}

bool usb_hid_recover_generation(uint64_t generation)
{
    portENTER_CRITICAL(&s_usb_lock);
    const bool accepted = s_usb.initialized && s_usb.mounted && !s_usb.failed &&
        generation == s_usb.generation;
    if (accepted) {
        fault_locked(ESP_ERR_INVALID_STATE);
    }
    portEXIT_CRITICAL(&s_usb_lock);
    return accepted;
}

/* Non-claiming observer: TinyUSB invokes class reset even for bus reset that
 * does not emit tud_umount_cb. The same USB task finishes resetting HID before
 * it can process another deferred dispatch. No extra interface is advertised. */
static void observer_init(void) {}
static void observer_reset(uint8_t rhport)
{
    (void)rhport;
    portENTER_CRITICAL(&s_usb_lock);
    invalidate_locked();
    s_usb.mounted = false;
    s_usb.suspended = false;
    if (s_usb.initialized && s_usb.generation != UINT64_MAX) {
        s_usb.failed = false;
        s_usb.error = ESP_OK;
    }
    s_inflight = false;
    s_dispatching = false;
    s_endpoint_ready = false;
    /* Keep an already-posted defer token alive until its callback drains. */
    portEXIT_CRITICAL(&s_usb_lock);
}

static uint16_t observer_open(uint8_t rhport, const tusb_desc_interface_t *interface, uint16_t length)
{
    (void)rhport;
    (void)interface;
    (void)length;
    return 0;
}

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
    static const usbd_class_driver_t observer = {
        .name = "input_lifecycle", .init = observer_init, .reset = observer_reset,
        .open = observer_open,
    };
    *count = 1;
    return &observer;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == 0 ? hid_engine_report_descriptor : NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
    uint8_t *buffer, uint16_t requested)
{
    if (instance || !buffer || !requested) {
        return 0;
    }
    uint8_t report[HID_ENGINE_KEYBOARD_REPORT_LENGTH] = {0};
    uint16_t length = 0;
    portENTER_CRITICAL(&s_usb_lock);
    if (type == HID_REPORT_TYPE_INPUT && id == HID_ENGINE_KEYBOARD_REPORT_ID) {
        length = HID_ENGINE_KEYBOARD_REPORT_LENGTH;
        if (ready_locked()) {
            memcpy(report, s_keyboard, sizeof(report));
        }
    } else if (type == HID_REPORT_TYPE_INPUT && id == HID_ENGINE_MOUSE_REPORT_ID) {
        length = HID_ENGINE_MOUSE_REPORT_LENGTH;
        report[0] = ready_locked() ? s_mouse_buttons : 0;
    } else if (type == HID_REPORT_TYPE_OUTPUT && id == HID_ENGINE_KEYBOARD_REPORT_ID) {
        length = 1;
        report[0] = s_usb.mounted && !s_usb.failed ? s_usb.keyboard_leds : 0;
    }
    portEXIT_CRITICAL(&s_usb_lock);
    if (length > requested) {
        length = requested;
    }
    memcpy(buffer, report, length);
    return length;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
    uint8_t const *buffer, uint16_t length)
{
    if (instance || type != HID_REPORT_TYPE_OUTPUT || !buffer) {
        return;
    }
    if (id == 0 && length == 2 && buffer[0] == HID_ENGINE_KEYBOARD_REPORT_ID) {
        id = buffer[0];
        ++buffer;
        --length;
    }
    if (id != HID_ENGINE_KEYBOARD_REPORT_ID || length != HID_ENGINE_KEYBOARD_LED_LENGTH) {
        return;
    }
    portENTER_CRITICAL(&s_usb_lock);
    if (s_usb.mounted && !s_usb.suspended && !s_usb.failed) {
        s_usb.keyboard_leds = buffer[0] & 0x1f;
    }
    portEXIT_CRITICAL(&s_usb_lock);
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    if (!instance && protocol != HID_PROTOCOL_REPORT) {
        portENTER_CRITICAL(&s_usb_lock);
        fault_locked(ESP_ERR_NOT_SUPPORTED);
        portEXIT_CRITICAL(&s_usb_lock);
    }
}

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate)
{
    return instance == 0 && idle_rate == 0;
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t length)
{
    if (instance) {
        return;
    }
    portENTER_CRITICAL(&s_usb_lock);
    if (s_inflight) {
        if (!report || length != s_wire_length || memcmp(report, s_wire, length) != 0) {
            fault_locked(ESP_ERR_INVALID_RESPONSE);
        } else {
            s_inflight = false;
            if (s_usb.completed != UINT64_MAX) {
                ++s_usb.completed;
            }
            if (s_report_generation == s_usb.generation && ready_locked()) {
                if (report[0] == HID_ENGINE_KEYBOARD_REPORT_ID) {
                    memcpy(s_keyboard, report + 1, sizeof(s_keyboard));
                } else {
                    s_mouse_buttons = report[1] & HID_ENGINE_MOUSE_BUTTON_MASK;
                }
                s_endpoint_ready = tud_hid_n_ready(0);
            }
        }
    }
    portEXIT_CRITICAL(&s_usb_lock);
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type,
    uint8_t const *report, uint16_t transferred)
{
    (void)report;
    (void)transferred;
    if (instance || type != HID_REPORT_TYPE_INPUT) {
        return;
    }
    portENTER_CRITICAL(&s_usb_lock);
    if (s_inflight) {
        fault_locked(ESP_FAIL);
    }
    portEXIT_CRITICAL(&s_usb_lock);
}
