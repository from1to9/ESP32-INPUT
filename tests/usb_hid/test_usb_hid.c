// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Production sources; public TinyUSB types/descriptor macros are the real
 * pinned headers. Only hardware/RTOS/SDK actions below that boundary are mocked. */
#include "../../firmware/main/usb_hid.c"
#include "../../firmware/main/usb_console.c"

static struct {
    uint64_t now_us;
    unsigned lock_depth;
    bool usb_task;
    bool mounted, suspended, endpoint_ready, reject_report;
    esp_err_t timer_create_error, timer_start_error, install_error, cdc_error, console_error;
    bool fail_worker;
    unsigned timers, workers, installs, cdc_inits, console_inits, notifications, submits;
    esp_timer_create_args_t timer;
    tinyusb_config_t driver;
    void (*deferred)(void *argument);
    void *deferred_argument;
    uint8_t submitted_wire[9];
    uint16_t submitted_length;
    void (*during_submit)(void);
    void (*during_install)(void);
} hw;
static int worker_handle, timer_handle;

void usb_test_enter(portMUX_TYPE *lock)
{
    (void)lock;
    assert(hw.lock_depth == 0);
    hw.lock_depth = 1;
}

void usb_test_leave(portMUX_TYPE *lock)
{
    (void)lock;
    assert(hw.lock_depth == 1);
    hw.lock_depth = 0;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)hw.now_us;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *handle)
{
    assert(hw.lock_depth == 0 && config->dispatch_method == ESP_TIMER_TASK);
    assert(config->skip_unhandled_events);
    ++hw.timers;
    hw.timer = *config;
    if (hw.timer_create_error == ESP_OK) {
        *handle = &timer_handle;
    }
    return hw.timer_create_error;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us)
{
    assert(hw.lock_depth == 0 && handle == &timer_handle && period_us == 10000);
    return hw.timer_start_error;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(function == usb_worker && strcmp(name, "usb_hid") == 0);
    assert(hw.lock_depth == 0 && stack == 3072 && priority == 1 && argument == NULL);
    ++hw.workers;
    if (hw.fail_worker) {
        return 0;
    }
    *handle = &worker_handle;
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t handle)
{
    assert(handle == &worker_handle && hw.lock_depth == 0);
    ++hw.notifications;
}

unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear;
    (void)timeout;
    assert(!"The test drives the actual worker's bounded post operation directly");
    return 0;
}

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    assert(hw.lock_depth == 0 && config == &s_driver);
    assert(config->event_cb && config->descriptor.full_speed_config && config->descriptor.high_speed_config);
    assert(!config->descriptor.device && !config->descriptor.qualifier && !config->descriptor.string);
    ++hw.installs;
    hw.driver = *config;
    uint8_t count = 0;
    const usbd_class_driver_t *observer = usbd_app_driver_get_cb(&count);
    assert(count == 1 && observer && observer->init && observer->reset);
    const tusb_desc_interface_t interface = {0};
    assert(observer->open(0, &interface, sizeof(interface)) == 0);
    observer->init();
    if (hw.during_install) {
        hw.during_install();
    }
    return hw.install_error;
}

esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config)
{
    assert(config && hw.lock_depth == 0);
    ++hw.cdc_inits;
    return hw.cdc_error;
}

esp_err_t tinyusb_console_init(int interface)
{
    assert(interface == TINYUSB_CDC_ACM_0 && hw.lock_depth == 0);
    ++hw.console_inits;
    return hw.console_error;
}

bool tud_mounted(void)
{
    return hw.mounted;
}

bool tud_suspended(void)
{
    return hw.suspended;
}

bool tud_hid_n_ready(uint8_t instance)
{
    assert(instance == 0);
    return hw.mounted && !hw.suspended && hw.endpoint_ready;
}

bool tud_hid_n_report(uint8_t instance, uint8_t id, const void *report, uint16_t length)
{
    assert(hw.usb_task && hw.lock_depth == 0 && instance == 0);
    assert(tud_hid_n_ready(0) && length <= 8);
    ++hw.submits;
    if (hw.during_submit) {
        void (*hook)(void) = hw.during_submit;
        hw.during_submit = NULL;
        hook();
    }
    if (hw.reject_report) {
        return false;
    }
    hw.submitted_wire[0] = id;
    memcpy(hw.submitted_wire + 1, report, length);
    hw.submitted_length = (uint16_t)(length + 1);
    hw.endpoint_ready = false;
    return true;
}

void usbd_defer_func(osal_task_func_t function, void *argument, bool in_isr)
{
    assert(!hw.usb_task && hw.lock_depth == 0 && !in_isr && !hw.deferred);
    hw.deferred = function;
    hw.deferred_argument = argument;
}

static void reset_fixture(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.now_us = 1000;
    memset(&s_usb, 0, sizeof(s_usb));
    s_prepared = s_endpoint_ready = s_sync_needed = false;
    s_queued = s_inflight = s_dispatching = s_posted = false;
    memset(s_wire, 0, sizeof(s_wire));
    s_wire_length = 0;
    s_report_generation = s_admitted_us = s_posted_us = s_retry_after_us = 0;
    memset(&s_token, 0, sizeof(s_token));
    s_usb_worker = NULL;
    s_usb_monitor = NULL;
    memset(s_keyboard, 0, sizeof(s_keyboard));
    s_mouse_buttons = 0;
    memset(&s_driver, 0, sizeof(s_driver));
    s_start_attempted = false;
    s_start_result = ESP_ERR_INVALID_STATE;
}

static void run_deferred(void)
{
    assert(hw.deferred && !hw.usb_task);
    void (*function)(void *) = hw.deferred;
    void *argument = hw.deferred_argument;
    hw.deferred = NULL;
    hw.deferred_argument = NULL;
    hw.usb_task = true;
    function(argument);
    hw.usb_task = false;
}

static void synchronize(void)
{
    assert(post_dispatch());
    run_deferred();
}

static void device_event(tinyusb_event_id_t id)
{
    tinyusb_event_t event = {.id = id};
    assert(hw.driver.event_cb);
    hw.usb_task = true;
    hw.driver.event_cb(&event, hw.driver.event_arg);
    hw.usb_task = false;
}

static void stack_reset(void)
{
    uint8_t count;
    const usbd_class_driver_t *observer = usbd_app_driver_get_cb(&count);
    assert(count == 1);
    hw.usb_task = true;
    observer->reset(0);
    hw.usb_task = false;
    hw.mounted = hw.suspended = hw.endpoint_ready = false;
}

static void mount_device(void)
{
    hw.mounted = hw.endpoint_ready = true;
    hw.suspended = false;
    device_event(TINYUSB_EVENT_ATTACHED);
}

static void start_ready(void)
{
    reset_fixture();
    assert(usb_hid_init() == ESP_OK);
    assert(usb_console_start() == ESP_OK && hw.installs == 1);
    synchronize();
    mount_device();
    assert(!usb_hid_snapshot().ready && !usb_hid_snapshot().enabled);
    usb_hid_set_enabled(true);
    synchronize();
    const usb_hid_snapshot_t snapshot = usb_hid_snapshot();
    assert(snapshot.initialized && snapshot.mounted && snapshot.enabled && snapshot.ready);
    assert(!snapshot.failed && !snapshot.suspended && !snapshot.congested);
}

static bool send_keyboard(uint8_t usage)
{
    const uint8_t report[8] = {0, 0, usage};
    return usb_hid_send_generation(usb_hid_snapshot().generation, 1, report, sizeof(report));
}

static void complete_report(void)
{
    assert(hw.submitted_length && !hw.endpoint_ready);
    hw.endpoint_ready = true; /* TinyUSB clears BUSY before its class callback. */
    hw.usb_task = true;
    tud_hid_report_complete_cb(0, hw.submitted_wire, hw.submitted_length);
    hw.usb_task = false;
}

static void test_admission_submission_and_completion(void)
{
    start_ready();
    const uint64_t generation = usb_hid_snapshot().generation;
    uint8_t report[8] = {2, 0, 4};
    assert(usb_hid_send_generation(generation, 1, report, 8));
    assert(hw.submits == 0 && !hw.deferred); /* Admission enters no TinyUSB locks/queue. */
    report[2] = 5;
    usb_hid_snapshot_t snapshot = usb_hid_snapshot();
    assert(snapshot.accepted == 1 && snapshot.submitted == 0 && snapshot.completed == 0);
    assert(snapshot.congested && !usb_hid_send_generation(generation, 1, report, 8));
    assert(post_dispatch() && !post_dispatch());
    assert(hw.submits == 0);
    run_deferred();
    snapshot = usb_hid_snapshot();
    assert(snapshot.submitted == 1 && snapshot.completed == 0 && snapshot.congested);
    assert(hw.submitted_length == 9 && hw.submitted_wire[0] == 1 && hw.submitted_wire[3] == 4);
    complete_report();
    snapshot = usb_hid_snapshot();
    assert(snapshot.completed == 1 && !snapshot.congested && snapshot.ready);
    assert(!usb_hid_send_generation(generation - 1, 1, report, 8));
    assert(hw.installs == 1 && hw.cdc_inits == 1 && hw.console_inits == 1);
}

static void test_invalid_reports_and_control_bounds(void)
{
    start_ready();
    const uint64_t generation = usb_hid_snapshot().generation;
    uint8_t keyboard[9] = {0};
    assert(!usb_hid_send_generation(generation, 1, NULL, 8));
    assert(!usb_hid_send_generation(generation, 1, keyboard, 7));
    assert(!usb_hid_send_generation(generation, 1, keyboard, 9));
    assert(!usb_hid_send_generation(generation, 3, keyboard, 8));
    keyboard[1] = 1;
    assert(!usb_hid_send_generation(generation, 1, keyboard, 8));
    keyboard[1] = 0;
    keyboard[2] = 1;
    assert(!usb_hid_send_generation(generation, 1, keyboard, 8));
    keyboard[2] = 0x66;
    assert(!usb_hid_send_generation(generation, 1, keyboard, 8));
    uint8_t mouse[4] = {8, 0, 0, 0};
    assert(!usb_hid_send_generation(generation, 2, mouse, 4));
    mouse[0] = 0;
    for (unsigned i = 1; i < 4; ++i) {
        mouse[i] = 0x80;
        assert(!usb_hid_send_generation(generation, 2, mouse, 4));
        mouse[i] = 0;
    }
    assert(hw.submits == 0 && usb_hid_snapshot().accepted == 0);
    assert(tud_hid_descriptor_report_cb(0) == hid_engine_report_descriptor);
    assert(!tud_hid_descriptor_report_cb(1));

    assert(send_keyboard(4));
    synchronize();
    complete_report();
    for (uint16_t capacity = 1; capacity <= 12; ++capacity) {
        uint8_t output[16];
        memset(output, 0xa5, sizeof(output));
        const uint16_t length = tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, output, capacity);
        assert(length == (capacity < 8 ? capacity : 8));
        if (length >= 3) {
            assert(output[2] == 4);
        }
        for (size_t i = length; i < sizeof(output); ++i) {
            assert(output[i] == 0xa5);
        }
    }
    uint8_t output[16] = {0};
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, NULL, 8) == 0);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, output, 0) == 0);
    assert(tud_hid_get_report_cb(1, 1, HID_REPORT_TYPE_INPUT, output, 8) == 0);
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, output, 8) == 0);
    assert(tud_hid_get_report_cb(0, 3, HID_REPORT_TYPE_INPUT, output, 8) == 0);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_FEATURE, output, 8) == 0);

    const uint8_t movement[] = {1, 55, (uint8_t)-13, 2};
    assert(usb_hid_send_generation(generation, 2, movement, sizeof(movement)));
    synchronize();
    complete_report();
    memset(output, 0xff, sizeof(output));
    assert(tud_hid_get_report_cb(0, 2, HID_REPORT_TYPE_INPUT, output, 4) == 4);
    assert(output[0] == 1 && output[1] == 0 && output[2] == 0 && output[3] == 0);
    usb_hid_set_enabled(false);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, output, 8) == 8);
    for (unsigned i = 0; i < 8; ++i) {
        assert(output[i] == 0);
    }
}

static void test_keyboard_led_output_and_report_protocol(void)
{
    start_ready();
    const uint8_t led[] = {0xe3};
    tud_hid_set_report_cb(0, 1, HID_REPORT_TYPE_OUTPUT, led, 1);
    assert(usb_hid_snapshot().keyboard_leds == 3);
    uint8_t output = 0;
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_OUTPUT, &output, 1) == 1 && output == 3);
    const uint8_t framed[] = {1, 0x1a};
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, framed, 2);
    assert(usb_hid_snapshot().keyboard_leds == 0x1a);
    tud_hid_set_report_cb(0, 1, HID_REPORT_TYPE_OUTPUT, framed, 2);
    tud_hid_set_report_cb(1, 1, HID_REPORT_TYPE_OUTPUT, led, 1);
    tud_hid_set_report_cb(0, 2, HID_REPORT_TYPE_OUTPUT, led, 1);
    tud_hid_set_report_cb(0, 1, HID_REPORT_TYPE_INPUT, led, 1);
    tud_hid_set_report_cb(0, 1, HID_REPORT_TYPE_OUTPUT, NULL, 1);
    assert(usb_hid_snapshot().keyboard_leds == 0x1a);
    assert(tud_hid_set_idle_cb(0, 0));
    assert(!tud_hid_set_idle_cb(0, 1) && !tud_hid_set_idle_cb(1, 0));
    tud_hid_set_protocol_cb(0, HID_PROTOCOL_REPORT);
    assert(!usb_hid_snapshot().failed);
    tud_hid_set_protocol_cb(0, HID_PROTOCOL_BOOT);
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().error == ESP_ERR_NOT_SUPPORTED);
}

static void test_stale_deferred_dispatch_and_reset_barrier(void)
{
    start_ready();
    const uint64_t old_generation = usb_hid_snapshot().generation;
    assert(send_keyboard(4));
    assert(post_dispatch());
    stack_reset();
    device_event(TINYUSB_EVENT_DETACHED);
    mount_device();
    assert(usb_hid_snapshot().generation != old_generation && usb_hid_snapshot().congested);
    assert(!send_keyboard(5)); /* Old posted token must drain before slot reuse. */
    run_deferred();
    assert(hw.submits == 0 && !usb_hid_snapshot().congested);
    assert(!usb_hid_send_generation(old_generation, 1, (uint8_t[8]){0}, 8));
    assert(send_keyboard(5));
    synchronize();
    complete_report();
    assert(usb_hid_snapshot().accepted == 2 && usb_hid_snapshot().submitted == 1);
    assert(hw.submitted_wire[3] == 5);

    uint8_t old_wire[9];
    memcpy(old_wire, hw.submitted_wire, sizeof(old_wire));
    stack_reset();
    mount_device();
    hw.usb_task = true;
    tud_hid_report_complete_cb(0, old_wire, 9); /* No in-flight owner after reset: ignore old completion. */
    hw.usb_task = false;
    assert(!usb_hid_snapshot().failed && usb_hid_snapshot().completed == 1);
    assert(send_keyboard(6));
    synchronize();
    hw.usb_task = true;
    tud_hid_report_complete_cb(0, old_wire, 9); /* Cannot acknowledge a different fresh report. */
    hw.usb_task = false;
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().completed == 1);
    assert(usb_hid_snapshot().congested);
}

static void disable_during_submit(void)
{
    usb_hid_set_enabled(false);
}

static void test_disable_suspend_and_recovery_keep_cdc(void)
{
    start_ready();
    assert(send_keyboard(4));
    assert(post_dispatch());
    usb_hid_set_enabled(false);
    usb_hid_set_enabled(true);
    run_deferred();
    assert(hw.submits == 0 && !usb_hid_snapshot().failed);
    synchronize();
    assert(send_keyboard(5));
    hw.during_submit = disable_during_submit;
    synchronize();
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().mounted && usb_hid_snapshot().congested);
    usb_hid_set_enabled(true);
    assert(!usb_hid_snapshot().enabled && !send_keyboard(6));
    complete_report();
    assert(usb_hid_snapshot().failed && !usb_hid_snapshot().ready);
    assert(hw.installs == 1 && hw.console_inits == 1);
    stack_reset();
    mount_device();
    assert(!usb_hid_snapshot().failed && !usb_hid_snapshot().enabled);
    usb_hid_set_enabled(true);
    synchronize();
    assert(usb_hid_snapshot().ready);

    assert(send_keyboard(4));
    hw.suspended = true;
    device_event(TINYUSB_EVENT_SUSPENDED);
    assert(!usb_hid_snapshot().failed && usb_hid_snapshot().suspended);
    assert(!send_keyboard(5) && hw.submits == 1); /* Unsubmitted slot was cancelled. */
    hw.suspended = false;
    device_event(TINYUSB_EVENT_RESUMED);
    assert(usb_hid_snapshot().ready && !usb_hid_snapshot().congested);
    assert(send_keyboard(4));
    synchronize();
    hw.suspended = true;
    device_event(TINYUSB_EVENT_SUSPENDED);
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().congested);
    hw.suspended = false;
    device_event(TINYUSB_EVENT_RESUMED);
    assert(!usb_hid_snapshot().ready && !send_keyboard(4));
    complete_report();
    assert(usb_hid_snapshot().failed && hw.console_inits == 1);
    stack_reset();
    mount_device();
    usb_hid_set_enabled(true);
    synchronize();
    const uint64_t generation = usb_hid_snapshot().generation;
    assert(usb_hid_recover_generation(generation));
    assert(!usb_hid_recover_generation(generation));
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().generation != generation);
    assert(hw.installs == 1 && hw.cdc_inits == 1 && hw.console_inits == 1);
}

static void test_suspend_visible_before_its_task_callback(void)
{
    start_ready();
    const uint64_t generation = usb_hid_snapshot().generation;
    assert(send_keyboard(4));
    assert(post_dispatch());
    hw.suspended = true; /* TinyUSB's ISR publishes suspend before queued callback dispatch. */
    run_deferred();
    assert(hw.submits == 0 && usb_hid_snapshot().congested);
    device_event(TINYUSB_EVENT_SUSPENDED);
    assert(!usb_hid_snapshot().failed && !usb_hid_snapshot().ready);
    hw.suspended = false;
    device_event(TINYUSB_EVENT_RESUMED);
    assert(usb_hid_snapshot().ready && !usb_hid_snapshot().congested);
    assert(!usb_hid_send_generation(generation, 1, (uint8_t[8]){0, 0, 4}, 8));
    assert(hw.submits == 0); /* Resume never dispatches the cancelled report. */
}

static void test_backpressure_failures_and_deadlines(void)
{
    start_ready();
    assert(send_keyboard(4));
    hw.endpoint_ready = false;
    synchronize();
    assert(hw.submits == 0 && usb_hid_snapshot().congested);
    assert(!post_dispatch());
    hw.now_us += 2000;
    hw.endpoint_ready = true;
    synchronize();
    assert(hw.submits == 1);
    complete_report();

    assert(send_keyboard(5));
    assert(post_dispatch());
    hw.now_us += 99999;
    hw.timer.callback(NULL);
    assert(!usb_hid_snapshot().failed);
    ++hw.now_us;
    hw.timer.callback(NULL);
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().error == ESP_ERR_TIMEOUT);
    run_deferred();
    assert(hw.submits == 1 && !usb_hid_snapshot().ready);

    start_ready();
    hw.reject_report = true;
    assert(send_keyboard(4));
    synchronize();
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().submitted == 0);
    assert(!post_dispatch() && hw.submits == 1); /* No automatic submission retry. */

    start_ready();
    assert(send_keyboard(4));
    synchronize();
    hw.now_us += 100000;
    hw.timer.callback(NULL);
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().congested);
    complete_report();
    assert(usb_hid_snapshot().failed && !usb_hid_snapshot().congested);
    assert(usb_hid_snapshot().completed == 1);

    start_ready();
    assert(send_keyboard(4));
    synchronize();
    hw.usb_task = true;
    tud_hid_report_failed_cb(0, HID_REPORT_TYPE_INPUT, hw.submitted_wire, 0);
    hw.usb_task = false;
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().congested);
}

static void concurrent_start_attempt(void)
{
    assert(usb_console_start() == ESP_ERR_INVALID_STATE);
}

static void mount_before_manager_callback_registration(void)
{
    hw.mounted = hw.endpoint_ready = true;
}

static void test_manager_failures_and_counter_boundaries(void)
{
    for (unsigned failure = 0; failure < 6; ++failure) {
        reset_fixture();
        switch (failure) {
        case 0: hw.timer_create_error = ESP_ERR_NO_MEM; break;
        case 1: hw.timer_start_error = ESP_FAIL; break;
        case 2: hw.fail_worker = true; break;
        case 3: hw.install_error = ESP_FAIL; break;
        case 4: hw.cdc_error = ESP_FAIL; break;
        case 5: hw.console_error = ESP_FAIL; break;
        }
        const esp_err_t error = usb_console_start();
        assert(error != ESP_OK && usb_hid_snapshot().failed && !usb_hid_snapshot().ready);
        const unsigned installs = hw.installs;
        assert(usb_hid_init() == error && hw.installs == installs);
    }
    reset_fixture();
    hw.during_install = concurrent_start_attempt;
    assert(usb_hid_init() == ESP_OK && hw.installs == 1);

    reset_fixture();
    hw.during_install = mount_before_manager_callback_registration;
    assert(usb_hid_init() == ESP_OK && !usb_hid_snapshot().mounted);
    synchronize(); /* Deferred USB-task synchronization observes a missed early mount. */
    assert(usb_hid_snapshot().mounted && !usb_hid_snapshot().enabled);
    usb_hid_set_enabled(true);
    synchronize();
    assert(usb_hid_snapshot().ready && !usb_hid_snapshot().congested);

    start_ready();
    s_usb.generation = UINT64_MAX;
    usb_hid_set_enabled(false);
    assert(usb_hid_snapshot().failed && !usb_hid_snapshot().ready);
    stack_reset();
    assert(usb_hid_snapshot().failed); /* Never wrap into an old generation. */
    start_ready();
    s_usb.accepted = UINT64_MAX;
    assert(!send_keyboard(4) && usb_hid_snapshot().failed);
    start_ready();
    assert(send_keyboard(4));
    s_token.sequence = UINT64_MAX;
    assert(!post_dispatch() && usb_hid_snapshot().failed);
    start_ready();
    assert(send_keyboard(4));
    hw.now_us = 999;
    hw.timer.callback(NULL);
    assert(usb_hid_snapshot().failed && usb_hid_snapshot().error == ESP_ERR_TIMEOUT);
}

static uint16_t little_word(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void verify_configuration(const uint8_t *descriptor, bool high_speed)
{
    assert(descriptor && descriptor[0] == 9 && descriptor[1] == TUSB_DESC_CONFIGURATION);
    assert(little_word(descriptor + 2) == 100 && descriptor[4] == 3);
    assert(descriptor[7] == 0x80); /* No remote wakeup or fabricated self-power capability. */
    unsigned interfaces = 0, endpoints = 0, cdc = 0, hid = 0;
    bool addresses[256] = {0};
    for (size_t offset = 9; offset < 100;) {
        const uint8_t *item = descriptor + offset;
        assert(item[0] >= 2 && offset + item[0] <= 100);
        if (item[1] == TUSB_DESC_INTERFACE) {
            ++interfaces;
            if (item[5] == TUSB_CLASS_CDC || item[5] == TUSB_CLASS_CDC_DATA) {
                ++cdc;
            } else {
                assert(item[2] == 2 && item[5] == TUSB_CLASS_HID && item[6] == 0 && item[7] == 0);
                ++hid;
            }
        } else if (item[1] == HID_DESC_TYPE_HID) {
            assert(little_word(item + 7) == hid_engine_report_descriptor_length);
        } else if (item[1] == TUSB_DESC_ENDPOINT) {
            assert(!addresses[item[2]]);
            addresses[item[2]] = true;
            ++endpoints;
            if (item[2] == 0x02 || item[2] == 0x82) {
                assert(item[3] == TUSB_XFER_BULK);
                assert(little_word(item + 4) == (high_speed ? 512 : 64));
            } else if (item[2] == 0x83) {
                assert(item[3] == TUSB_XFER_INTERRUPT && little_word(item + 4) == 16);
                assert(item[6] == (high_speed ? 4 : 1));
            } else {
                assert(item[2] == 0x81 && item[3] == TUSB_XFER_INTERRUPT);
            }
        }
        offset += item[0];
    }
    assert(interfaces == 3 && cdc == 2 && hid == 1 && endpoints == 4);
    assert(addresses[0x02] && addresses[0x81] && addresses[0x82] && addresses[0x83]);
}

static void test_real_tinyusb_descriptor_macros_and_long_lifecycle(void)
{
    start_ready();
    verify_configuration(hw.driver.descriptor.full_speed_config, false);
    verify_configuration(hw.driver.descriptor.high_speed_config, true);
    for (unsigned i = 0; i < 100000; ++i) {
        hw.now_us += 10000;
        assert(send_keyboard(0));
        synchronize();
        complete_report();
        stack_reset();
        mount_device();
        assert(usb_hid_snapshot().ready && !usb_hid_snapshot().failed);
    }
    const usb_hid_snapshot_t snapshot = usb_hid_snapshot();
    assert(snapshot.accepted == 100000 && snapshot.submitted == 100000 && snapshot.completed == 100000);
    assert(hw.installs == 1 && hw.workers == 1 && hw.timers == 1);
}

int main(void)
{
    test_admission_submission_and_completion();
    test_invalid_reports_and_control_bounds();
    test_keyboard_led_output_and_report_protocol();
    test_stale_deferred_dispatch_and_reset_barrier();
    test_disable_suspend_and_recovery_keep_cdc();
    test_suspend_visible_before_its_task_callback();
    test_backpressure_failures_and_deadlines();
    test_manager_failures_and_counter_boundaries();
    test_real_tinyusb_descriptor_macros_and_long_lifecycle();
    puts("USB HID: shared CDC manager, real FS/HS descriptors, bounded reports, lifecycle generations, controls, failures and 100000 cycles passed");
    return 0;
}
