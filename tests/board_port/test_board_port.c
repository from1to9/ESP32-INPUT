// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Exercise the actual adapter, including private sampler transitions, against
 * driver doubles. The doubles do not provide device qualification evidence. */
#include "../../firmware/main/board_mosaico.c"

static struct {
    uint64_t time_us;
    unsigned critical_depth;
    esp_err_t variant_error, power_error, button_error;
    esp_err_t imu_init_error, imu_start_error, gyro_error, accel_error, cleanup_error;
    button_event_t register_fail;
    button_event_t registration_order[BUTTON_EVENT_MAX];
    unsigned registrations, deleted_buttons;
    button_cb_t callbacks[BUTTON_EVENT_MAX];
    void *contexts[BUTTON_EVENT_MAX];
    uint16_t hold_ms;
    unsigned legacy_holds, raw_count;
    board_button_event_t raw_events[8];
    uint64_t raw_times[8];
    bool sensor_initialized, fail_imu_task;
    uint64_t ready_after_us;
    uint8_t ready_mask;
    int8_t status_error;
    unsigned imu_inits, imu_starts, imu_stops, imu_deinits;
    unsigned status_reads, gyro_reads, accel_reads;
    float gyro[3], accel[3];
    uint64_t read_delay_us;
    bsp_imu_config_t imu_config;
    TaskFunction_t imu_entry;
    unsigned imu_stack, imu_priority, task_creations, delay_calls;
    bool worker_running;
    void (*during_gyro_read)(void);
    unsigned motor_strength;
    esp_err_t gauge_bus_error, gauge_add_error, gauge_read_error, gauge_cleanup_error;
    int gauge_fail_command;
    uint16_t gauge_words[256];
    unsigned gauge_reads, gauge_adds, gauge_removes;
    uint64_t gauge_read_delay_us;
    uint64_t gauge_poll_times[16];
    unsigned gauge_polls;
    TaskFunction_t power_entry;
    bool fail_power_task, power_worker_running;
    unsigned notifications, waits, stop_after_waits;
    uint64_t notification_interval_us;
    void (*on_power_wait)(void);
    bool display_lock_allowed, display_locked;
    bool display_started;
    esp_err_t spi_limit_error;
    size_t spi_max_bytes;
    unsigned spi_limit_queries, spi_limit_logs;
    char spi_limit_log[192];
    esp_err_t brightness_error;
    unsigned brightness_writes;
    int brightness_actual;
    void (*during_brightness_write)(void);
} hw;
static lv_display_t display;
static lv_indev_t touch;
static struct bmi2_dev sensor;
static int physical_button;
static int gauge_device, gauge_bus;
static jmp_buf worker_exit;

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "injected status";
}

void board_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    assert(hw.critical_depth == 0);
    if (strncmp(format, "Display SPI ", 12) == 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(hw.spi_limit_log, sizeof(hw.spi_limit_log), format, arguments);
        va_end(arguments);
        ++hw.spi_limit_logs;
    }
}

void board_test_enter(portMUX_TYPE *mux)
{
    (void)mux;
    assert(hw.critical_depth == 0);
    ++hw.critical_depth;
}

void board_test_leave(portMUX_TYPE *mux)
{
    (void)mux;
    assert(hw.critical_depth == 1);
    --hw.critical_depth;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)hw.time_us;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(hw.critical_depth == 0 && argument == NULL);
    ++hw.task_creations;
    if (strcmp(name, "imu") == 0) {
        hw.imu_entry = function;
        hw.imu_stack = stack;
        hw.imu_priority = priority;
        if (hw.fail_imu_task) {
            return 0;
        }
        *handle = &sensor;
    } else if (strcmp(name, "board_power") == 0) {
        assert(stack == 3072 && priority == 1);
        hw.power_entry = function;
        if (hw.fail_power_task) {
            return 0;
        }
        *handle = &gauge_device;
    } else {
        assert(strcmp(name, "haptics") == 0 && stack == 3072 && priority == 1);
        *handle = &physical_button;
    }
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t handle)
{
    assert(handle != NULL && hw.critical_depth == 0);
    ++hw.notifications;
}

unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    assert(clear == pdTRUE && hw.power_worker_running && hw.critical_depth == 0);
    ++hw.waits;
    if (hw.waits >= hw.stop_after_waits) {
        longjmp(worker_exit, 1);
    }
    hw.time_us += hw.notification_interval_us ? hw.notification_interval_us : (uint64_t)timeout * 1000;
    if (hw.on_power_wait) {
        hw.on_power_wait();
    }
    return 0;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(hw.time_us / 1000);
}

BaseType_t xTaskDelayUntil(TickType_t *deadline, TickType_t increment)
{
    assert(hw.critical_depth == 0);
    const TickType_t target = *deadline + increment;
    const int32_t wait = (int32_t)(target - xTaskGetTickCount());
    *deadline = target;
    if (wait > 0) {
        hw.time_us += (uint64_t)wait * 1000;
    }
    ++hw.delay_calls;
    return wait > 0;
}

void vTaskDelay(TickType_t delay)
{
    assert(hw.critical_depth == 0);
    hw.time_us += (uint64_t)delay * 1000;
    ++hw.delay_calls;
}

void vTaskDelete(TaskHandle_t handle)
{
    assert(handle == NULL && hw.worker_running && hw.critical_depth == 0);
    longjmp(worker_exit, 1);
}

esp_err_t bsp_board_variant_get(bsp_board_variant_t *variant)
{
    *variant = BSP_BOARD_VARIANT_V1_2;
    return hw.variant_error;
}

esp_err_t bsp_power_init(void)
{
    return hw.power_error;
}

lv_display_t *bsp_display_start_with_config(const bsp_display_config_t *config)
{
    assert(config->enable_touch);
    hw.display_started = true;
    return &display;
}

esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host_id, size_t *max_bytes)
{
    assert(hw.critical_depth == 0 && hw.display_started);
    assert(host_id == BSP_LCD_SPI_HOST && max_bytes != NULL);
    ++hw.spi_limit_queries;
    /* Deliberately write a value even on failure: it must never be reported
     * as a successful query result. */
    *max_bytes = hw.spi_max_bytes;
    return hw.spi_limit_error;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return &touch;
}

bool bsp_display_lock(int32_t timeout_ms)
{
    assert(timeout_ms == 1000 || timeout_ms == 100);
    if (hw.display_lock_allowed) {
        assert(!hw.display_locked);
        hw.display_locked = true;
    }
    return hw.display_lock_allowed;
}

void bsp_display_unlock(void)
{
    assert(hw.display_locked);
    hw.display_locked = false;
}

esp_err_t bsp_display_brightness_set(int percent)
{
    assert(hw.critical_depth == 0 && hw.display_locked && percent >= 0 && percent <= 100);
    ++hw.brightness_writes;
    if (hw.during_brightness_write) {
        void (*hook)(void) = hw.during_brightness_write;
        hw.during_brightness_write = NULL;
        hook();
    }
    if (hw.brightness_error == ESP_OK) {
        hw.brightness_actual = percent;
    }
    return hw.brightness_error;
}

esp_err_t bsp_i2c_init(void)
{
    assert(hw.critical_depth == 0);
    return hw.gauge_bus_error;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    assert(hw.critical_depth == 0);
    return &gauge_bus;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config, i2c_master_dev_handle_t *device)
{
    assert(hw.critical_depth == 0 && bus == &gauge_bus);
    assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(config->device_address == 0x55 && config->scl_speed_hz == 400000);
    ++hw.gauge_adds;
    if (hw.gauge_add_error == ESP_OK) {
        *device = &gauge_device;
    }
    return hw.gauge_add_error;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(hw.critical_depth == 0 && device == &gauge_device);
    ++hw.gauge_removes;
    return hw.gauge_cleanup_error;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const uint8_t *write_buffer, size_t write_size, uint8_t *read_buffer,
    size_t read_size, int timeout_ms)
{
    assert(hw.critical_depth == 0 && device == &gauge_device);
    assert(write_size == 1 && read_size == 2 && timeout_ms == 3);
    const uint8_t command = *write_buffer;
    assert(command == 0x06 || command == 0x08 || command == 0x0a || command == 0x0c ||
        command == 0x2c || command == 0x3a); /* No gauge profile/control writes. */
    if (command == 0x3a) {
        assert(hw.gauge_polls < 16);
        hw.gauge_poll_times[hw.gauge_polls++] = hw.time_us;
    }
    ++hw.gauge_reads;
    hw.time_us += hw.gauge_read_delay_us;
    if (hw.gauge_read_error != ESP_OK) {
        return hw.gauge_read_error;
    }
    if (command == hw.gauge_fail_command) {
        return ESP_FAIL;
    }
    const uint16_t word = hw.gauge_words[command];
    read_buffer[0] = (uint8_t)word;
    read_buffer[1] = (uint8_t)(word >> 8);
    return ESP_OK;
}

esp_err_t bsp_iot_button_create(button_handle_t buttons[], int *count, int capacity)
{
    assert(capacity == BSP_BUTTON_NUM);
    if (hw.button_error != ESP_OK) {
        return hw.button_error;
    }
    buttons[BSP_BUTTON_AI] = &physical_button;
    *count = BSP_BUTTON_NUM;
    return ESP_OK;
}

esp_err_t iot_button_register_cb(button_handle_t button, button_event_t event,
    button_event_args_t *arguments, button_cb_t callback, void *context)
{
    assert(button == &physical_button && event < BUTTON_EVENT_MAX);
    hw.registration_order[hw.registrations++] = event;
    if (event == hw.register_fail) {
        return ESP_FAIL;
    }
    hw.callbacks[event] = callback;
    hw.contexts[event] = context;
    if (arguments) {
        hw.hold_ms = arguments->long_press.press_time;
    }
    return ESP_OK;
}

esp_err_t iot_button_delete(button_handle_t button)
{
    assert(button == &physical_button);
    memset(hw.callbacks, 0, sizeof(hw.callbacks));
    ++hw.deleted_buttons;
    return ESP_OK;
}

esp_err_t bsp_motor_init(void)
{
    assert(hw.critical_depth == 0);
    hw.motor_strength = 0;
    return ESP_OK;
}

esp_err_t bsp_motor_set_strength(uint8_t strength)
{
    assert(hw.critical_depth == 1 && strength <= 100);
    hw.motor_strength = strength;
    return ESP_OK;
}

esp_err_t bsp_motor_set(bool on)
{
    return bsp_motor_set_strength(on ? 100 : 0);
}

esp_err_t bsp_imu_init(void)
{
    assert(hw.critical_depth == 0);
    ++hw.imu_inits;
    hw.sensor_initialized = hw.imu_init_error == ESP_OK;
    return hw.imu_init_error;
}

esp_err_t bsp_imu_start(const bsp_imu_config_t *config)
{
    assert(hw.critical_depth == 0 && hw.sensor_initialized);
    ++hw.imu_starts;
    hw.imu_config = *config;
    return hw.imu_start_error;
}

esp_err_t bsp_imu_stop(void)
{
    assert(hw.critical_depth == 0 && hw.sensor_initialized);
    ++hw.imu_stops;
    return ESP_OK;
}

esp_err_t bsp_imu_deinit(void)
{
    assert(hw.critical_depth == 0);
    ++hw.imu_deinits;
    if (hw.cleanup_error == ESP_OK) {
        hw.sensor_initialized = false;
    }
    return hw.cleanup_error;
}

struct bmi2_dev *bsp_imu_get_handle(void)
{
    assert(hw.critical_depth == 0);
    return hw.sensor_initialized ? &sensor : NULL;
}

int8_t bmi2_get_status(uint8_t *status, struct bmi2_dev *device)
{
    assert(hw.critical_depth == 0 && device == &sensor);
    ++hw.status_reads;
    *status = hw.time_us >= hw.ready_after_us ? hw.ready_mask : 0;
    return hw.status_error;
}

esp_err_t bsp_imu_get_gyro(float *x, float *y, float *z)
{
    assert(hw.critical_depth == 0);
    ++hw.gyro_reads;
    hw.time_us += hw.read_delay_us;
    if (hw.during_gyro_read) {
        void (*hook)(void) = hw.during_gyro_read;
        hw.during_gyro_read = NULL;
        hook();
    }
    *x = hw.gyro[0];
    *y = hw.gyro[1];
    *z = hw.gyro[2];
    return hw.gyro_error;
}

esp_err_t bsp_imu_get_accel(float *x, float *y, float *z)
{
    assert(hw.critical_depth == 0);
    ++hw.accel_reads;
    hw.time_us += hw.read_delay_us;
    *x = hw.accel[0];
    *y = hw.accel[1];
    *z = hw.accel[2];
    return hw.accel_error;
}

static void raw_button_event(board_button_event_t event, uint64_t timestamp_us)
{
    assert(hw.critical_depth == 0 && hw.raw_count < 8);
    hw.raw_events[hw.raw_count] = event;
    hw.raw_times[hw.raw_count++] = timestamp_us;
}

static void legacy_hold(void)
{
    ++hw.legacy_holds;
}

static void reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.register_fail = BUTTON_EVENT_MAX;
    hw.ready_mask = BMI2_DRDY_GYR | BMI2_DRDY_ACC;
    hw.time_us = 1000;
    hw.read_delay_us = 10;
    hw.display_lock_allowed = true;
    hw.spi_max_bytes = 7168;
    hw.brightness_actual = 100; /* Driver state is unknown until acknowledged. */
    hw.gauge_fail_command = -1;
    hw.gauge_read_delay_us = 10;
    hw.gauge_words[0x3a] = 1U << 5;
    hw.gauge_words[0x0a] = 1U << 3;
    hw.gauge_words[0x08] = 3850;
    hw.gauge_words[0x0c] = (uint16_t)-12;
    hw.gauge_words[0x06] = 2981;
    hw.gauge_words[0x2c] = 63;
    hw.gyro[0] = 1.25f;
    hw.gyro[1] = -2.5f;
    hw.gyro[2] = 3.75f;
    hw.accel[0] = 0.1f;
    hw.accel[1] = -0.2f;
    hw.accel[2] = 0.95f;
    s_caps = (board_capabilities_t){.name = "uninitialized"};
    s_init_attempted = false;
    s_display = NULL;
    memset(s_buttons, 0, sizeof(s_buttons));
    s_lock_callback = NULL;
    s_button_event_callback = NULL;
    s_haptic_task = NULL;
    memset(&s_haptics, 0, sizeof(s_haptics));
    s_imu_task = NULL;
    memset(&s_imu_sample, 0, sizeof(s_imu_sample));
    s_imu_sample_valid = false;
    s_imu_error = ESP_ERR_INVALID_STATE;
    s_power_task = NULL;
    s_gauge = NULL;
    s_power = (board_power_status_t){.error = ESP_ERR_INVALID_STATE};
    s_brightness = (board_brightness_status_t){.error = ESP_ERR_INVALID_STATE};
    s_brightness_generation = 0;
    s_brightness_attempted = false;
    s_brightness_attempt_us = 0;
    s_power_logged_error = ESP_OK;
    memset(&s_dispatch, 0, sizeof(s_dispatch));
    s_dispatch_generation = 0;
    s_motor_active = false;
    s_pending = false;
    s_pending_event = HAPTICS_CONFIRM;
    s_pending_ms = 0;
    s_motor_write_failed = false;
    s_motor_error = ESP_OK;
}

static void assert_read_fails_untouched(esp_err_t expected)
{
    board_imu_sample_t sample;
    unsigned char before[sizeof(sample)];
    memset(&sample, 0xa5, sizeof(sample));
    memcpy(before, &sample, sizeof(sample));
    assert(board_imu_read(&sample) == expected);
    assert(memcmp(before, &sample, sizeof(sample)) == 0);
}

static void test_display_spi_diagnostic_is_observational(void)
{
    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    assert(hw.spi_limit_queries == 1 && hw.spi_limit_logs == 1);
    assert(strstr(hw.spi_limit_log, "7168") != NULL);
    assert(board_init(legacy_hold) == ESP_ERR_INVALID_STATE);
    assert(hw.spi_limit_queries == 1 && hw.spi_limit_logs == 1);

    reset();
    hw.spi_limit_error = ESP_ERR_INVALID_ARG;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(hw.spi_limit_queries == 1 && hw.spi_limit_logs == 1);
    assert(strstr(hw.spi_limit_log, "7168") == NULL);
    assert(strstr(hw.spi_limit_log, esp_err_to_name(hw.spi_limit_error)) != NULL);
    assert(board_capabilities().display && board_capabilities().touch);

    reset();
    hw.power_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_FAIL);
    assert(hw.spi_limit_queries == 0 && hw.spi_limit_logs == 0);
}

static void test_raw_button_authority_and_registration(void)
{
    reset();
    assert(board_button_events_register(NULL) == ESP_ERR_INVALID_ARG);
    assert(board_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(board_button_events_register(raw_button_event) == ESP_OK);
    assert(board_button_events_register(raw_button_event) == ESP_ERR_INVALID_STATE);
    assert(board_init(NULL) == ESP_OK);
    assert(board_button_events_register(raw_button_event) == ESP_ERR_INVALID_STATE);
    assert(hw.registrations == 2);
    assert(hw.registration_order[0] == BUTTON_PRESS_UP);
    assert(hw.registration_order[1] == BUTTON_PRESS_DOWN);
    assert(hw.callbacks[BUTTON_LONG_PRESS_START] == NULL);
    assert(board_capabilities().lock_button);
    hw.time_us = 200000;
    hw.callbacks[BUTTON_PRESS_DOWN](&physical_button, hw.contexts[BUTTON_PRESS_DOWN]);
    hw.time_us = 1100000;
    hw.callbacks[BUTTON_PRESS_UP](&physical_button, hw.contexts[BUTTON_PRESS_UP]);
    assert(hw.raw_count == 2 && hw.legacy_holds == 0);
    assert(hw.raw_events[0] == BOARD_BUTTON_DOWN && hw.raw_times[0] == 200000);
    assert(hw.raw_events[1] == BOARD_BUTTON_UP && hw.raw_times[1] == 1100000);

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    assert(hw.registrations == 1 && hw.hold_ms == 900);
    assert(hw.callbacks[BUTTON_PRESS_DOWN] == NULL && hw.callbacks[BUTTON_PRESS_UP] == NULL);
    hw.callbacks[BUTTON_LONG_PRESS_START](&physical_button, NULL);
    assert(hw.legacy_holds == 1 && hw.raw_count == 0);

    for (button_event_t failure = BUTTON_PRESS_DOWN; failure <= BUTTON_PRESS_UP; ++failure) {
        reset();
        hw.register_fail = failure;
        assert(board_button_events_register(raw_button_event) == ESP_OK);
        assert(board_init(legacy_hold) == ESP_FAIL);
        assert(!board_capabilities().lock_button && hw.deleted_buttons == 1);
        assert(hw.imu_inits == 0 && hw.task_creations == 0);
        assert(hw.callbacks[BUTTON_LONG_PRESS_START] == NULL);
    }
}

static void test_imu_real_units_and_nonblocking_cache(void)
{
    reset();
    assert_read_fails_untouched(ESP_ERR_INVALID_STATE);
    assert(board_imu_read(NULL) == ESP_ERR_INVALID_ARG);
    hw.ready_after_us = 11000;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_capabilities().imu && hw.status_reads > 1);
    assert(hw.imu_config.accel_odr == BMI2_ACC_ODR_100HZ);
    assert(hw.imu_config.gyro_odr == BMI2_GYR_ODR_100HZ);
    assert(hw.imu_stack == 3072 && hw.imu_priority == 2 && hw.task_creations == 3);
    board_imu_sample_t sample;
    assert(board_imu_read(&sample) == ESP_OK);
    assert(sample.x_dps == 1.25f && sample.y_dps == -2.5f && sample.z_dps == 3.75f);
    assert(sample.x_g == 0.1f && sample.y_g == -0.2f && sample.z_g == 0.95f);
    assert(sample.timestamp_us < hw.time_us); /* Other board initialization does not retimestamp the cache. */
    const unsigned status_reads = hw.status_reads;
    const unsigned gyro_reads = hw.gyro_reads;
    const unsigned accel_reads = hw.accel_reads;
    board_imu_sample_t repeated;
    assert(board_imu_read(&repeated) == ESP_OK && repeated.timestamp_us == sample.timestamp_us);
    assert(hw.status_reads == status_reads && hw.gyro_reads == gyro_reads && hw.accel_reads == accel_reads);
    hw.time_us = sample.timestamp_us + 50000;
    assert(board_imu_read(&repeated) == ESP_OK);
    ++hw.time_us;
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    assert(!board_capabilities().imu);
    assert(imu_update() == ESP_ERR_TIMEOUT);
    assert(hw.status_reads == status_reads); /* Faulted stream does not reacquire. */
}

static void test_optional_sensor_failures(void)
{
    for (unsigned fault = 0; fault < 7; ++fault) {
        reset();
        esp_err_t expected = ESP_FAIL;
        switch (fault) {
        case 0: hw.imu_init_error = ESP_FAIL; break;
        case 1: hw.imu_start_error = ESP_FAIL; break;
        case 2: hw.status_error = -1; break;
        case 3: hw.gyro_error = ESP_FAIL; break;
        case 4: hw.accel_error = ESP_FAIL; break;
        case 5: hw.fail_imu_task = true; expected = ESP_ERR_NO_MEM; break;
        case 6: hw.ready_mask = 0; expected = ESP_ERR_TIMEOUT; break;
        }
        assert(board_init(legacy_hold) == ESP_OK);
        assert(!board_capabilities().imu && board_capabilities().lock_button);
        assert_read_fails_untouched(expected);
        assert(hw.imu_deinits == 1 && hw.imu_stops == (fault == 0 ? 0U : 1U));
    }
    reset();
    hw.read_delay_us = 11000;
    assert(board_init(legacy_hold) == ESP_OK && !board_capabilities().imu);
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
}

static void stale_consumer_during_bus_read(void)
{
    hw.time_us += 60000;
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
}

static void test_runtime_faults_and_inflight_cancellation(void)
{
    for (unsigned fault = 0; fault < 6; ++fault) {
        reset();
        assert(board_init(legacy_hold) == ESP_OK);
        hw.time_us += 10000;
        esp_err_t expected = ESP_FAIL;
        switch (fault) {
        case 0: hw.gyro_error = ESP_FAIL; break;
        case 1: hw.accel_error = ESP_FAIL; break;
        case 2: hw.status_error = -1; break;
        case 3: hw.gyro[1] = NAN; expected = ESP_ERR_INVALID_RESPONSE; break;
        case 4: hw.accel[2] = INFINITY; expected = ESP_ERR_INVALID_RESPONSE; break;
        case 5: hw.during_gyro_read = stale_consumer_during_bus_read; expected = ESP_ERR_TIMEOUT; break;
        }
        assert(imu_update() == expected);
        assert(!board_capabilities().imu);
        assert_read_fails_untouched(expected);
        hw.gyro_error = hw.accel_error = ESP_OK;
        hw.status_error = 0;
        assert(imu_update() == expected && !board_capabilities().imu);
    }
    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    hw.ready_mask = BMI2_DRDY_GYR; /* Acceleration has no fresh conversion. */
    hw.time_us += 50001;
    assert(imu_update() == ESP_ERR_TIMEOUT && !board_capabilities().imu);

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us = s_imu_sample.timestamp_us - 100;
    assert(imu_update() == ESP_ERR_INVALID_STATE && !board_capabilities().imu);
}

static void test_sensor_worker_cleans_up_after_fault(void)
{
    for (unsigned cleanup_fails = 0; cleanup_fails < 2; ++cleanup_fails) {
        reset();
        hw.time_us = (uint64_t)UINT32_MAX * 1000 - 1000;
        assert(board_init(legacy_hold) == ESP_OK);
        hw.status_error = -1;
        hw.cleanup_error = cleanup_fails ? ESP_FAIL : ESP_OK;
        hw.worker_running = true;
        if (setjmp(worker_exit) == 0) {
            hw.imu_entry(NULL);
            assert(!"Failed sensor worker must delete itself");
        }
        hw.worker_running = false;
        assert(!board_capabilities().imu);
        assert(hw.delay_calls == 1 && hw.imu_stops == 1 && hw.imu_deinits == 1);
        assert(hw.sensor_initialized == (cleanup_fails != 0));
        assert_read_fails_untouched(ESP_FAIL);
    }
}

static void test_long_stream_crosses_32_bit_milliseconds(void)
{
    reset();
    hw.time_us = (uint64_t)UINT32_MAX * 1000 - 1000;
    assert(board_init(legacy_hold) == ESP_OK);
    uint64_t previous_us = s_imu_sample.timestamp_us;
    for (unsigned i = 0; i < 100000; ++i) {
        hw.time_us += 10000;
        assert(imu_update() == ESP_OK);
        board_imu_sample_t sample;
        assert(board_imu_read(&sample) == ESP_OK);
        assert(sample.timestamp_us > previous_us && sample.z_g == 0.95f);
        previous_us = sample.timestamp_us;
    }
    assert(board_capabilities().imu && hw.task_creations == 3);
    assert(hw.imu_deinits == 0 && hw.imu_stops == 0);
}

static void assert_power_unknown(const board_power_status_t *status)
{
    assert(!status->gauge_available && status->battery_present == BOARD_TRUTH_UNKNOWN);
    assert(status->external_power == BOARD_TRUTH_UNKNOWN && status->charging == BOARD_TRUTH_UNKNOWN);
    assert(status->full == BOARD_TRUTH_UNKNOWN && !status->percent_valid);
    assert(!status->voltage_valid && !status->current_valid && !status->temperature_valid);
}

static void test_checked_battery_readings_and_truth_states(void)
{
    reset();
    board_power_status_t status;
    assert(board_power_read(NULL) == ESP_ERR_INVALID_ARG);
    assert(board_power_read(&status) == ESP_ERR_INVALID_STATE);
    assert_power_unknown(&status);
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_capabilities().battery && board_capabilities().brightness);
    assert(board_power_read(&status) == ESP_OK);
    assert(status.gauge_available && status.battery_present == BOARD_TRUTH_YES);
    assert(status.external_power == BOARD_TRUTH_UNKNOWN && status.charging == BOARD_TRUTH_NO);
    assert(status.percent_valid && status.percent == 63);
    assert(status.voltage_valid && status.voltage_mv == 3850);
    assert(status.current_valid && status.current_ma == -12);
    assert(status.temperature_valid && fabsf(status.temperature_c - 24.95f) < 0.001f);
    const unsigned reads = hw.gauge_reads;
    assert(board_power_read(&status) == ESP_OK && hw.gauge_reads == reads);

    hw.gauge_words[0x0c] = 21;
    power_update();
    assert(board_power_read(&status) == ESP_OK && status.charging == BOARD_TRUTH_YES);
    assert(status.external_power == BOARD_TRUTH_UNKNOWN); /* Positive current is not a VBUS sensor. */
    hw.gauge_words[0x0c] = 0;
    hw.gauge_words[0x0a] |= 1U << 9;
    power_update();
    assert(board_power_read(&status) == ESP_OK && status.charging == BOARD_TRUTH_UNKNOWN);
    assert(status.full == BOARD_TRUTH_YES && status.external_power == BOARD_TRUTH_UNKNOWN);

    hw.gauge_words[0x0c] = UINT16_MAX; /* Signed -1 mA, not a read-failure sentinel. */
    hw.gauge_words[0x2c] = 0;
    power_update();
    assert(board_power_read(&status) == ESP_OK && status.current_ma == -1);
    assert(status.percent_valid && status.percent == 0 && status.charging == BOARD_TRUTH_NO);
    hw.gauge_words[0x2c] = 257; /* Do not truncate to a plausible uint8 percentage. */
    power_update();
    assert(board_power_read(&status) == ESP_OK && !status.percent_valid);
    assert(status.voltage_valid && status.current_valid);

    hw.gauge_words[0x0a] = 0;
    power_update();
    assert(board_power_read(&status) == ESP_OK && status.gauge_available);
    assert(status.battery_present == BOARD_TRUTH_NO && status.external_power == BOARD_TRUTH_UNKNOWN);
    assert(status.charging == BOARD_TRUTH_UNKNOWN && !status.percent_valid && !status.voltage_valid);
    assert(board_capabilities().battery); /* Telemetry capability is independent of cell presence. */
}

static void test_battery_failures_staleness_and_recovery(void)
{
    const uint8_t commands[] = {0x3a, 0x0a, 0x08, 0x0c, 0x06, 0x2c};
    board_power_status_t status;
    for (size_t i = 0; i < sizeof(commands); ++i) {
        reset();
        hw.gauge_fail_command = commands[i];
        assert(board_init(legacy_hold) == ESP_OK);
        assert(board_power_read(&status) == ESP_FAIL);
        assert_power_unknown(&status);
        assert(hw.gauge_reads == i + 1 && !board_capabilities().battery);
        hw.gauge_fail_command = -1;
        power_update();
        assert(board_power_read(&status) == ESP_OK && board_capabilities().battery);
    }
    reset();
    hw.gauge_bus_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK && board_capabilities().brightness);
    assert(board_power_read(&status) == ESP_FAIL && hw.gauge_adds == 0);
    assert_power_unknown(&status);
    reset();
    hw.gauge_add_error = ESP_ERR_NO_MEM;
    assert(board_init(legacy_hold) == ESP_OK && board_capabilities().brightness);
    assert(board_power_read(&status) == ESP_ERR_NO_MEM);
    assert_power_unknown(&status);

    reset();
    hw.gauge_words[0x3a] = 0; /* Device answers, but gauge initialization is not complete. */
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_INVALID_STATE && status.gauge_available);
    assert(status.battery_present == BOARD_TRUTH_UNKNOWN && !status.percent_valid);
    assert(!board_capabilities().battery);
    hw.gauge_words[0x3a] = (1U << 5) | (1U << 10); /* Configuration update is not ready. */
    power_update();
    assert(board_power_read(&status) == ESP_ERR_INVALID_STATE && !status.percent_valid);

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    const uint64_t timestamp_us = s_power.timestamp_us;
    hw.time_us = timestamp_us + 5000000;
    assert(board_power_read(&status) == ESP_OK);
    ++hw.time_us;
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT && status.timestamp_us == timestamp_us);
    assert_power_unknown(&status);
    const unsigned reads = hw.gauge_reads;
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT && hw.gauge_reads == reads);
    power_update();
    assert(board_power_read(&status) == ESP_OK && board_capabilities().battery);
    hw.time_us = status.timestamp_us - 1;
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT);
    assert_power_unknown(&status);

    reset();
    hw.gauge_read_delay_us = 4000;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT);
    assert_power_unknown(&status);
    assert(hw.gauge_reads == 6); /* Batch budget rejects a late all-success response. */

    reset();
    hw.fail_power_task = true;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_NO_MEM);
    assert_power_unknown(&status);
    assert(!board_capabilities().brightness && !board_capabilities().battery);
    assert(hw.gauge_removes == 1 && s_gauge == NULL);
    assert(board_brightness_set(50) == ESP_ERR_INVALID_STATE);

    reset();
    hw.fail_power_task = true;
    hw.gauge_cleanup_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_NO_MEM && !board_capabilities().battery);
    assert_power_unknown(&status);
    assert(hw.gauge_removes == 1 && s_gauge != NULL); /* Preserve failed-cleanup ownership. */
}

static void request_restore_during_blank(void)
{
    assert(board_brightness_set(80) == ESP_OK);
}

static void test_brightness_queue_off_restore_and_failures(void)
{
    reset();
    assert(board_brightness_set(-1) == ESP_ERR_INVALID_ARG);
    assert(board_brightness_set(101) == ESP_ERR_INVALID_ARG);
    assert(board_brightness_set(30) == ESP_ERR_INVALID_STATE);
    assert(board_init(legacy_hold) == ESP_OK);
    board_brightness_status_t status = board_brightness_status();
    assert(!status.requested && !status.applied_known && !status.pending);
    assert(hw.brightness_writes == 0); /* Preserve retained boot brightness until policy requests. */
    const unsigned reads = hw.gauge_reads;
    assert(board_brightness_set(0) == ESP_OK);
    assert(hw.brightness_writes == 0 && hw.gauge_reads == reads);
    status = board_brightness_status();
    assert(status.pending && status.requested_percent == 0 && !status.applied_known);
    const unsigned notifications = hw.notifications;
    assert(board_brightness_set(0) == ESP_OK && hw.notifications == notifications);
    brightness_update();
    status = board_brightness_status();
    assert(status.applied_known && status.applied_percent == 0 && !status.pending && status.error == ESP_OK);
    assert(hw.brightness_actual == 0 && hw.brightness_writes == 1);
    assert(board_capabilities().display && board_capabilities().touch && board_capabilities().imu);
    assert(board_brightness_set(0) == ESP_OK && hw.brightness_writes == 1);
    assert(board_brightness_set(1) == ESP_OK);
    assert(board_brightness_set(100) == ESP_OK); /* Latest pending value replaces one older value. */
    brightness_update();
    assert(hw.brightness_actual == 0 && board_brightness_status().pending);
    hw.time_us += 20000;
    brightness_update();
    assert(hw.brightness_actual == 100 && hw.brightness_writes == 2);

    hw.during_brightness_write = request_restore_during_blank;
    assert(board_brightness_set(0) == ESP_OK);
    hw.time_us += 20000;
    brightness_update();
    status = board_brightness_status();
    assert(status.applied_percent == 0 && status.requested_percent == 80 && status.pending);
    hw.time_us += 20000;
    brightness_update();
    status = board_brightness_status();
    assert(status.applied_percent == 80 && !status.pending);

    hw.brightness_error = ESP_FAIL;
    assert(board_brightness_set(15) == ESP_OK);
    hw.time_us += 20000;
    brightness_update();
    status = board_brightness_status();
    assert(status.error == ESP_FAIL && !status.applied_known && !status.pending);
    hw.brightness_error = ESP_OK;
    assert(board_brightness_set(15) == ESP_OK); /* An explicit retry of a failed value is not coalesced away. */
    hw.time_us += 20000;
    brightness_update();
    assert(board_brightness_status().applied_known && hw.brightness_actual == 15);

    hw.display_lock_allowed = false;
    const unsigned writes = hw.brightness_writes;
    assert(board_brightness_set(25) == ESP_OK);
    hw.time_us += 20000;
    brightness_update();
    assert(board_brightness_status().error == ESP_ERR_TIMEOUT && hw.brightness_writes == writes);
    hw.display_lock_allowed = true;
    assert(board_brightness_set(25) == ESP_OK);
    hw.time_us += 20000;
    brightness_update();
    assert(hw.brightness_actual == 25);
    s_brightness_generation = UINT64_MAX;
    assert(board_brightness_set(35) == ESP_ERR_INVALID_STATE);
    assert(!board_brightness_status().pending && hw.brightness_actual == 25);
}

static void brightness_flood(void)
{
    assert(board_brightness_set((int)(hw.waits % 101)) == ESP_OK);
}

static void test_worker_rate_limits_battery_during_brightness_flood(void)
{
    reset();
    hw.time_us = (uint64_t)UINT32_MAX * 1000 - 1000;
    assert(board_init(legacy_hold) == ESP_OK);
    hw.power_worker_running = true;
    hw.stop_after_waits = 2010;
    hw.notification_interval_us = 1000;
    hw.on_power_wait = brightness_flood;
    if (setjmp(worker_exit) == 0) {
        hw.power_entry(NULL);
        assert(!"Deterministic worker harness must end at its wait boundary");
    }
    hw.power_worker_running = false;
    assert(hw.gauge_polls == 3 && hw.gauge_reads == 18);
    assert(hw.brightness_writes >= 90 && hw.brightness_writes <= 101);
    assert(hw.gauge_adds == 1 && hw.task_creations == 3);
    for (unsigned i = 1; i < hw.gauge_polls; ++i) {
        assert(hw.gauge_poll_times[i] - hw.gauge_poll_times[i - 1] >= 1000000);
    }
}

int main(void)
{
    test_display_spi_diagnostic_is_observational();
    test_raw_button_authority_and_registration();
    test_imu_real_units_and_nonblocking_cache();
    test_optional_sensor_failures();
    test_runtime_faults_and_inflight_cancellation();
    test_sensor_worker_cleans_up_after_fault();
    test_long_stream_crosses_32_bit_milliseconds();
    test_checked_battery_readings_and_truth_states();
    test_battery_failures_staleness_and_recovery();
    test_brightness_queue_off_restore_and_failures();
    test_worker_rate_limits_battery_during_brightness_flood();
    puts("Board adapter: physical edges, IMU stream, checked battery states, brightness transitions, faults and bounded worker schedules passed");
    return 0;
}
