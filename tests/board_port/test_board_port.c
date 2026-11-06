// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interaction.h"

/* Exercise the actual adapter, including private sampler transitions, against
 * driver doubles. The doubles do not provide device qualification evidence. */
#include "../../firmware/main/board_mosaico.c"

static struct {
    uint64_t time_us;
    unsigned critical_depth;
    esp_err_t variant_error, power_error, button_error;
    esp_err_t display_new_error, observer_error;
    esp_err_t motor_error;
    unsigned motor_writes;
    unsigned display_new_calls, observer_installs;
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
    uint64_t gyro_ready_after_us;
    unsigned accel_only_status_reads;
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
    unsigned stop_sensor_after_delays;
    void (*on_sensor_delay)(void);
    bool record_sensor_attempts;
    uint64_t sensor_attempt_times[512];
    unsigned sensor_attempt_count;
    void (*during_gyro_read)(void);
    unsigned motor_strength;
    esp_err_t gauge_bus_error, gauge_add_error, gauge_read_error, gauge_cleanup_error;
    int gauge_fail_command;
    uint16_t gauge_words[256];
    unsigned gauge_reads, gauge_adds, gauge_removes;
    uint64_t gauge_read_delay_us;
    uint64_t gauge_after_read_delay_us, gauge_last_packet_end_us;
    bool gauge_packet_seen;
    unsigned gauge_gap_delays, power_warnings;
    char power_warning[256];
    void (*during_gauge_read)(void);
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
static int panel_token, panel_io_token;
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
    } else if (strncmp(format, "Battery telemetry unavailable:", 30) == 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(hw.power_warning, sizeof(hw.power_warning), format, arguments);
        va_end(arguments);
        ++hw.power_warnings;
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

void esp_rom_delay_us(uint32_t microseconds)
{
    assert(hw.critical_depth == 0 && microseconds == 100);
    hw.time_us += microseconds; ++hw.gauge_gap_delays;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(hw.critical_depth == 0 && argument == NULL);
    ++hw.task_creations;
    if (strcmp(name, "board_sensor") == 0) {
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
    if (hw.worker_running && hw.stop_sensor_after_delays && hw.delay_calls >= hw.stop_sensor_after_delays)
        longjmp(worker_exit, 1);
    if (hw.worker_running && hw.on_sensor_delay) hw.on_sensor_delay();
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
    assert(config->enable_touch && hw.display_new_calls == 1 && hw.observer_installs == 1);
    hw.display_started = true;
    return &display;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *panel)
{
    assert(config->enable_touch && panel && !hw.display_started);
    ++hw.display_new_calls;
    if (hw.display_new_error == ESP_OK) *panel = (esp_lcd_panel_handle_t)&panel_token;
    return hw.display_new_error;
}

esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void)
{
    assert(hw.display_new_calls == 1 && !hw.display_started);
    return (esp_lcd_panel_io_handle_t)&panel_io_token;
}

esp_err_t board_display_transfer_install(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io)
{
    assert(!hw.display_started && hw.display_new_calls == 1);
    assert(panel == (esp_lcd_panel_handle_t)&panel_token && io == (esp_lcd_panel_io_handle_t)&panel_io_token);
    ++hw.observer_installs;
    return hw.observer_error;
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
    assert(config->scl_wait_us == 5000); /* TI permits a 4 ms stretch. */
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
    assert(write_size == 1 && timeout_ms == 6);
    const uint8_t command = *write_buffer;
    assert((command == 0x06 && read_size == 8) ||
        ((command == 0x2c || command == 0x3a) && read_size == 2)); /* Read-only packets. */
    if (hw.gauge_packet_seen) {
        assert(hw.time_us >= hw.gauge_last_packet_end_us &&
            hw.time_us - hw.gauge_last_packet_end_us >= 66);
    }
    if (command == 0x3a) {
        assert(hw.gauge_polls < 16);
        hw.gauge_poll_times[hw.gauge_polls++] = hw.time_us;
    }
    ++hw.gauge_reads;
    hw.time_us += hw.gauge_read_delay_us + hw.gauge_after_read_delay_us;
    if (hw.during_gauge_read) {
        void (*hook)(void) = hw.during_gauge_read; hw.during_gauge_read = NULL; hook();
    }
    hw.gauge_packet_seen = true; hw.gauge_last_packet_end_us = hw.time_us;
    if (hw.gauge_read_error != ESP_OK) {
        return hw.gauge_read_error;
    }
    if (hw.gauge_read_delay_us > (uint64_t)timeout_ms * 1000U) return ESP_ERR_TIMEOUT;
    if (hw.gauge_fail_command >= command &&
        hw.gauge_fail_command < (int)(command + read_size)) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < read_size; ++i) {
        const unsigned address = (unsigned)command + (unsigned)i;
        const uint16_t word = hw.gauge_words[address & ~1U];
        read_buffer[i] = (uint8_t)(word >> ((address & 1U) * 8U));
    }
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
    ++hw.motor_writes;
    if (hw.motor_error == ESP_OK) hw.motor_strength = strength;
    return hw.motor_error;
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
    if (hw.time_us < hw.gyro_ready_after_us) {
        *status &= (uint8_t)~BMI2_DRDY_GYR;
    }
    if ((*status & (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) == BMI2_DRDY_ACC) {
        ++hw.accel_only_status_reads;
    }
    return hw.status_error;
}

int8_t bmi2_get_sensor_data(struct bmi2_sens_data *data, struct bmi2_dev *device)
{
    if (hw.record_sensor_attempts) {
        assert(hw.sensor_attempt_count < 512);
        hw.sensor_attempt_times[hw.sensor_attempt_count++] = hw.time_us;
    }
    int8_t error = bmi2_get_status(&data->status, device);
    if (error != BMI2_OK) return error;
    ++hw.gyro_reads; ++hw.accel_reads;
    hw.time_us += hw.read_delay_us * 2U; /* Complete corrected-frame wall span. */
    if (hw.during_gyro_read) {
        void (*hook)(void) = hw.during_gyro_read; hw.during_gyro_read = NULL; hook();
    }
    if (hw.gyro_error != ESP_OK || hw.accel_error != ESP_OK) return BMI2_E_COM_FAIL;
    int16_t *gyro[] = {&data->gyr.x, &data->gyr.y, &data->gyr.z};
    int16_t *accel[] = {&data->acc.x, &data->acc.y, &data->acc.z};
    for (unsigned i = 0; i < 3; ++i) {
        assert(isfinite(hw.gyro[i]) && isfinite(hw.accel[i]));
        *gyro[i] = (int16_t)lroundf(hw.gyro[i] * (32768.0f / 2000.0f));
        *accel[i] = (int16_t)lroundf(hw.accel[i] * (32768.0f / 4.0f));
    }
    return BMI2_OK;
}

esp_err_t bsp_imu_get_gyro(float *x, float *y, float *z)
{
    assert(hw.critical_depth == 0);
    assert(hw.time_us >= hw.gyro_ready_after_us);
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
    hw.gyro[0] = 20 * (2000.0f / 32768.0f);
    hw.gyro[1] = -40 * (2000.0f / 32768.0f);
    hw.gyro[2] = 60 * (2000.0f / 32768.0f);
    hw.accel[0] = 1024 * (4.0f / 32768.0f);
    hw.accel[1] = -2048 * (4.0f / 32768.0f);
    hw.accel[2] = 8192 * (4.0f / 32768.0f);
    s_caps = (board_capabilities_t){.name = "uninitialized"};
    s_init_attempted = false;
    s_display = NULL;
    memset(s_buttons, 0, sizeof(s_buttons));
    s_lock_callback = NULL;
    s_button_event_callback = NULL;
    s_haptic_task = NULL;
    memset(&s_haptics, 0, sizeof(s_haptics));
    s_haptic_status = (board_haptic_status_t){.error = ESP_ERR_INVALID_STATE};
    s_haptic_diagnostic_pending = false;
    s_imu_task = NULL;
    s_imu_hardware_live = false; s_imu_generation = 0;
    memset(&s_imu_config, 0, sizeof(s_imu_config));
    s_imu_accel_scale = s_imu_gyro_scale = 0;
    memset(&s_imu_sample, 0, sizeof(s_imu_sample));
    memset(&s_imu_stream, 0, sizeof(s_imu_stream));
    memset(&s_sensor_journal, 0, sizeof(s_sensor_journal));
    memset(&s_imu_observation, 0, sizeof(s_imu_observation));
    s_imu_sample_valid = false;
    s_imu_error = ESP_ERR_INVALID_STATE;
    s_imu_status = (board_imu_status_t){
        .error = ESP_ERR_INVALID_STATE,
        .first_fault_stage = "none",
        .last_hardware_stage = "none",
        .last_hardware_error = ESP_ERR_INVALID_STATE,
        .last_read_error = ESP_ERR_INVALID_STATE,
    };
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
    assert(hw.imu_stack == 4096 && hw.imu_priority == 6 && hw.task_creations == 3);
    board_imu_sample_t sample;
    assert(board_imu_read(&sample) == ESP_OK);
    assert(sample.x_dps == hw.gyro[0] && sample.y_dps == hw.gyro[1] && sample.z_dps == hw.gyro[2]);
    assert(sample.x_g == hw.accel[0] && sample.y_g == hw.accel[1] && sample.z_g == hw.accel[2]);
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
    assert(board_imu_status().phase == BOARD_IMU_RECOVERING);
    assert(imu_update() == ESP_OK && board_capabilities().imu);
    assert(hw.status_reads == status_reads + 1); /* Only a new frame restores capability. */
}

static void test_imu_display_frame_is_one_proper_rotation(void)
{
    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    const float expected[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float transformed[3][3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned component = 0; component < 3; ++component) {
            hw.gyro[component] = component == axis ? 62.5f : 0.0f;
            hw.accel[component] = component == axis ? 0.125f : 0.0f;
        }
        hw.time_us += 10000;
        assert(imu_update() == ESP_OK);
        const board_imu_sample_t raw_cache = s_imu_sample;
        const unsigned bus_reads = hw.gyro_reads + hw.accel_reads + hw.status_reads;
        board_imu_sample_t output;
        assert(board_imu_read(&output) == ESP_OK);
        transformed[axis][0] = output.x_dps / 62.5f;
        transformed[axis][1] = output.y_dps / 62.5f;
        transformed[axis][2] = output.z_dps / 62.5f;
        assert(output.x_g / 0.125f == output.x_dps / 62.5f &&
            output.y_g / 0.125f == output.y_dps / 62.5f && output.z_g / 0.125f == output.z_dps / 62.5f);
        for (unsigned component = 0; component < 3; ++component)
            assert(transformed[axis][component] == expected[axis][component]);
        assert(output.timestamp_us == raw_cache.timestamp_us);
        assert(s_imu_sample.x_dps == raw_cache.x_dps && s_imu_sample.y_dps == raw_cache.y_dps && s_imu_sample.z_dps == raw_cache.z_dps);
        assert(s_imu_sample.x_g == raw_cache.x_g && s_imu_sample.y_g == raw_cache.y_g && s_imu_sample.z_g == raw_cache.z_g);
        assert(s_imu_sample.timestamp_us == raw_cache.timestamp_us);
        assert(bus_reads == hw.gyro_reads + hw.accel_reads + hw.status_reads);
    }
    /* A rotation preserves orthonormality and handedness: R(eX) x R(eY) = R(eZ).
     * An improper axis permutation would fail this determinant +1 invariant. */
    for (unsigned a = 0; a < 3; ++a) {
        for (unsigned b = 0; b < 3; ++b) {
            float dot = 0;
            for (unsigned c = 0; c < 3; ++c) dot += transformed[a][c] * transformed[b][c];
            assert(dot == (a == b ? 1.0f : 0.0f));
        }
        unsigned b = (a + 1) % 3, c = (a + 2) % 3;
        float cross = transformed[0][b] * transformed[1][c] - transformed[0][c] * transformed[1][b];
        assert(cross == transformed[2][a]);
    }
}

static void test_confirmed_upright_pose_uses_display_top_axis(void)
{
    reset();
    memset(hw.gyro, 0, sizeof hw.gyro);
    /* Confirmed text-upright, screen-facing hold on 2026-10-23: the old
     * adapter reported X=-0.851..-0.816, Y=-0.032..0.023, Z=0.502..0.562 g.
     * Undoing that known old (-nativeY,nativeX,nativeZ) transform gives this
     * representative native boundary fixture. The hold was inclined, not an
     * exact 90-degree pose. Its up component belongs to display Y, not X. */
    hw.accel[0] = 0; hw.accel[1] = .84f; hw.accel[2] = .53f;
    assert(board_init(legacy_hold) == ESP_OK);
    board_imu_sample_t sample;
    assert(board_imu_read(&sample) == ESP_OK);
    assert(fabsf(sample.x_g) < .035f);
    assert(sample.y_g > .80f && sample.y_g < .88f);
    assert(sample.z_g > .49f && sample.z_g < .58f);
    const float norm = sqrtf(sample.x_g * sample.x_g + sample.y_g * sample.y_g + sample.z_g * sample.z_g);
    assert(fabsf(norm - 1) < .03f);
}

static input_delta_t board_motion_frame(interaction_t *model)
{
    hw.time_us += 10000;
    assert(imu_update() == ESP_OK);
    board_imu_sample_t sample;
    assert(board_imu_read(&sample) == ESP_OK);
    const interaction_imu_sample_t mapped = {.timestamp_us = sample.timestamp_us,
        .gx_dps = sample.x_dps, .gy_dps = sample.y_dps, .gz_dps = sample.z_dps,
        .ax_g = sample.x_g, .ay_g = sample.y_g, .az_g = sample.z_g};
    return interaction_motion_sample(model, &mapped);
}

static void calibrated_board_grip(interaction_t *model, float tilt)
{
    reset();
    memset(hw.gyro, 0, sizeof hw.gyro);
    hw.accel[0] = 0; hw.accel[1] = sinf(tilt); hw.accel[2] = cosf(tilt);
    assert(board_init(legacy_hold) == ESP_OK);
    interaction_init(model);
    input_state_t state;
    input_init(&state);
    input_set_connected(&state, true);
    assert(input_set_mode(&state, INPUT_MOUSE));
    assert(input_ack_release(&state, state.epoch));
    interaction_sync(model, &state, true, hw.time_us / 1000U);
    interaction_config_t config = model->config;
    config.pointer_gain_x = 8; config.pointer_gain_y = 2; config.filter_seconds = 0;
    assert(interaction_configure(model, &config));
    for (unsigned i = 0; i < 64; ++i) {
        const input_delta_t delta = board_motion_frame(model);
        assert(delta.x == 0 && delta.y == 0);
    }
    assert(model->view.calibrated && model->gravity_ready);
    float calibrated_bias[3];
    memcpy(calibrated_bias, model->bias, sizeof calibrated_bias);
    assert(interaction_set_motion(model, true));
    assert(model->view.calibrated && model->view.motion_enabled && model->gravity_ready);
    const input_delta_t aligned = board_motion_frame(model);
    assert(aligned.x == 0 && aligned.y == 0 && model->aim_aligned);
    assert(model->view.calibrated && !memcmp(calibrated_bias, model->bias, sizeof calibrated_bias));
}

static void test_board_to_model_flat_inclined_and_upright_axes(void)
{
    const float grips[] = {0, .5f * 3.14159265359f, atan2f(.84f, .53f)};
    for (unsigned grip = 0; grip < sizeof(grips) / sizeof(grips[0]); ++grip) {
        for (int direction = -1; direction <= 1; direction += 2) {
            for (unsigned pitch = 0; pitch < 2; ++pitch) {
                interaction_t model;
                calibrated_board_grip(&model, grips[grip]);
                const uint64_t first_us = s_imu_sample.timestamp_us;
                const float rate = 31.25f * (float)direction;
                int total_x = 0, total_y = 0;
                for (unsigned i = 0; i < 100; ++i) {
                    const float elapsed = (float)(hw.time_us + 10000 + hw.read_delay_us * 2 - first_us) / 1000000.0f;
                    const float tilt = grips[grip] + (pitch ? rate * .017453292519943295f * elapsed : 0);
                    hw.accel[0] = 0; hw.accel[1] = sinf(tilt); hw.accel[2] = cosf(tilt);
                    /* Physical yaw is about world-up; pitch is about the
                     * screen left-right axis. These are not Euler UI labels. */
                    hw.gyro[0] = pitch ? rate : 0;
                    hw.gyro[1] = pitch ? 0 : rate * sinf(tilt);
                    hw.gyro[2] = pitch ? 0 : rate * cosf(tilt);
                    const input_delta_t delta = board_motion_frame(&model);
                    total_x += delta.x; total_y += delta.y;
                    assert(model.gravity_ready);
                }
                if (pitch) {
                    assert(abs(total_x) <= 1 && total_y * direction >= 61 && total_y * direction <= 64);
                } else {
                    assert(abs(total_y) <= 1 && total_x * direction >= 249 && total_x * direction <= 252);
                }
            }
        }
    }
}

static void test_imu_start_waits_for_gyro_with_a_finite_deadline(void)
{
    reset();
    const uint64_t start_us = hw.time_us;
    hw.ready_mask |= 0x10; /* Command-ready bit retained alongside sensor readiness. */
    hw.ready_after_us = start_us + 2000;
    hw.gyro_ready_after_us = start_us + 80000;
    assert(board_init(legacy_hold) == ESP_OK);
    board_imu_status_t status = board_imu_status();
    assert(status.available && status.sample_count == 1 && !status.fault_latched);
    assert(hw.accel_only_status_reads > 0 && hw.gyro_reads == hw.status_reads && hw.accel_reads == hw.status_reads);
    assert(status.last_sample_us >= hw.gyro_ready_after_us);
    assert(status.last_sample_us - start_us < 100000 && status.data_ready_bits == 0xd0);
    hw.time_us = status.last_sample_us + 50001;
    assert_read_fails_untouched(ESP_ERR_TIMEOUT); /* Startup allowance never widens runtime freshness. */
    assert(strcmp(board_imu_status().first_fault_stage, "cache_age") == 0);

    reset();
    const uint64_t timeout_start_us = hw.time_us;
    hw.ready_mask |= 0x10;
    hw.ready_after_us = timeout_start_us + 2000;
    hw.gyro_ready_after_us = timeout_start_us + 120000;
    assert(board_init(legacy_hold) == ESP_OK);
    status = board_imu_status();
    assert(!status.available && status.sample_count == 0 && status.fault_latched);
    assert(status.first_fault_error == ESP_ERR_TIMEOUT && strcmp(status.first_fault_stage, "first_sample") == 0);
    assert(status.first_fault_us - timeout_start_us >= 100000);
    assert(status.first_fault_us - timeout_start_us < 102000);
    assert(status.data_ready_known && status.data_ready_bits == 0x90);
    assert(hw.accel_only_status_reads > 0 && hw.gyro_reads == hw.status_reads && hw.accel_reads == hw.status_reads);
    assert(hw.imu_stops == 1 && hw.imu_deinits == 1 && !hw.sensor_initialized);

    reset();
    hw.ready_after_us = hw.time_us + 90000;
    hw.read_delay_us = 6000;
    assert(board_init(legacy_hold) == ESP_OK);
    status = board_imu_status();
    assert(!status.available && status.sample_count == 0 && status.fault_latched);
    assert(strcmp(status.first_fault_stage, "first_sample") == 0);
    assert(status.last_hardware_error == ESP_OK); /* A valid batch completed after the startup deadline. */
}

static void test_optional_sensor_failures(void)
{
    static const char *const stages[] = {
        "init", "start", "frame", "frame", "frame", "worker_create", "first_sample",
    };
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
        const board_imu_status_t status = board_imu_status();
        assert(status.initialization_attempted && status.fault_latched && !status.available);
        assert(status.first_fault_error == expected && strcmp(status.first_fault_stage, stages[fault]) == 0);
        assert(status.first_fault_us <= status.timestamp_us);
        assert(status.initialization_succeeded == (fault != 0));
        assert(status.start_succeeded == (fault != 0 && fault != 1));
        assert(status.worker_created == (fault != 5) && !status.worker_running);
        assert(status.sample_count == (fault == 5 ? 1U : 0U));
    }
    reset();
    hw.read_delay_us = 11000;
    assert(board_init(legacy_hold) == ESP_OK && !board_capabilities().imu);
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
}

static void test_imu_health_is_nonmutating_and_retains_first_fault(void)
{
    reset();
    board_imu_status_t status = board_imu_status();
    assert(!status.initialization_attempted && !status.initialization_succeeded);
    assert(!status.available && !status.sample_valid && !status.sample_fresh && !status.sample_age_known);
    assert(!status.fault_latched && !status.hardware_read_seen && !status.read_seen);
    assert(status.error == ESP_ERR_INVALID_STATE && hw.status_reads == 0);

    assert(board_init(legacy_hold) == ESP_OK);
    status = board_imu_status();
    assert(status.initialization_attempted && status.initialization_succeeded && status.start_succeeded);
    assert(status.worker_created && status.available && status.sample_valid && status.sample_fresh);
    assert(status.sample_count == 1 && !status.sample_count_saturated && status.sample_age_known);
    assert(status.last_sample_us == s_imu_sample.timestamp_us && !status.fault_latched);
    assert(status.hardware_read_seen && status.last_hardware_error == ESP_OK);
    assert(strcmp(status.last_hardware_stage, "sample") == 0);
    assert(status.data_ready_known && status.data_ready_bits == (BMI2_DRDY_GYR | BMI2_DRDY_ACC));
    assert(!status.read_seen && status.last_read_error == ESP_ERR_INVALID_STATE);

    const unsigned reads = hw.status_reads;
    hw.time_us = status.last_sample_us + 60000;
    for (unsigned i = 0; i < 1000; ++i) {
        status = board_imu_status();
        assert(status.sample_age_us == 60000 && status.available && status.sample_valid && !status.sample_fresh);
        assert(!status.fault_latched && status.error == ESP_OK && status.sample_count == 1);
    }
    assert(hw.status_reads == reads && s_imu_error == ESP_OK);
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    status = board_imu_status();
    const uint64_t fault_us = status.first_fault_us;
    assert(status.fault_latched && !status.available && !status.sample_valid);
    assert(strcmp(status.first_fault_stage, "cache_age") == 0);
    assert(status.first_fault_error == ESP_ERR_TIMEOUT && fault_us == hw.time_us);
    assert(status.read_seen && status.last_read_error == ESP_ERR_TIMEOUT && status.last_read_us == hw.time_us);
    hw.time_us += 10000;
    imu_latch_fault(ESP_FAIL, "later_fault");
    imu_cleanup(true);
    status = board_imu_status();
    assert(status.first_fault_us == fault_us && status.first_fault_error == ESP_ERR_TIMEOUT);
    assert(strcmp(status.first_fault_stage, "cache_age") == 0 && status.sample_count == 1);
    assert(status.initialization_succeeded && status.start_succeeded && status.error == ESP_FAIL);
    assert(status.last_read_error == ESP_ERR_TIMEOUT && hw.status_reads == reads);

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    s_imu_status.sample_count = UINT64_MAX;
    hw.time_us += 10000;
    assert(imu_update() == ESP_OK);
    status = board_imu_status();
    assert(status.sample_count == UINT64_MAX && status.sample_count_saturated && status.available);
    hw.time_us = status.last_sample_us - 1;
    status = board_imu_status();
    assert(!status.sample_age_known && !status.sample_fresh && status.sample_age_us == 0);
    assert(!status.fault_latched && status.available);
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
        case 3: s_imu_gyro_scale = NAN; expected = ESP_ERR_INVALID_RESPONSE; break;
        case 4: s_imu_accel_scale = INFINITY; expected = ESP_ERR_INVALID_RESPONSE; break;
        case 5: hw.during_gyro_read = stale_consumer_during_bus_read; expected = ESP_ERR_TIMEOUT; break;
        }
        assert(imu_update() == expected);
        assert(!board_capabilities().imu);
        assert_read_fails_untouched(expected);
        if (fault == 5) {
            const board_imu_status_t status = board_imu_status();
            assert(strcmp(status.first_fault_stage, "cache_age") == 0 && status.sample_count == 1);
            assert(status.last_hardware_error == ESP_ERR_TIMEOUT);
            assert(strcmp(status.last_hardware_stage, "batch_time") == 0); /* Later batch failure retains the cache fault. */
        }
        hw.gyro_error = hw.accel_error = ESP_OK;
        hw.status_error = 0;
        if (fault == 0 || fault == 1 || fault == 5) {
            hw.time_us += 10000;
            assert(imu_update() == ESP_OK && board_capabilities().imu);
            assert(board_imu_status().recovery_count == 1);
        } else assert(imu_update() == expected && !board_capabilities().imu);
    }
    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    hw.ready_mask = BMI2_DRDY_GYR; /* Acceleration has no fresh conversion. */
    hw.time_us += 50001;
    assert(imu_update() == ESP_ERR_TIMEOUT && !board_capabilities().imu);

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us = s_imu_sample.timestamp_us - 100;
    assert(imu_update() == ESP_ERR_TIMEOUT && !board_capabilities().imu);
    assert(!strcmp(board_imu_status().last_hardware_stage, "clock"));

    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us = s_imu_sample.timestamp_us; hw.read_delay_us = 0;
    assert(imu_update() == ESP_ERR_INVALID_STATE && !board_capabilities().imu);
    assert(board_imu_status().phase == BOARD_IMU_FATAL &&
        !strcmp(board_imu_status().first_fault_stage, "sample_time"));
}

static void test_late_batch_discard_preserves_cache_and_recovers(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    board_imu_sample_t accepted, raw;
    assert(board_imu_read(&accepted) == ESP_OK);
    memcpy(&raw, &s_imu_sample, sizeof(raw));
    hw.time_us = accepted.timestamp_us + 1000;
    hw.read_delay_us = 10001; /* 20002 us total, just outside coherence. */
    hw.gyro[0] = hw.gyro[1] = hw.gyro[2] = 62.5f;
    hw.accel[0] = hw.accel[1] = hw.accel[2] = 0.5f;
    assert(imu_update() == ESP_OK);
    assert(!memcmp(&raw, &s_imu_sample, sizeof(raw)));
    board_imu_status_t status = board_imu_status();
    assert(status.available && status.sample_fresh && !status.fault_latched && status.error == ESP_OK);
    assert(status.sample_count == 1 && status.late_batch_count == 1 && !status.late_batch_count_saturated);
    assert(status.last_hardware_error == ESP_ERR_TIMEOUT && !strcmp(status.last_hardware_stage, "batch_time"));
    board_imu_sample_t repeated; assert(board_imu_read(&repeated) == ESP_OK);
    assert(repeated.timestamp_us == accepted.timestamp_us && repeated.x_dps == accepted.x_dps &&
        repeated.y_g == accepted.y_g && repeated.z_g == accepted.z_g);
    assert(!hw.imu_stops && !hw.imu_deinits);
    hw.read_delay_us = 10; hw.time_us += 10000;
    assert(imu_update() == ESP_OK);
    status = board_imu_status();
    assert(status.sample_count == 2 && status.late_batch_count == 1 && !status.fault_latched);
    assert(board_imu_read(&repeated) == ESP_OK && repeated.timestamp_us > accepted.timestamp_us && repeated.z_dps == 62.5f);
}

static void test_late_batch_exact_coherence_and_freshness_boundaries(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    const uint64_t first = s_imu_sample.timestamp_us;
    hw.time_us = first + 1000; hw.read_delay_us = 10000;
    assert(imu_update() == ESP_OK); /* Exactly 20 ms is a coherent batch. */
    board_imu_status_t status = board_imu_status();
    assert(status.sample_count == 2 && !status.late_batch_count && status.last_sample_us == first + 21000);
    for (unsigned excess = 0; excess < 2; ++excess) {
        reset(); assert(board_init(legacy_hold) == ESP_OK);
        const uint64_t timestamp = s_imu_sample.timestamp_us;
        hw.time_us = timestamp + 28000 + excess; hw.read_delay_us = 11000;
        assert(imu_update() == (excess ? ESP_ERR_TIMEOUT : ESP_OK));
        status = board_imu_status();
        assert(status.sample_count == 1 && status.late_batch_count == 1 && status.last_sample_us == timestamp);
        assert(status.sample_age_us == 50000 + excess && status.available == !excess);
        if (!excess) {
            board_imu_sample_t sample; assert(board_imu_read(&sample) == ESP_OK && sample.timestamp_us == timestamp);
            ++hw.time_us; assert_read_fails_untouched(ESP_ERR_TIMEOUT);
        } else assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    }
}

static void rollback_during_gyro_read(void)
{
    hw.time_us -= 30000;
}

static void test_late_discard_never_hides_other_faults_or_overflows(void)
{
    for (unsigned fault = 0; fault < 5; ++fault) {
        reset(); assert(board_init(legacy_hold) == ESP_OK);
        hw.time_us = s_imu_sample.timestamp_us + 10000; hw.read_delay_us = 11000;
        switch (fault) {
        case 0: hw.gyro_error = ESP_ERR_TIMEOUT; hw.read_delay_us = 25000; break;
        case 1: hw.accel_error = ESP_ERR_TIMEOUT; break;
        case 2: hw.during_gyro_read = rollback_during_gyro_read; break;
        case 3: s_imu_gyro_scale = NAN; break;
        case 4: s_imu_sample_valid = false; break;
        }
        const esp_err_t expected = fault < 2 ? ESP_FAIL : ESP_ERR_TIMEOUT;
        assert(imu_update() == expected && !board_capabilities().imu);
        const board_imu_status_t status = board_imu_status();
        assert(status.fault_latched && status.sample_count == 1 && status.late_batch_count == (fault == 4 ? 1U : 0U));
        assert_read_fails_untouched(expected);
    }
    reset(); hw.read_delay_us = 11000;
    assert(board_init(legacy_hold) == ESP_OK && !board_capabilities().imu);
    board_imu_status_t status = board_imu_status();
    assert(status.sample_count == 0 && status.late_batch_count == 1 && status.fault_latched);
    assert(!strcmp(status.first_fault_stage, "batch_time")); /* No coherent initial sample to retain. */

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us = s_imu_sample.timestamp_us + 1000; hw.read_delay_us = 11000;
    s_imu_status.late_batch_count = UINT64_MAX;
    assert(imu_update() == ESP_OK);
    status = board_imu_status();
    assert(status.late_batch_count == UINT64_MAX && status.late_batch_count_saturated && status.sample_count == 1);
    assert(status.available && !status.fault_latched);

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us += 10000; hw.read_delay_us = 11000;
    hw.during_gyro_read = stale_consumer_during_bus_read;
    assert(imu_update() == ESP_ERR_TIMEOUT && !board_capabilities().imu);
    status = board_imu_status();
    assert(status.late_batch_count == 1 && status.sample_count == 1 && !strcmp(status.first_fault_stage, "cache_age"));
}

static void test_repeated_late_batches_expire_on_normal_worker_schedule(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    const uint64_t timestamp = s_imu_sample.timestamp_us;
    hw.time_us = timestamp; hw.read_delay_us = 11000;
    hw.worker_running = true; hw.stop_sensor_after_delays = 4;
    if (setjmp(worker_exit) == 0) {
        hw.imu_entry(NULL);
        assert(!"Bounded worker fixture must yield");
    }
    hw.worker_running = false;
    const board_imu_status_t status = board_imu_status();
    assert(status.late_batch_count == 3 && status.sample_count == 1 && status.last_sample_us == timestamp);
    assert(hw.delay_calls == 4 && hw.time_us - timestamp >= 100000);
    assert(status.fault_latched && !status.available && status.worker_running && status.phase == BOARD_IMU_RECOVERING);
    assert(hw.imu_stops == 0 && hw.imu_deinits == 0);
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    hw.read_delay_us = 10; hw.time_us += 10000;
    assert(imu_update() == ESP_OK && board_capabilities().imu && board_imu_status().recovery_count == 1);
}

static void test_sensor_worker_cleans_up_after_fault(void)
{
    for (unsigned cleanup_fails = 0; cleanup_fails < 2; ++cleanup_fails) {
        reset();
        hw.gauge_add_error = ESP_FAIL; /* No remaining battery owner after fatal sensor cleanup. */
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
        const board_imu_status_t status = board_imu_status();
        assert(status.worker_created && !status.worker_running && status.fault_latched);
        assert(strcmp(status.first_fault_stage, "frame") == 0 && status.first_fault_error == ESP_FAIL);
        assert(!status.data_ready_known && status.last_hardware_error == ESP_FAIL);
    }
}

static void transient_then_fresh_frame(void)
{
    if (hw.delay_calls == 1) {
        hw.read_delay_us = 30000; /* Full burst returns after the old cache expired. */
        hw.gyro[2] = 62.5f;
    } else if (hw.delay_calls == 2) {
        const uint64_t generation = s_imu_generation;
        const uint64_t timestamp = s_imu_sample.timestamp_us;
        for (unsigned i = 0; i < 100; ++i) assert_read_fails_untouched(ESP_ERR_TIMEOUT);
        assert(s_imu_generation == generation && s_imu_sample.timestamp_us == timestamp);
        assert(board_imu_status().phase == BOARD_IMU_RECOVERING && board_imu_status().sample_count == 1);
        hw.read_delay_us = 10;
    }
}

static void run_sensor_until(unsigned delays)
{
    hw.worker_running = true; hw.stop_sensor_after_delays = delays;
    if (setjmp(worker_exit) == 0) {
        hw.imu_entry(NULL);
        assert(!"Controlled sensor schedule must yield to its fixture");
    }
    hw.worker_running = false;
}

static void test_worker_recovers_without_reinitialization_or_old_output(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    const uint64_t first = s_imu_sample.timestamp_us;
    hw.time_us = first; hw.on_sensor_delay = transient_then_fresh_frame;
    run_sensor_until(5);
    board_imu_status_t status = board_imu_status();
    assert(status.worker_running && status.phase == BOARD_IMU_ACTIVE && status.available);
    assert(status.sample_count == 4 && status.recovery_count == 1 && status.late_batch_count == 1);
    assert(status.last_sample_us > first && status.sample_fresh && status.failure_streak == 0);
    assert(status.fault_latched && !strcmp(status.first_fault_stage, "batch_time"));
    assert(hw.imu_inits == 1 && hw.imu_starts == 1 && !hw.imu_stops && !hw.imu_deinits);
    board_imu_sample_t sample; assert(board_imu_read(&sample) == ESP_OK && sample.z_dps == 62.5f);
}

static void test_persistent_transport_failure_has_capped_fair_backoff(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us = s_imu_sample.timestamp_us; hw.gyro_error = ESP_FAIL;
    hw.record_sensor_attempts = true;
    run_sensor_until(16);
    const board_imu_status_t status = board_imu_status();
    assert(status.phase == BOARD_IMU_RECOVERING && status.worker_running && !status.available);
    assert(status.sample_count == 1 && status.failure_streak == 15 && status.retry_delay_ms == 250);
    assert(hw.sensor_attempt_count == 15 && hw.gauge_polls == 3);
    const uint32_t periods_ms[] = {10, 20, 40, 80, 160, 250};
    for (unsigned i = 1; i < hw.sensor_attempt_count; ++i) {
        const uint32_t period = periods_ms[i - 1 < 6 ? i - 1 : 5];
        const uint64_t delta = hw.sensor_attempt_times[i] - hw.sensor_attempt_times[i - 1];
        assert(delta >= period * 1000U && delta <= period * 1000U + 1000U);
    }
    assert(!hw.imu_stops && !hw.imu_deinits && hw.gauge_adds == 1);
    assert_read_fails_untouched(ESP_FAIL);
    hw.gyro_error = ESP_OK; hw.time_us += 10000;
    assert(imu_update() == ESP_OK && board_imu_status().phase == BOARD_IMU_ACTIVE);
    assert(board_imu_status().failure_streak == 0 && board_imu_status().retry_delay_ms == 10);
}

static void expire_cache_then_corrupt_scale(void)
{
    stale_consumer_during_bus_read();
    s_imu_accel_scale = INFINITY;
}

static void test_fatal_outcomes_win_over_transient_generation_and_early_exit(void)
{
    for (unsigned not_ready = 0; not_ready < 2; ++not_ready) {
        reset(); assert(board_init(legacy_hold) == ESP_OK);
        hw.time_us = s_imu_sample.timestamp_us - 1000;
        if (not_ready) hw.ready_mask = 0;
        else hw.gyro_error = ESP_FAIL;
        assert(imu_update() == ESP_ERR_TIMEOUT);
        assert(board_imu_status().phase == BOARD_IMU_FATAL &&
            !strcmp(board_imu_status().last_hardware_stage, "clock"));
    }
    for (unsigned not_ready = 0; not_ready < 2; ++not_ready) {
        reset(); assert(board_init(legacy_hold) == ESP_OK);
        hw.time_us = s_imu_sample.timestamp_us + 10000;
        hw.read_delay_us = 11000; hw.during_gyro_read = rollback_during_gyro_read;
        if (not_ready) hw.ready_mask = 0;
        else hw.gyro_error = ESP_FAIL;
        assert(imu_update() == ESP_ERR_TIMEOUT);
        const board_imu_status_t status = board_imu_status();
        assert(status.phase == BOARD_IMU_FATAL && !status.available && !strcmp(status.last_hardware_stage, "clock"));
        unsigned reads = hw.status_reads;
        assert(imu_update() == ESP_ERR_TIMEOUT && hw.status_reads == reads);
    }
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.time_us += 10000; hw.during_gyro_read = expire_cache_then_corrupt_scale;
    assert(imu_update() == ESP_ERR_TIMEOUT); /* Both late and nonfinite is fatal. */
    assert(board_imu_status().phase == BOARD_IMU_FATAL && !board_capabilities().imu);
    assert(!strcmp(board_imu_status().first_fault_stage, "cache_age")); /* History retained. */

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    s_imu_config.accel_range = 0xff;
    assert(imu_update() == ESP_ERR_INVALID_STATE && board_imu_status().phase == BOARD_IMU_FATAL);
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.sensor_initialized = false;
    assert(imu_update() == ESP_ERR_INVALID_STATE && board_imu_status().phase == BOARD_IMU_FATAL);
}

static void consume_expiring_cache_during_coherent_frame(void)
{
    const uint64_t generation = s_imu_generation;
    assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    assert(s_imu_generation == generation + 1);
    for (unsigned i = 0; i < 100; ++i) assert_read_fails_untouched(ESP_ERR_TIMEOUT);
    assert(s_imu_generation == generation + 1);
}

static void test_inflight_coherent_frame_cannot_cross_loss_generation(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    const uint64_t timestamp = s_imu_sample.timestamp_us;
    hw.time_us = timestamp + 49990; hw.read_delay_us = 10;
    hw.during_gyro_read = consume_expiring_cache_during_coherent_frame;
    assert(imu_update() == ESP_ERR_TIMEOUT && board_imu_status().sample_count == 1);
    assert(board_imu_status().last_hardware_error == ESP_OK && !board_capabilities().imu);
    hw.time_us += 10000;
    s_imu_status.recovery_count = UINT64_MAX;
    assert(imu_update() == ESP_OK && board_capabilities().imu);
    assert(board_imu_status().sample_count == 2 && s_imu_sample.timestamp_us > timestamp);
    assert(board_imu_status().recovery_count == UINT64_MAX && board_imu_status().recovery_count_saturated);

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    s_imu_generation = UINT64_MAX;
    hw.time_us = s_imu_sample.timestamp_us + 50001;
    assert_read_fails_untouched(ESP_ERR_INVALID_STATE);
    assert(board_imu_status().phase == BOARD_IMU_FATAL && !strcmp(board_imu_status().first_fault_stage, "generation"));
}

static void queue_brightness_during_sensor_schedule(void)
{
    assert(board_brightness_set((int)(hw.delay_calls % 101)) == ESP_OK);
}

static void assert_power_unknown(const board_power_status_t *status);
static void test_single_owner_schedules_long_battery_without_sensor_starvation(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.gauge_read_delay_us = 1000; hw.gauge_after_read_delay_us = 18000;
    hw.record_sensor_attempts = true;
    hw.on_sensor_delay = queue_brightness_during_sensor_schedule;
    run_sensor_until(270);
    const board_imu_status_t status = board_imu_status();
    assert(status.phase == BOARD_IMU_ACTIVE && status.worker_running && status.available && status.sample_fresh);
    assert(status.sample_count == 270 && hw.sensor_attempt_count == 269);
    assert(hw.gauge_polls == 3 && hw.gauge_reads == 7 && hw.gauge_adds == 1);
    assert(status.max_schedule_late_us >= 28000 && !hw.imu_stops && !hw.imu_deinits);
    assert(hw.brightness_writes == 0 && hw.notifications > 250); /* No display lock in sensor owner. */
    assert(hw.power_warnings == 1 && strstr(hw.power_warning, "stage=batch_time") &&
        strstr(hw.power_warning, "command=0x06") && strstr(hw.power_warning, "packet_us=19000"));
    for (unsigned i = 1; i < hw.gauge_polls; ++i)
        assert(hw.gauge_poll_times[i] - hw.gauge_poll_times[i - 1] >= 1000000);
    for (unsigned i = 1; i < hw.sensor_attempt_count; ++i) {
        const uint64_t ticks = hw.sensor_attempt_times[i] / 1000U - hw.sensor_attempt_times[i - 1] / 1000U;
        if (ticks < 10) fprintf(stderr, "Sensor cadence index=%u previous=%llu current=%llu ticks=%llu\n", i,
            (unsigned long long)hw.sensor_attempt_times[i - 1], (unsigned long long)hw.sensor_attempt_times[i],
            (unsigned long long)ticks);
        assert(ticks >= 10); /* Real 1 kHz scheduling is quantized to ticks. */
    }
    board_power_status_t power;
    assert(board_power_read(&power) == ESP_ERR_TIMEOUT); assert_power_unknown(&power);
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
        assert(sample.timestamp_us > previous_us && sample.z_g == 1.0f);
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
    const unsigned packets_to_fail[] = {1, 2, 2, 2, 2, 3};
    board_power_status_t status;
    for (size_t i = 0; i < sizeof(commands); ++i) {
        reset();
        hw.gauge_fail_command = commands[i];
        assert(board_init(legacy_hold) == ESP_OK);
        assert(board_power_read(&status) == ESP_FAIL);
        assert_power_unknown(&status);
        assert(hw.gauge_reads == packets_to_fail[i] && !board_capabilities().battery);
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
    assert(board_power_read(&status) == ESP_OK && hw.gauge_reads == 3);
    assert(hw.gauge_gap_delays == 3 && !hw.power_warnings);
    assert(status.percent_valid && status.percent == 63 && status.current_ma == -12);

    reset();
    hw.gauge_read_delay_us = 4000; hw.gauge_after_read_delay_us = 3000;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT); assert_power_unknown(&status);
    assert(hw.gauge_reads == 3); /* The unchanged 20 ms gate rejects a late all-success batch. */
    assert(hw.power_warnings == 1 && strstr(hw.power_warning, "stage=batch_time") &&
        strstr(hw.power_warning, "command=0x2c") && strstr(hw.power_warning, "batch_us=21300"));

    reset();
    hw.fail_power_task = true;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_OK && board_capabilities().battery);
    assert(!board_capabilities().brightness && hw.gauge_removes == 0 && s_gauge != NULL);
    assert(board_brightness_set(50) == ESP_ERR_INVALID_STATE);

    reset();
    hw.fail_imu_task = true;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_NO_MEM);
    assert_power_unknown(&status);
    assert(board_capabilities().brightness && !board_capabilities().battery);
    assert(hw.gauge_removes == 1 && s_gauge == NULL);
    assert(board_brightness_set(50) == ESP_OK);

    reset();
    hw.fail_imu_task = true;
    hw.gauge_cleanup_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_power_read(&status) == ESP_ERR_NO_MEM && !board_capabilities().battery);
    assert_power_unknown(&status);
    assert(hw.gauge_removes == 1 && s_gauge != NULL); /* Preserve failed-cleanup ownership. */
}

static void rollback_during_gauge_read(void)
{
    hw.time_us -= 1000;
}

static void finish_power_batch_at_boundary(void)
{
    /* Three packets include 300 us gap and 30 us wire time by default. */
    hw.gauge_after_read_delay_us = 0;
    hw.time_us += 19670;
}

static void test_battery_packet_contract_and_truthful_failure_stages(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    board_power_status_t status;
    assert(hw.gauge_reads == 3 && hw.gauge_gap_delays == 3);
    hw.gauge_read_error = ESP_ERR_TIMEOUT;
    power_update();
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT && !status.gauge_available && !status.percent_valid);
    assert(strstr(hw.power_warning, "stage=read") && strstr(hw.power_warning, "command=0x3a") &&
        strstr(hw.power_warning, "batch_us_known=1"));
    unsigned reads = hw.gauge_reads;
    hw.gauge_read_error = ESP_OK; power_update();
    assert(board_power_read(&status) == ESP_OK && hw.gauge_reads == reads + 3);
    hw.gauge_read_delay_us = 6001;
    power_update();
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT && strstr(hw.power_warning, "stage=read"));
    assert_power_unknown(&status);

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.during_gauge_read = rollback_during_gauge_read;
    power_update();
    assert(board_power_read(&status) == ESP_ERR_TIMEOUT && strstr(hw.power_warning, "stage=clock") &&
        strstr(hw.power_warning, "batch_us_known=0"));
    assert_power_unknown(&status);

    reset(); assert(board_init(legacy_hold) == ESP_OK);
    hw.during_gauge_read = finish_power_batch_at_boundary;
    power_update();
    assert(board_power_read(&status) == ESP_OK); /* Exactly 20 ms remains valid. */
    assert(status.current_ma == -12 && status.voltage_mv == 3850 && status.percent == 63);
    assert(!hw.power_warnings);
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
    assert(hw.gauge_polls == 1 && hw.gauge_reads == 3); /* Brightness cannot start sensor IO. */
    assert(hw.brightness_writes >= 90 && hw.brightness_writes <= 101);
    assert(hw.gauge_adds == 1 && hw.task_creations == 3);
    for (unsigned i = 1; i < hw.gauge_polls; ++i) {
        assert(hw.gauge_poll_times[i] - hw.gauge_poll_times[i - 1] >= 1000000);
    }
}

static void test_display_observer_installs_before_adapter(void)
{
    reset();
    assert(board_init(legacy_hold) == ESP_OK && hw.observer_installs == 1 && hw.display_started);
    reset(); hw.display_new_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_FAIL && !hw.observer_installs && !hw.display_started);
    reset(); hw.observer_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK && hw.display_started);
}

static void haptic_step(uint64_t time_ms, uint64_t *generation)
{
    hw.time_us = time_ms * 1000U;
    haptics_service(generation);
}

static void test_haptic_diagnostic_actual_receipts(void)
{
    reset();
    uint64_t request_id = 99, applied = 0;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_ERR_INVALID_STATE && request_id == 99);
    assert(board_init(legacy_hold) == ESP_OK);
    haptic_step(1000, &applied);
    const board_feedback_t patterns[] = {BOARD_FEEDBACK_CONFIRM, BOARD_FEEDBACK_MODE, BOARD_FEEDBACK_LOCK, BOARD_FEEDBACK_ERROR};
    uint64_t previous_id = 0;
    for (size_t p = 0; p < sizeof(patterns)/sizeof(patterns[0]); ++p) {
        uint64_t start = 1500 + p * 500;
        hw.time_us = start * 1000;
        unsigned writes = hw.motor_writes;
        assert(board_haptics_request(BOARD_HAPTIC_TEST, patterns[p], &request_id) == ESP_OK && request_id > previous_id);
        previous_id = request_id;
        board_haptic_status_t status = board_haptics_status();
        assert(status.state == BOARD_HAPTIC_PENDING && !status.on_confirmed && hw.motor_writes == writes);
        assert(board_haptics_request(BOARD_HAPTIC_TEST, patterns[p], &request_id) == ESP_ERR_INVALID_STATE);
        haptic_step(start, &applied);
        status = board_haptics_status();
        assert(status.state == BOARD_HAPTIC_ACTIVE && status.on_confirmed && status.started_ms == start);
        for (uint64_t i = 1; i <= 20; ++i) haptic_step(start + i * 10, &applied);
        status = board_haptics_status();
        assert(status.request_id == previous_id && status.state == BOARD_HAPTIC_COMPLETED && status.on_confirmed && status.off_confirmed);
        assert(!board_haptics_active() && hw.motor_strength == 0 && hw.motor_writes > writes);
        writes = hw.motor_writes; (void)board_haptics_status(); (void)board_haptics_status();
        assert(hw.motor_writes == writes);
    }
    hw.time_us = 4000000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_OK);
    unsigned writes = hw.motor_writes;
    haptic_step(4101, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_NO_EFFECT && !board_haptics_status().on_confirmed && hw.motor_writes == writes);
    hw.time_us = 4500000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_OK);
    haptic_step(4500, &applied); haptic_step(4530, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_COMPLETED);
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_MODE, &request_id) == ESP_OK);
    haptic_step(4531, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_NO_EFFECT && !board_haptics_status().on_confirmed);
    hw.time_us = 5000000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_OK);
    haptic_step(5000, &applied); board_haptics_enable(false); haptic_step(5001, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_CANCELLED && board_haptics_status().off_confirmed);
    board_haptics_enable(true); haptic_step(5500, &applied);
    hw.time_us = 5800000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_OK);
    hw.motor_error = ESP_FAIL; haptic_step(5800, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_UNCERTAIN && !board_haptics_status().off_confirmed && !board_capabilities().haptics);
    hw.motor_error = ESP_OK;
    assert(board_haptics_request(BOARD_HAPTIC_RECOVER, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_OK);
    haptic_step(5801, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_COMPLETED && !board_haptics_status().on_confirmed && board_haptics_status().off_confirmed);
    assert(!board_haptics_enabled() && board_capabilities().haptics);
    board_haptics_enable(true); haptic_step(6200, &applied);
    hw.time_us = 6500000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_ERROR, &request_id) == ESP_OK);
    writes = hw.motor_writes;
    assert(board_haptics_cancel(request_id + 1) == ESP_ERR_INVALID_STATE);
    assert(board_haptics_cancel(request_id) == ESP_OK && hw.motor_writes == writes && board_haptics_enabled());
    haptic_step(6501, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_CANCELLED && !board_haptics_status().on_confirmed && board_haptics_status().off_confirmed);
    hw.time_us = 7000000;
    assert(board_haptics_request(BOARD_HAPTIC_TEST, BOARD_FEEDBACK_ERROR, &request_id) == ESP_OK);
    haptic_step(7000, &applied); writes = hw.motor_writes;
    assert(board_haptics_status().state == BOARD_HAPTIC_ACTIVE && board_haptics_cancel(request_id) == ESP_OK && hw.motor_writes == writes);
    haptic_step(7001, &applied);
    assert(board_haptics_status().state == BOARD_HAPTIC_CANCELLED && board_haptics_status().on_confirmed && board_haptics_status().off_confirmed && board_haptics_enabled());
    s_haptic_status.request_id = UINT64_MAX;
    assert(board_haptics_request(BOARD_HAPTIC_RECOVER, BOARD_FEEDBACK_CONFIRM, &request_id) == ESP_ERR_INVALID_STATE);
}

static board_imu_frame_t stream_frames[BOARD_IMU_STREAM_CAPACITY];
static board_imu_batch_t stream_batch;
static board_imu_consumer_t stream_consumer;
static uint64_t stream_fenced_epoch;

static void stream_drain(void)
{
    const unsigned reads = hw.gyro_reads;
    assert(board_imu_stream_drain(stream_consumer, stream_frames, BOARD_IMU_STREAM_CAPACITY, &stream_batch) == ESP_OK);
    assert(hw.gyro_reads == reads && hw.critical_depth == 0);
    assert(stream_batch.count <= BOARD_IMU_STREAM_CAPACITY);
}

static void stream_start(void)
{
    reset();
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_imu_stream_attach(&stream_consumer) == ESP_OK);
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_ATTACH && stream_batch.latest_sequence == 1);
}

static void test_ordered_stream_preserves_cache_units_and_identity(void)
{
    reset();
    board_imu_consumer_t untouched = 99;
    assert(board_imu_stream_attach(&untouched) == ESP_ERR_INVALID_STATE && untouched == 99);
    assert(board_imu_stream_attach(NULL) == ESP_ERR_INVALID_ARG);
    stream_start();
    assert(board_imu_stream_attach(&untouched) == ESP_ERR_INVALID_STATE && untouched == 99);
    for (unsigned i = 0; i < 3; ++i) {
        hw.time_us += 10000; hw.gyro[0] += 1; hw.accel[1] += 0.01f;
        assert(imu_update() == ESP_OK);
    }
    board_imu_sample_t latest;
    assert(board_imu_read(&latest) == ESP_OK);
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DATA && stream_batch.count == 3 && !stream_batch.dropped_frames);
    for (unsigned i = 0; i < 3; ++i) {
        assert(stream_frames[i].sequence == i + 2 && stream_frames[i].delivery_epoch == stream_batch.delivery_epoch);
        if (i) assert(stream_frames[i].sample.timestamp_us > stream_frames[i-1].sample.timestamp_us);
    }
    assert(memcmp(&stream_frames[2].sample, &latest, sizeof(latest)) == 0);
    stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_EMPTY);
    s_imu_status.sample_count = UINT64_MAX;
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    stream_drain();
    assert(stream_batch.count == 1 && stream_frames[0].sequence == 5 && s_imu_status.sample_count_saturated);
    /* Invalid queries must neither overwrite outputs nor latch source expiry. */
    const board_imu_batch_t saved_batch = stream_batch;
    const board_imu_frame_t saved_frame = stream_frames[0];
    hw.time_us += 60000;
    assert(board_imu_stream_drain(stream_consumer + 1, stream_frames, 8, &stream_batch) == ESP_ERR_INVALID_STATE);
    assert(board_imu_stream_drain(stream_consumer, stream_frames, 0, &stream_batch) == ESP_ERR_INVALID_ARG);
    assert(board_imu_stream_drain(stream_consumer, stream_frames, 9, &stream_batch) == ESP_ERR_INVALID_ARG);
    assert(board_imu_stream_drain(stream_consumer, NULL, 8, &stream_batch) == ESP_ERR_INVALID_ARG);
    assert(memcmp(&saved_batch, &stream_batch, sizeof(stream_batch)) == 0 &&
        memcmp(&saved_frame, &stream_frames[0], sizeof(saved_frame)) == 0 && s_caps.imu);
}

static void fence_during_imu_read(void)
{
    assert(board_imu_stream_fence(stream_consumer, &stream_fenced_epoch) == ESP_OK);
}

static void replace_consumer_during_imu_read(void)
{
    const board_imu_consumer_t previous = stream_consumer;
    assert(board_imu_stream_detach(previous) == ESP_OK);
    assert(board_imu_stream_attach(&stream_consumer) == ESP_OK && stream_consumer != previous);
    assert(board_imu_stream_detach(previous) == ESP_ERR_INVALID_STATE);
}

static void test_ordered_stream_inflight_fence_is_delivery_only(void)
{
    stream_start();
    hw.time_us += 10000; assert(imu_update() == ESP_OK); /* Queued pre-fence frame. */
    const uint64_t hardware_generation = s_imu_generation;
    hw.time_us += 10000; hw.during_gyro_read = fence_during_imu_read;
    assert(imu_update() == ESP_OK && s_caps.imu && s_imu_generation == hardware_generation);
    board_imu_sample_t latest; assert(board_imu_read(&latest) == ESP_OK);
    assert(latest.timestamp_us == hw.time_us && !s_imu_status.fault_latched);
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.count == 0 &&
        stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_FENCE && stream_batch.dropped_frames == 2);
    assert(stream_batch.latest_sequence == 3 && stream_batch.delivery_epoch == stream_fenced_epoch);
    stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_EMPTY);
    hw.time_us += 10000; assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.count == 1 && stream_frames[0].sequence == 4 && stream_frames[0].delivery_epoch == stream_fenced_epoch);
    hw.time_us += 10000; hw.during_gyro_read = replace_consumer_during_imu_read;
    assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.dropped_frames == 3 &&
        (stream_batch.discontinuities & BOARD_IMU_DISCONTINUITY_ATTACH));
    hw.time_us += 10000; assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.count == 1 && stream_frames[0].sequence == 6);
}

static void test_ordered_stream_loss_and_hardware_health_are_distinct(void)
{
    stream_start();
    for (unsigned i = 0; i < 9; ++i) { hw.time_us += 1000; assert(imu_update() == ESP_OK); }
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_OVERFLOW && stream_batch.dropped_frames == 9);
    assert(s_caps.imu && !s_imu_status.fault_latched && !s_imu_generation && stream_batch.source_error == ESP_OK);
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    hw.time_us += 50001; assert(imu_update() == ESP_OK); /* Oldest stale, newest fresh. */
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_STALE && stream_batch.dropped_frames == 11);
    assert(s_caps.imu && !s_imu_status.fault_latched && !s_imu_generation);
    hw.time_us += 1000; hw.read_delay_us = 10001;
    const uint64_t accepted = s_imu_status.sample_count;
    assert(imu_update() == ESP_OK); /* Legacy cache is still fresh; source attempt was rejected. */
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_REJECTED && stream_batch.latest_sequence == accepted);
    assert(s_caps.imu && !s_imu_status.fault_latched && stream_batch.source_error == ESP_OK);
    hw.read_delay_us = 10; hw.ready_mask = BMI2_DRDY_ACC;
    assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.discontinuities == BOARD_IMU_DISCONTINUITY_REJECTED && s_caps.imu);
    hw.ready_mask = BMI2_DRDY_GYR | BMI2_DRDY_ACC;
    hw.gyro_error = ESP_FAIL;
    assert(imu_update() == ESP_FAIL); stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY &&
        (stream_batch.discontinuities & BOARD_IMU_DISCONTINUITY_SOURCE) && stream_batch.source_error == ESP_FAIL);
    const uint64_t fault_us = s_imu_status.first_fault_us;
    stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_UNAVAILABLE && !stream_batch.count);
    hw.gyro_error = ESP_OK; hw.time_us += 10000; assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.count == 1 && stream_batch.source_error == ESP_OK && s_caps.imu);
    assert(s_imu_status.first_fault_us == fault_us && s_imu_status.first_fault_error == ESP_FAIL);
    /* Diagnostic stream observes expiry without latching production loss. */
    hw.time_us = s_imu_sample.timestamp_us + 50001; stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_UNAVAILABLE && stream_batch.source_error == ESP_ERR_TIMEOUT && s_caps.imu);
    assert(s_imu_status.first_fault_us == fault_us);
}

static void test_ordered_stream_stale_and_clock_observation_cannot_change_source(void)
{
    for (unsigned clock = 0; clock < 2; ++clock) {
        stream_start();
        hw.time_us += 10000; assert(imu_update() == ESP_OK);
        board_imu_sample_t latest; assert(board_imu_read(&latest) == ESP_OK);
        const board_capabilities_t caps = s_caps;
        const board_imu_status_t status = s_imu_status;
        const board_imu_sample_t cache = s_imu_sample;
        const bool valid = s_imu_sample_valid;
        const uint64_t generation = s_imu_generation;
        const esp_err_t source_error = s_imu_error;
        const unsigned reads = hw.gyro_reads;
        hw.time_us = clock ? latest.timestamp_us - 1 : latest.timestamp_us + 50001;
        stream_drain();
        assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.source_error == ESP_ERR_TIMEOUT && !stream_batch.count);
        assert(stream_batch.discontinuities == (BOARD_IMU_DISCONTINUITY_SOURCE |
            (clock ? BOARD_IMU_DISCONTINUITY_CLOCK : BOARD_IMU_DISCONTINUITY_STALE)));
        stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_UNAVAILABLE);
        assert(memcmp(&caps, &s_caps, sizeof(caps)) == 0 && memcmp(&status, &s_imu_status, sizeof(status)) == 0);
        assert(memcmp(&cache, &s_imu_sample, sizeof(cache)) == 0 && s_imu_sample_valid == valid);
        assert(s_imu_error == source_error && s_imu_generation == generation && hw.gyro_reads == reads);
        /* A subsequent legacy read independently owns its original transition. */
        assert(board_imu_read(&latest) == ESP_ERR_TIMEOUT && !s_caps.imu && !s_imu_sample_valid);
        assert(s_imu_status.phase == (clock ? BOARD_IMU_FATAL : BOARD_IMU_RECOVERING));
        assert(s_imu_status.fault_latched && !strcmp(s_imu_status.first_fault_stage, "cache_age"));
        assert(s_imu_status.first_fault_error == ESP_ERR_TIMEOUT);
    }
}

static void test_ordered_stream_failed_source_and_delivery_exhaustion(void)
{
    reset(); hw.imu_init_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK && board_imu_stream_attach(&stream_consumer) == ESP_OK);
    stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.source_error == ESP_FAIL);
    stream_drain(); assert(stream_batch.state == BOARD_IMU_DELIVERY_UNAVAILABLE);
    stream_start();
    s_imu_stream.latest_sequence = UINT64_MAX;
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_FAILED && stream_batch.source_error == ESP_OK &&
        (stream_batch.discontinuities & BOARD_IMU_DISCONTINUITY_ID_EXHAUSTED));
    assert(s_caps.imu && !s_imu_status.fault_latched && s_imu_status.sample_count == 2);
    board_imu_sample_t latest; assert(board_imu_read(&latest) == ESP_OK);
}

static void observe_stale_during_imu_read(void)
{
    stream_drain();
    assert(stream_batch.discontinuities == (BOARD_IMU_DISCONTINUITY_STALE | BOARD_IMU_DISCONTINUITY_SOURCE));
    assert(stream_batch.source_error == ESP_ERR_TIMEOUT && s_caps.imu && !s_imu_status.fault_latched);
}

static void test_ordered_stream_observer_during_acquisition_preserves_cause(void)
{
    stream_start();
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    hw.time_us += 50001; hw.during_gyro_read = observe_stale_during_imu_read;
    assert(imu_update() == ESP_OK && s_caps.imu && !s_imu_generation && !s_imu_status.fault_latched);
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.dropped_frames == 2 &&
        stream_batch.discontinuities == (BOARD_IMU_DISCONTINUITY_STALE | BOARD_IMU_DISCONTINUITY_SOURCE) &&
        stream_batch.source_error == ESP_OK && stream_batch.latest_sequence == 3);
    hw.time_us += 10000; assert(imu_update() == ESP_OK); stream_drain();
    assert(stream_batch.count == 1 && stream_frames[0].sequence == 4);
}

static board_sensor_record_t journal_records[BOARD_SENSOR_JOURNAL_READ_MAX];
static board_sensor_batch_t journal_batch;

static uint64_t journal_word64(const uint32_t *words)
{
    return (uint64_t)words[0] | ((uint64_t)words[1] << 32);
}

static void journal_read(uint64_t after)
{
    const unsigned imu_reads = hw.gyro_reads, gauge_reads = hw.gauge_reads;
    assert(board_sensor_journal_read(after, journal_records, BOARD_SENSOR_JOURNAL_READ_MAX, &journal_batch) == ESP_OK);
    assert(imu_reads == hw.gyro_reads && gauge_reads == hw.gauge_reads && !hw.critical_depth);
}

static void test_raw_journal_retains_samples_around_actual_read_rejections(void)
{
    stream_start();
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK);
    const uint64_t before = journal_batch.latest_sequence;
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    const board_imu_sample_t accepted = s_imu_sample;
    hw.ready_mask = BMI2_DRDY_ACC; hw.time_us += 10000;
    const uint64_t start = hw.time_us;
    assert(imu_update() == ESP_OK && s_caps.imu && !s_imu_status.fault_latched);
    hw.ready_mask = BMI2_DRDY_GYR | BMI2_DRDY_ACC; hw.time_us += 10000;
    assert(imu_update() == ESP_OK);
    journal_read(before);
    assert(journal_batch.state == BOARD_SENSOR_READ_DATA && journal_batch.count == 3);
    assert(journal_records[0].kind == BOARD_SENSOR_IMU_SAMPLE && journal_records[0].source_sequence == 2);
    assert(journal_records[0].sample_us == accepted.timestamp_us && !journal_records[0].source_epoch);
    const float axes[6] = {accepted.x_dps, accepted.y_dps, accepted.z_dps, accepted.x_g, accepted.y_g, accepted.z_g};
    assert(memcmp(journal_records[0].payload, axes, sizeof(axes)) == 0);
    const board_sensor_record_t *rejected = &journal_records[1];
    assert(rejected->kind == BOARD_SENSOR_IMU_REJECTED && rejected->error == ESP_ERR_NOT_FOUND &&
        rejected->discontinuities == BOARD_IMU_DISCONTINUITY_REJECTED && !rejected->source_sequence);
    assert(journal_word64(rejected->payload) == start && journal_word64(rejected->payload + 2) == start + 20 &&
        journal_word64(rejected->payload + 4) == 20 && rejected->sample_us == start + 20);
    assert((rejected->flags >> 8) == BMI2_DRDY_ACC && (rejected->payload[6] & 0xffffU) == (1U | (BMI2_DRDY_ACC << 8)));
    assert(((rejected->payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_NOT_READY &&
        (rejected->payload[6] >> 24) == BOARD_SENSOR_STAGE_DATA_READY && rejected->payload[7] == ESP_OK);
    assert(rejected->validity == (BOARD_SENSOR_VALID_READ_START | BOARD_SENSOR_VALID_READ_END |
        BOARD_SENSOR_VALID_DURATION | BOARD_SENSOR_VALID_SOURCE_ERROR));
    assert(journal_records[2].kind == BOARD_SENSOR_IMU_SAMPLE && journal_records[2].source_sequence == 3);
    /* Existing actuation semantics still discard the pending accepted sample. */
    stream_drain();
    assert(stream_batch.state == BOARD_IMU_DELIVERY_DISCONTINUITY && stream_batch.dropped_frames == 1);
    const uint64_t last = journal_batch.next_after;
    hw.time_us += 1000; hw.read_delay_us = 10001;
    assert(imu_update() == ESP_OK); journal_read(last);
    assert(journal_batch.count == 1 && journal_records[0].error == ESP_ERR_TIMEOUT &&
        ((journal_records[0].payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_LATE &&
        (journal_records[0].payload[6] >> 24) == BOARD_SENSOR_STAGE_BATCH_TIME &&
        journal_word64(journal_records[0].payload + 4) == 20002);
    assert(journal_records[0].payload[7] == ESP_OK && s_imu_status.late_batch_count == 1);
}

static void test_raw_journal_source_epoch_recovery_and_no_observer_mutation(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK);
    const uint64_t before = journal_batch.latest_sequence;
    hw.time_us += 10000; hw.gyro_error = ESP_FAIL;
    assert(imu_update() == ESP_FAIL && s_imu_generation == 1);
    hw.time_us += 10000; hw.gyro_error = ESP_OK;
    assert(imu_update() == ESP_OK);
    const board_imu_status_t status = s_imu_status;
    const board_imu_sample_t sample = s_imu_sample;
    const board_imu_queue_t queue = s_imu_stream;
    const board_sensor_journal_t journal = s_sensor_journal;
    journal_read(before);
    assert(journal_batch.count == 2 && journal_records[0].kind == BOARD_SENSOR_IMU_REJECTED &&
        journal_records[0].error == ESP_FAIL && (int32_t)journal_records[0].payload[7] == ESP_FAIL &&
        journal_records[0].source_epoch == 0 && journal_records[1].source_epoch == 1);
    assert(((journal_records[0].payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_COMMUNICATION);
    assert(journal_records[1].kind == BOARD_SENSOR_IMU_SAMPLE && journal_records[1].source_sequence == 2);
    hw.time_us += 60000; journal_read(before); /* Read stale history without latching source loss. */
    assert(memcmp(&status, &s_imu_status, sizeof(status)) == 0 && memcmp(&sample, &s_imu_sample, sizeof(sample)) == 0 &&
        memcmp(&queue, &s_imu_stream, sizeof(queue)) == 0 && memcmp(&journal, &s_sensor_journal, sizeof(journal)) == 0 && s_caps.imu);
    const board_sensor_batch_t saved = journal_batch;
    const board_sensor_record_t saved_record = journal_records[0];
    assert(board_sensor_journal_read(journal.latest_sequence + 1, journal_records, 8, &journal_batch) == ESP_ERR_INVALID_ARG);
    assert(board_sensor_journal_read(0, journal_records, 9, &journal_batch) == ESP_ERR_INVALID_ARG);
    assert(memcmp(&saved, &journal_batch, sizeof(saved)) == 0 && memcmp(&saved_record, journal_records, sizeof(saved_record)) == 0);
}

static void test_power_observer_is_pure_and_power_journal_has_provenance_fields(void)
{
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    journal_read(0);
    const board_sensor_record_t *power = NULL;
    for (uint32_t i = 0; i < journal_batch.count; ++i) if (journal_records[i].kind == BOARD_SENSOR_POWER) power = &journal_records[i];
    assert(power && power->error == ESP_OK && power->payload[1] == (uint32_t)-12 &&
        power->payload[2] == 3850 && power->payload[3] == 63 && power->payload[4] == 1 && !power->source_epoch);
    float temperature; memcpy(&temperature, power->payload, sizeof(temperature));
    assert(fabsf(temperature - 24.95f) < 0.001f && power->sample_us == s_power.timestamp_us);
    assert(power->validity == (BOARD_SENSOR_VALID_POWER_STATE | BOARD_SENSOR_VALID_POWER_PERCENT |
        BOARD_SENSOR_VALID_POWER_VOLTAGE | BOARD_SENSOR_VALID_POWER_CURRENT | BOARD_SENSOR_VALID_POWER_TEMPERATURE));
    const board_power_status_t cached = s_power;
    const board_capabilities_t caps = s_caps;
    const unsigned reads = hw.gauge_reads;
    board_power_status_t observed;
    hw.time_us = s_power.timestamp_us + POWER_MAX_AGE_US;
    assert(board_power_observe(&observed) == ESP_OK && observed.temperature_valid);
    ++hw.time_us;
    assert(board_power_observe(&observed) == ESP_ERR_TIMEOUT && !observed.temperature_valid && observed.timestamp_us == cached.timestamp_us);
    hw.time_us = s_power.timestamp_us - 1;
    assert(board_power_observe(&observed) == ESP_ERR_TIMEOUT);
    assert(memcmp(&cached, &s_power, sizeof(cached)) == 0 && memcmp(&caps, &s_caps, sizeof(caps)) == 0 && reads == hw.gauge_reads);
    assert(board_power_read(&observed) == ESP_ERR_TIMEOUT && !s_caps.battery); /* Legacy behavior remains independent. */
}

static void test_raw_journal_input_whitelist_redaction_and_exhaustion_isolation(void)
{
    reset();
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK && journal_batch.state == BOARD_SENSOR_READ_UNAVAILABLE);
    board_sensor_record_t input = {.kind = BOARD_SENSOR_BUTTON, .flags = BOARD_SENSOR_FLAG_SAMPLE_TIME,
        .sample_us = 1000, .validity = BOARD_SENSOR_VALID_INPUT};
    assert(board_sensor_journal_publish_input(&input) == ESP_ERR_INVALID_STATE);
    assert(board_init(legacy_hold) == ESP_OK);
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK);
    const uint64_t before = journal_batch.latest_sequence;
    const board_sensor_record_t original = input;
    assert(board_sensor_journal_publish_input(&input) == ESP_OK && memcmp(&input, &original, sizeof(input)) == 0);
    input.kind = BOARD_SENSOR_IMU_SAMPLE; assert(board_sensor_journal_publish_input(&input) == ESP_ERR_INVALID_ARG);
    input.kind = BOARD_SENSOR_TOUCH; input.flags |= BOARD_SENSOR_FLAG_REDACTED; input.validity = 0;
    assert(board_sensor_journal_publish_input(&input) == ESP_OK);
    input.payload[3] = 88; assert(board_sensor_journal_publish_input(&input) == ESP_ERR_INVALID_ARG);
    journal_read(before);
    assert(journal_batch.count == 2 && journal_records[0].kind == BOARD_SENSOR_BUTTON &&
        (journal_records[0].flags & BOARD_SENSOR_FLAG_OBSERVED_TIME));
    assert(journal_records[1].kind == BOARD_SENSOR_TOUCH && !journal_records[1].validity &&
        (journal_records[1].flags & BOARD_SENSOR_FLAG_REDACTED));
    for (unsigned i = 0; i < 8; ++i) assert(!journal_records[1].payload[i]);
    s_sensor_journal = (board_sensor_journal_t){.records = s_sensor_records, .capacity = 256,
        .latest_sequence = UINT64_MAX - 1, .state = BOARD_SENSOR_JOURNAL_LIVE};
    hw.time_us += 10000; assert(imu_update() == ESP_OK);
    assert(s_sensor_journal.state == BOARD_SENSOR_JOURNAL_EXHAUSTED);
    const uint64_t samples = s_imu_status.sample_count;
    hw.time_us += 10000; assert(imu_update() == ESP_OK && s_imu_status.sample_count == samples + 1 && s_caps.imu);
    assert(!s_imu_status.fault_latched && s_sensor_journal.latest_sequence == UINT64_MAX);
    journal_read(UINT64_MAX - 1);
    assert(journal_batch.count == 1 && journal_records[0].sequence == UINT64_MAX &&
        journal_batch.journal_state == BOARD_SENSOR_JOURNAL_EXHAUSTED);
}

static void test_raw_journal_startup_failure_clock_and_inflight_cancellation(void)
{
    reset(); hw.imu_init_error = ESP_FAIL;
    assert(board_init(legacy_hold) == ESP_OK); journal_read(0);
    assert(journal_records[0].kind == BOARD_SENSOR_IMU_REJECTED && journal_records[0].error == ESP_FAIL &&
        ((journal_records[0].payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_STARTUP &&
        (journal_records[0].payload[6] >> 24) == BOARD_SENSOR_STAGE_INIT);
    assert(!(journal_records[0].flags & BOARD_SENSOR_FLAG_SAMPLE_TIME) && !journal_records[0].sample_us &&
        !(journal_records[0].validity & (BOARD_SENSOR_VALID_READ_START | BOARD_SENSOR_VALID_READ_END | BOARD_SENSOR_VALID_DURATION)));
    reset(); hw.gyro_ready_after_us = 80000;
    assert(board_init(legacy_hold) == ESP_OK);
    uint64_t after = 0; unsigned accepted = 0, not_ready = 0;
    do {
        journal_read(after);
        for (uint32_t i = 0; i < journal_batch.count; ++i) {
            if (journal_records[i].kind == BOARD_SENSOR_IMU_SAMPLE) ++accepted;
            if (journal_records[i].kind == BOARD_SENSOR_IMU_REJECTED &&
                ((journal_records[i].payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_NOT_READY) ++not_ready;
        }
        after = journal_batch.next_after;
    } while (journal_batch.count);
    assert(accepted == 1 && not_ready > 1 && s_imu_status.sample_count == 1);
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK); after = journal_batch.latest_sequence;
    hw.time_us += 10000; hw.during_gyro_read = stale_consumer_during_bus_read;
    assert(imu_update() == ESP_ERR_TIMEOUT); journal_read(after);
    assert(journal_batch.count == 1 && journal_records[0].kind == BOARD_SENSOR_IMU_REJECTED &&
        journal_records[0].source_epoch == 0 && s_imu_generation == 1);
    assert((int32_t)journal_records[0].payload[7] == ESP_ERR_TIMEOUT);
    reset(); assert(board_init(legacy_hold) == ESP_OK);
    assert(board_sensor_journal_status(&journal_batch) == ESP_OK); after = journal_batch.latest_sequence;
    hw.time_us += 40000; hw.during_gyro_read = rollback_during_gyro_read;
    assert(imu_update() == ESP_ERR_TIMEOUT); journal_read(after);
    assert(journal_batch.count == 1 && (journal_records[0].payload[6] >> 24) == BOARD_SENSOR_STAGE_CLOCK &&
        ((journal_records[0].payload[6] >> 16) & 0xffU) == BOARD_SENSOR_ATTEMPT_FATAL &&
        !(journal_records[0].validity & BOARD_SENSOR_VALID_DURATION) &&
        (journal_records[0].discontinuities & BOARD_IMU_DISCONTINUITY_CLOCK));
    assert(journal_word64(journal_records[0].payload + 2) < journal_word64(journal_records[0].payload));
}

int main(void)
{
    test_raw_journal_retains_samples_around_actual_read_rejections();
    test_raw_journal_source_epoch_recovery_and_no_observer_mutation();
    test_power_observer_is_pure_and_power_journal_has_provenance_fields();
    test_raw_journal_input_whitelist_redaction_and_exhaustion_isolation();
    test_raw_journal_startup_failure_clock_and_inflight_cancellation();
    test_ordered_stream_preserves_cache_units_and_identity();
    test_ordered_stream_inflight_fence_is_delivery_only();
    test_ordered_stream_loss_and_hardware_health_are_distinct();
    test_ordered_stream_stale_and_clock_observation_cannot_change_source();
    test_ordered_stream_failed_source_and_delivery_exhaustion();
    test_ordered_stream_observer_during_acquisition_preserves_cause();
    test_confirmed_upright_pose_uses_display_top_axis();
    test_board_to_model_flat_inclined_and_upright_axes();
    test_display_observer_installs_before_adapter();
    test_haptic_diagnostic_actual_receipts();
    test_display_spi_diagnostic_is_observational();
    test_raw_button_authority_and_registration();
    test_imu_real_units_and_nonblocking_cache();
    test_imu_display_frame_is_one_proper_rotation();
    test_imu_start_waits_for_gyro_with_a_finite_deadline();
    test_optional_sensor_failures();
    test_imu_health_is_nonmutating_and_retains_first_fault();
    test_runtime_faults_and_inflight_cancellation();
    test_late_batch_discard_preserves_cache_and_recovers();
    test_late_batch_exact_coherence_and_freshness_boundaries();
    test_late_discard_never_hides_other_faults_or_overflows();
    test_repeated_late_batches_expire_on_normal_worker_schedule();
    test_sensor_worker_cleans_up_after_fault();
    test_worker_recovers_without_reinitialization_or_old_output();
    test_persistent_transport_failure_has_capped_fair_backoff();
    test_fatal_outcomes_win_over_transient_generation_and_early_exit();
    test_inflight_coherent_frame_cannot_cross_loss_generation();
    test_single_owner_schedules_long_battery_without_sensor_starvation();
    test_long_stream_crosses_32_bit_milliseconds();
    test_checked_battery_readings_and_truth_states();
    test_battery_failures_staleness_and_recovery();
    test_battery_packet_contract_and_truthful_failure_stages();
    test_brightness_queue_off_restore_and_failures();
    test_worker_rate_limits_battery_during_brightness_flood();
    puts("Board adapter: physical edges, IMU stream, checked battery states, brightness transitions, faults and bounded worker schedules passed");
    return 0;
}
