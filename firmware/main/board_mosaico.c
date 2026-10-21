// SPDX-License-Identifier: Apache-2.0
#include "board_port.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "haptics_dispatch.h"
#include "soc/soc_caps.h"

#if !CONFIG_BSP_DISPLAY_ENABLE || !CONFIG_BSP_DISPLAY_LVGL_ENABLE
#error "The Mosaico board adapter requires the BSP display and LVGL backend"
#endif
#if CONFIG_BSP_MOTOR_ENABLE_PWM && !SOC_LEDC_SUPPORT_FADE_STOP
#error "The synchronized Mosaico motor path requires the non-waiting S31 LEDC driver"
#endif

enum {
    HAPTICS_POLL_MS = 10,
    HAPTICS_MAX_AGE_MS = 100,
    HAPTICS_STACK_BYTES = 3072,
    LOCK_HOLD_MS = 900,
    IMU_POLL_MS = 10,
    IMU_MAX_AGE_US = 50000,
    IMU_MAX_BATCH_US = 20000,
    IMU_START_TIMEOUT_US = 50000,
    IMU_STACK_BYTES = 3072,
    POWER_POLL_MS = 1000,
    POWER_MAX_AGE_US = 5000000,
    POWER_MAX_BATCH_US = 20000,
    POWER_I2C_TIMEOUT_MS = 3,
    POWER_STACK_BYTES = 3072,
    BRIGHTNESS_LOCK_MS = 100,
    BRIGHTNESS_MIN_INTERVAL_US = 20000,
};

/* BQ27220 standard command map and flag bits from the pinned official driver
 * (bq27220_reg.h/bq27220.h). Checked I2C reads retain transport errors, unlike
 * that driver's public scalar getters. No profile/control commands are sent. */
enum {
    GAUGE_TEMPERATURE = 0x06,
    GAUGE_VOLTAGE = 0x08,
    GAUGE_BATTERY_STATUS = 0x0a,
    GAUGE_CURRENT = 0x0c,
    GAUGE_PERCENT = 0x2c,
    GAUGE_OPERATION_STATUS = 0x3a,
    GAUGE_BATTERY_PRESENT = 1U << 3,
    GAUGE_FULL = 1U << 9,
    GAUGE_INIT_COMPLETE = 1U << 5,
    GAUGE_CONFIG_UPDATE = 1U << 10,
};

static const char *TAG = "board";
static board_capabilities_t s_caps = {.name = "Mosaico (uninitialized)"};
static bool s_init_attempted;
static lv_display_t *s_display;
static button_handle_t s_buttons[BSP_BUTTON_NUM];
static void (*s_lock_callback)(void);
static board_button_event_callback_t s_button_event_callback;
static TaskHandle_t s_haptic_task;
static haptics_t s_haptics;
static TaskHandle_t s_imu_task;
static board_imu_sample_t s_imu_sample;
static bool s_imu_sample_valid;
static esp_err_t s_imu_error = ESP_ERR_INVALID_STATE;
static TaskHandle_t s_power_task;
static i2c_master_dev_handle_t s_gauge;
static board_power_status_t s_power = {.error = ESP_ERR_INVALID_STATE};
static board_brightness_status_t s_brightness = {.error = ESP_ERR_INVALID_STATE};
static uint64_t s_brightness_generation;
static bool s_brightness_attempted;
static uint64_t s_brightness_attempt_us;
static esp_err_t s_power_logged_error = ESP_OK;

/* This lock serializes control mutation with the complete motor dispatch.
 * The pinned S31 BSP/driver fast path is bounded after motor initialization:
 * no fade is registered on the exclusively owned motor channel, and S31 has
 * no duty-update wait. Never install a fade or reconfigure that channel at
 * runtime. Initialization, policy execution and adapter logging stay outside. */
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static haptics_dispatch_t s_dispatch;
static uint64_t s_dispatch_generation; /* Worker-owned token for all pulse phases. */
static bool s_motor_active;
static bool s_pending;
static haptics_event_t s_pending_event;
static uint64_t s_pending_ms;
static bool s_motor_write_failed;
static esp_err_t s_motor_error;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void motor_dispatch_enter(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_status_mux);
}

static void motor_dispatch_leave(void *context)
{
    (void)context;
    portEXIT_CRITICAL(&s_status_mux);
}

/* Called with the dispatch lock held, only after BSP motor initialization.
 * Control cannot invalidate the token between its check and this write. */
static haptics_write_result_t motor_driver_write(void *context, uint8_t strength)
{
    (void)context;
    if (strength) {
        s_motor_active = true;
    }
    const esp_err_t result = strength ? bsp_motor_set_strength(strength) : bsp_motor_set(false);
    if (!strength) {
        s_motor_active = result != ESP_OK;
    }
    s_motor_error = result;
    return result == ESP_OK ? HAPTICS_WRITE_OK : HAPTICS_WRITE_FAILED;
}

static void report_motor_result(esp_err_t result)
{
    if (result != ESP_OK && !s_motor_write_failed) {
        ESP_LOGE(TAG, "Motor write failed: %s", esp_err_to_name(result));
    }
    /* Off retries are bounded by policy; log only the failure edge so a
     * persistent hardware fault does not flood serial output during uptime. */
    s_motor_write_failed = result != ESP_OK;
}

static haptics_write_result_t motor_write(void *context, uint8_t strength)
{
    const uint64_t generation = *(const uint64_t *)context;
    const haptics_write_result_t written = haptics_dispatch_write(&s_dispatch, generation, strength);
    if (written != HAPTICS_WRITE_CANCELLED) {
        report_motor_result(s_motor_error);
    }
    return written;
}

/* Initialization failure may require BSP reinitialization during off. Keep
 * that potentially allocating path outside the runtime dispatch lock. */
static void motor_initial_safety_stop(void)
{
    const esp_err_t result = bsp_motor_set(false);
    portENTER_CRITICAL(&s_status_mux);
    s_motor_active = result != ESP_OK;
    portEXIT_CRITICAL(&s_status_mux);
    report_motor_result(result);
}

static void publish_haptics(void)
{
    portENTER_CRITICAL(&s_status_mux);
    s_caps.haptics = haptics_available(&s_haptics) && !s_dispatch.exhausted;
    if (!s_caps.haptics) {
        (void)haptics_dispatch_enable_locked(&s_dispatch, false);
        s_pending = false;
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void haptics_worker(void *argument)
{
    (void)argument;
    uint64_t applied_generation = 0;
    TickType_t poll_ticks = pdMS_TO_TICKS(HAPTICS_POLL_MS);
    if (!poll_ticks) {
        poll_ticks = 1;
    }
    for (;;) {
        bool enabled;
        bool pending;
        haptics_event_t event;
        uint64_t generation;
        uint64_t pending_ms;
        portENTER_CRITICAL(&s_status_mux);
        enabled = s_dispatch.enabled;
        generation = s_dispatch.generation;
        pending = s_pending;
        event = s_pending_event;
        pending_ms = s_pending_ms;
        s_pending = false;
        portEXIT_CRITICAL(&s_status_mux);

        const uint64_t time_ms = now_ms();
        s_dispatch_generation = generation;
        if (generation != applied_generation || enabled != haptics_enabled(&s_haptics)) {
            /* Even a disable/enable pair between worker iterations cancels the
             * old pattern. The mailbox was cleared by each control change. */
            (void)haptics_set_enabled(&s_haptics, false, time_ms);
            (void)haptics_set_enabled(&s_haptics, enabled, time_ms);
            applied_generation = generation;
        }
        if (pending && enabled) {
            /* Every nonzero phase is checked atomically with its driver write,
             * including a second error pulse generated by haptics_poll(). */
            if (time_ms >= pending_ms && time_ms - pending_ms <= HAPTICS_MAX_AGE_MS) {
                (void)haptics_request(&s_haptics, event, time_ms);
            }
        }
        (void)haptics_poll(&s_haptics, time_ms);
        publish_haptics();
        /* A control notification wakes immediately; the 10 ms deadline also
         * advances pulse phases without occupying the HID/coordinator task. */
        (void)ulTaskNotifyTake(pdTRUE, poll_ticks);
    }
}

static void lock_button_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    if (s_lock_callback) {
        s_lock_callback();
    }
}

static void button_down_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    s_button_event_callback(BOARD_BUTTON_DOWN, (uint64_t)esp_timer_get_time());
}

static void button_up_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    s_button_event_callback(BOARD_BUTTON_UP, (uint64_t)esp_timer_get_time());
}

static void delete_buttons(void)
{
    for (size_t i = 0; i < BSP_BUTTON_NUM; ++i) {
        if (s_buttons[i]) {
            const esp_err_t result = iot_button_delete(s_buttons[i]);
            if (result == ESP_OK) {
                s_buttons[i] = NULL;
            } else {
                ESP_LOGE(TAG, "Button cleanup failed: %s", esp_err_to_name(result));
            }
        }
    }
}

static void init_optional_haptics(void)
{
    esp_err_t result = bsp_motor_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Motor unavailable: %s", esp_err_to_name(result));
        motor_initial_safety_stop();
        return;
    }
#if CONFIG_BSP_MOTOR_ENABLE_PWM
    const bool strength_control = true;
#else
    const bool strength_control = false;
#endif
    /* The GPIO-only BSP fallback turns every nonzero strength into full power;
     * it cannot provide this adapter's mild feedback contract. */
    if (!strength_control) {
        motor_initial_safety_stop();
        ESP_LOGW(TAG, "Haptics require CONFIG_BSP_MOTOR_ENABLE_PWM");
        return;
    }
    if (!haptics_dispatch_init(&s_dispatch, motor_dispatch_enter, motor_dispatch_leave,
                               NULL, motor_driver_write, NULL)) {
        motor_initial_safety_stop();
        return;
    }
    if (haptics_init(&s_haptics, true, motor_write, &s_dispatch_generation, now_ms()) != HAPTICS_OK) {
        ESP_LOGW(TAG, "Motor-off verification failed; haptics disabled");
        /* Keep the worker even after an off failure so bounded retries run. */
    }
    portENTER_CRITICAL(&s_status_mux);
    s_caps.haptics = haptics_available(&s_haptics);
    (void)haptics_dispatch_enable_locked(&s_dispatch, haptics_enabled(&s_haptics));
    portEXIT_CRITICAL(&s_status_mux);
    if (xTaskCreate(haptics_worker, "haptics", HAPTICS_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 1, &s_haptic_task) != pdPASS) {
        (void)haptics_set_enabled(&s_haptics, false, now_ms());
        portENTER_CRITICAL(&s_status_mux);
        s_caps.haptics = false;
        (void)haptics_dispatch_enable_locked(&s_dispatch, false);
        portEXIT_CRITICAL(&s_status_mux);
        ESP_LOGW(TAG, "Haptic worker unavailable; motor stopped where possible");
    }
}

static void imu_latch_fault(esp_err_t error)
{
    portENTER_CRITICAL(&s_status_mux);
    s_imu_error = error;
    s_imu_sample_valid = false;
    s_caps.imu = false;
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGW(TAG, "IMU stream unavailable: %s", esp_err_to_name(error));
}

static void imu_cleanup(bool initialized)
{
    if (initialized) {
        const esp_err_t stop = bsp_imu_stop();
        if (stop != ESP_OK) {
            ESP_LOGW(TAG, "IMU stop failed: %s", esp_err_to_name(stop));
        }
    }
    const esp_err_t cleanup = bsp_imu_deinit();
    if (cleanup != ESP_OK) {
        ESP_LOGW(TAG, "IMU cleanup failed: %s", esp_err_to_name(cleanup));
    }
}

static esp_err_t imu_read_hardware(board_imu_sample_t *sample)
{
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    struct bmi2_dev *device = bsp_imu_get_handle();
    if (!device) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t status = 0;
    if (bmi2_get_status(&status, device) != BMI2_OK) {
        return ESP_FAIL;
    }
    if ((status & (BMI2_DRDY_GYR | BMI2_DRDY_ACC)) != (BMI2_DRDY_GYR | BMI2_DRDY_ACC)) {
        return ESP_ERR_NOT_FOUND;
    }
    board_imu_sample_t candidate;
    const esp_err_t result = bsp_imu_get_gyro(&candidate.x_dps, &candidate.y_dps, &candidate.z_dps);
    if (result != ESP_OK) {
        return result;
    }
    const esp_err_t accel = bsp_imu_get_accel(&candidate.x_g, &candidate.y_g, &candidate.z_g);
    if (accel != ESP_OK) {
        return accel;
    }
    candidate.timestamp_us = (uint64_t)esp_timer_get_time();
    if (candidate.timestamp_us < started_us || candidate.timestamp_us - started_us > IMU_MAX_BATCH_US) {
        return ESP_ERR_TIMEOUT;
    }
    if (!isfinite(candidate.x_dps) || !isfinite(candidate.y_dps) || !isfinite(candidate.z_dps) ||
            !isfinite(candidate.x_g) || !isfinite(candidate.y_g) || !isfinite(candidate.z_g)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *sample = candidate;
    return ESP_OK;
}

/* One sampler owns all BSP sensor reads. A fault set by a consumer while a bus
 * read is in flight wins over the result, so stale data cannot restore imu. */
static esp_err_t imu_update(void)
{
    portENTER_CRITICAL(&s_status_mux);
    esp_err_t result = s_imu_error;
    portEXIT_CRITICAL(&s_status_mux);
    if (result != ESP_OK) {
        return result;
    }
    board_imu_sample_t candidate;
    result = imu_read_hardware(&candidate);
    portENTER_CRITICAL(&s_status_mux);
    if (s_imu_error != ESP_OK) {
        result = s_imu_error;
    } else if (result == ESP_OK) {
        if (s_imu_sample_valid && candidate.timestamp_us <= s_imu_sample.timestamp_us) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            s_imu_sample = candidate;
            s_imu_sample_valid = true;
        }
    } else if (result == ESP_ERR_NOT_FOUND) {
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        result = s_imu_sample_valid && time_us >= s_imu_sample.timestamp_us &&
            time_us - s_imu_sample.timestamp_us <= IMU_MAX_AGE_US ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    portEXIT_CRITICAL(&s_status_mux);
    if (result != ESP_OK) {
        imu_latch_fault(result);
    }
    return result;
}

static void imu_worker(void *argument)
{
    (void)argument;
    TickType_t poll_ticks = pdMS_TO_TICKS(IMU_POLL_MS);
    if (!poll_ticks) {
        poll_ticks = 1;
    }
    TickType_t deadline = xTaskGetTickCount();
    for (;;) {
        /* Skip missed periods after a slow bus transaction rather than issuing
         * a burst of catch-up reads with misleading sample intervals. */
        const TickType_t current = xTaskGetTickCount();
        if ((TickType_t)(current - deadline) >= poll_ticks) {
            deadline = current;
        }
        (void)xTaskDelayUntil(&deadline, poll_ticks);
        if (imu_update() != ESP_OK) {
            break;
        }
    }
    imu_cleanup(true);
    vTaskDelete(NULL);
}

static void init_optional_imu(void)
{
    esp_err_t result = bsp_imu_init();
    if (result != ESP_OK) {
        imu_latch_fault(result);
        imu_cleanup(false);
        return;
    }
    bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
    config.accel_odr = BMI2_ACC_ODR_100HZ;
    config.gyro_odr = BMI2_GYR_ODR_100HZ;
    result = bsp_imu_start(&config);
    if (result != ESP_OK) {
        imu_latch_fault(result);
        imu_cleanup(true);
        return;
    }
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    board_imu_sample_t first;
    do {
        result = imu_read_hardware(&first);
        if (result != ESP_ERR_NOT_FOUND) {
            break;
        }
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (time_us < started_us || time_us - started_us >= IMU_START_TIMEOUT_US) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t wait = pdMS_TO_TICKS(2);
        vTaskDelay(wait ? wait : 1);
    } while (true);
    if (result != ESP_OK) {
        imu_latch_fault(result);
        imu_cleanup(true);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_imu_sample = first;
    s_imu_sample_valid = true;
    s_imu_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_mux);
    if (xTaskCreate(imu_worker, "imu", IMU_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 2, &s_imu_task) != pdPASS) {
        imu_latch_fault(ESP_ERR_NO_MEM);
        imu_cleanup(true);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_caps.imu = s_imu_error == ESP_OK;
    portEXIT_CRITICAL(&s_status_mux);
}

static board_power_status_t power_unknown(esp_err_t error)
{
    return (board_power_status_t){.error = error};
}

static esp_err_t gauge_read_word(uint8_t command, uint16_t *word)
{
    uint8_t bytes[2];
    const esp_err_t result = i2c_master_transmit_receive(s_gauge, &command, 1,
        bytes, sizeof(bytes), POWER_I2C_TIMEOUT_MS);
    if (result == ESP_OK) {
        *word = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    }
    return result;
}

static board_power_status_t power_read_hardware(void)
{
    if (!s_gauge) {
        return power_unknown(ESP_ERR_INVALID_STATE);
    }
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    const uint8_t commands[] = {GAUGE_OPERATION_STATUS, GAUGE_BATTERY_STATUS,
        GAUGE_VOLTAGE, GAUGE_CURRENT, GAUGE_TEMPERATURE, GAUGE_PERCENT};
    uint16_t words[sizeof(commands) / sizeof(commands[0])];
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        const esp_err_t result = gauge_read_word(commands[i], &words[i]);
        if (result != ESP_OK) {
            return power_unknown(result);
        }
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (time_us < started_us || time_us - started_us > POWER_MAX_BATCH_US) {
            return power_unknown(ESP_ERR_TIMEOUT);
        }
    }
    board_power_status_t status = {
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .gauge_available = true,
    };
    if (!(words[0] & GAUGE_INIT_COMPLETE) || (words[0] & GAUGE_CONFIG_UPDATE)) {
        status.error = ESP_ERR_INVALID_STATE;
        return status;
    }
    status.battery_present = words[1] & GAUGE_BATTERY_PRESENT ? BOARD_TRUTH_YES : BOARD_TRUTH_NO;
    if (status.battery_present == BOARD_TRUTH_NO) {
        return status;
    }
    status.full = words[1] & GAUGE_FULL ? BOARD_TRUTH_YES : BOARD_TRUTH_NO;
    status.voltage_valid = words[2] != UINT16_MAX;
    status.voltage_mv = words[2];
    status.current_valid = true;
    const int32_t signed_current = words[3] <= INT16_MAX ? (int32_t)words[3] : (int32_t)words[3] - 65536;
    status.current_ma = (int16_t)signed_current;
    status.charging = signed_current > 0 ? BOARD_TRUTH_YES :
        signed_current < 0 ? BOARD_TRUTH_NO : BOARD_TRUTH_UNKNOWN;
    status.temperature_valid = words[4] != 0 && words[4] != UINT16_MAX;
    status.temperature_c = words[4] / 10.0f - 273.15f;
    status.percent_valid = words[5] <= 100;
    if (status.percent_valid) {
        status.percent = (uint8_t)words[5];
    }
    /* Current direction and the FC bit describe gauge observations. Neither
     * provides independent evidence of VBUS/charger/external supply presence. */
    return status;
}

static void power_update(void)
{
    const board_power_status_t status = power_read_hardware();
    portENTER_CRITICAL(&s_status_mux);
    s_power = status;
    s_caps.battery = status.error == ESP_OK && status.gauge_available;
    portEXIT_CRITICAL(&s_status_mux);
    if (status.error != ESP_OK && status.error != s_power_logged_error) {
        ESP_LOGW(TAG, "Battery telemetry unavailable: %s", esp_err_to_name(status.error));
    }
    s_power_logged_error = status.error;
}

static void brightness_update(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const bool pending = s_brightness.pending;
    const uint8_t percent = s_brightness.requested_percent;
    const uint64_t generation = s_brightness_generation;
    portEXIT_CRITICAL(&s_status_mux);
    if (!pending) {
        return;
    }
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    if (s_brightness_attempted && started_us >= s_brightness_attempt_us &&
            started_us - s_brightness_attempt_us < BRIGHTNESS_MIN_INTERVAL_US) {
        return;
    }
    s_brightness_attempted = true;
    s_brightness_attempt_us = started_us;
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (board_display_lock(BRIGHTNESS_LOCK_MS)) {
        result = bsp_display_brightness_set(percent);
        board_display_unlock();
    }
    const uint64_t applied_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_status_mux);
    s_brightness.error = result;
    s_brightness.applied_known = result == ESP_OK;
    if (result == ESP_OK) {
        s_brightness.applied_percent = percent;
        s_brightness.applied_at_us = applied_us;
    }
    if (s_brightness_generation == generation) {
        s_brightness.pending = false;
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void power_worker(void *argument)
{
    (void)argument;
    uint64_t last_poll_us = (uint64_t)esp_timer_get_time();
    for (;;) {
        brightness_update();
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (s_gauge && (time_us < last_poll_us || time_us - last_poll_us >= POWER_POLL_MS * 1000U)) {
            power_update();
            last_poll_us = (uint64_t)esp_timer_get_time();
        }
        const uint64_t after_us = (uint64_t)esp_timer_get_time();
        uint32_t wait_ms = POWER_POLL_MS;
        if (s_gauge && after_us >= last_poll_us) {
            const uint64_t elapsed_us = after_us - last_poll_us;
            wait_ms = elapsed_us >= POWER_POLL_MS * 1000U ? 1 :
                (uint32_t)((POWER_POLL_MS * 1000U - elapsed_us + 999) / 1000);
        }
        portENTER_CRITICAL(&s_status_mux);
        const bool brightness_pending = s_brightness.pending;
        portEXIT_CRITICAL(&s_status_mux);
        if (brightness_pending) {
            uint32_t brightness_wait_ms = 1;
            if (s_brightness_attempted && after_us >= s_brightness_attempt_us &&
                    after_us - s_brightness_attempt_us < BRIGHTNESS_MIN_INTERVAL_US) {
                brightness_wait_ms = (uint32_t)((BRIGHTNESS_MIN_INTERVAL_US -
                    (after_us - s_brightness_attempt_us) + 999) / 1000);
            }
            if (brightness_wait_ms < wait_ms) {
                wait_ms = brightness_wait_ms;
            }
        }
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        (void)ulTaskNotifyTake(pdTRUE, ticks ? ticks : 1);
    }
}

static void init_optional_power(void)
{
    esp_err_t result = bsp_i2c_init();
    i2c_master_bus_handle_t bus = result == ESP_OK ? bsp_i2c_get_handle() : NULL;
    if (result == ESP_OK && !bus) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BSP_BATTERY_I2C_ADDR,
            .scl_speed_hz = BSP_BATTERY_I2C_SPEED_HZ,
        };
        result = i2c_master_bus_add_device(bus, &config, &s_gauge);
    }
    if (result == ESP_OK) {
        power_update();
    } else {
        portENTER_CRITICAL(&s_status_mux);
        s_power = power_unknown(result);
        portEXIT_CRITICAL(&s_status_mux);
        ESP_LOGW(TAG, "Battery bus unavailable: %s", esp_err_to_name(result));
    }
    if (xTaskCreate(power_worker, "board_power", POWER_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 1, &s_power_task) != pdPASS) {
        portENTER_CRITICAL(&s_status_mux);
        s_power = power_unknown(ESP_ERR_NO_MEM);
        s_caps.battery = false;
        s_brightness.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_status_mux);
        if (s_gauge) {
            const esp_err_t cleanup = i2c_master_bus_rm_device(s_gauge);
            if (cleanup == ESP_OK) {
                s_gauge = NULL;
            } else {
                ESP_LOGW(TAG, "Battery bus cleanup failed: %s", esp_err_to_name(cleanup));
            }
        }
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_caps.brightness = true;
    portEXIT_CRITICAL(&s_status_mux);
}

esp_err_t board_button_events_register(board_button_event_callback_t callback)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_init_attempted || s_button_event_callback) {
        return ESP_ERR_INVALID_STATE;
    }
    s_button_event_callback = callback;
    return ESP_OK;
}

esp_err_t board_init(void (*lock_callback)(void))
{
    if (!lock_callback && !s_button_event_callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_init_attempted) {
        return ESP_ERR_INVALID_STATE;
    }
    s_init_attempted = true;
    bsp_board_variant_t variant;
    esp_err_t result = bsp_board_variant_get(&variant);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unsupported board eFuse revision: %s", esp_err_to_name(result));
        return result;
    }
    if (variant != BSP_BOARD_VARIANT_V1_0 && variant != BSP_BOARD_VARIANT_V1_2) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_caps.name = variant == BSP_BOARD_VARIANT_V1_0 ? "Mosaico v1.0" : "Mosaico v1.2";
    result = bsp_power_init();
    if (result != ESP_OK) {
        return result;
    }
    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = true;
    s_display = bsp_display_start_with_config(&display_config);
    s_caps.display = s_display != NULL;
    s_caps.touch = bsp_display_get_input_dev() != NULL;
    if (!s_caps.display || !s_caps.touch) {
        ESP_LOGE(TAG, "Required display/touch unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    /* The pinned BSP CO5300 configuration uses queue depth 10. Query the
     * driver's actual transfer limit rather than assuming its DMA rounding. */
    size_t spi_max_bytes = 0;
    const esp_err_t spi_limit_result = spi_bus_get_max_transaction_len(BSP_LCD_SPI_HOST, &spi_max_bytes);
    if (spi_limit_result == ESP_OK) {
        ESP_LOGI(TAG, "Display SPI max_transaction_bytes=%zu pinned_queue_depth=10", spi_max_bytes);
    } else {
        ESP_LOGW(TAG, "Display SPI transfer limit unavailable: %s (pinned_queue_depth=10)",
                 esp_err_to_name(spi_limit_result));
    }
    if (!board_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    board_display_unlock();

    int button_count = 0;
    result = bsp_iot_button_create(s_buttons, &button_count, BSP_BUTTON_NUM);
    if (result == ESP_OK && (button_count <= BSP_BUTTON_AI || !s_buttons[BSP_BUTTON_AI])) {
        result = ESP_ERR_NOT_FOUND;
    }
    s_lock_callback = s_button_event_callback ? NULL : lock_callback;
    if (result == ESP_OK && s_button_event_callback) {
        /* Establish release handling before admitting any down event. Only
         * these raw events are bound: the coordinator owns gesture timing. */
        result = iot_button_register_cb(s_buttons[BSP_BUTTON_AI], BUTTON_PRESS_UP,
                                        NULL, button_up_callback, NULL);
        if (result == ESP_OK) {
            result = iot_button_register_cb(s_buttons[BSP_BUTTON_AI], BUTTON_PRESS_DOWN,
                                            NULL, button_down_callback, NULL);
        }
    } else if (result == ESP_OK) {
        button_event_args_t hold = {.long_press = {.press_time = LOCK_HOLD_MS}};
        result = iot_button_register_cb(s_buttons[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START,
                                        &hold, lock_button_callback, NULL);
    }
    if (result != ESP_OK) {
        s_lock_callback = NULL;
        delete_buttons();
        ESP_LOGE(TAG, "Required physical lock unavailable: %s", esp_err_to_name(result));
        return result;
    }
    s_caps.lock_button = true;

    init_optional_imu();
    init_optional_haptics();
    init_optional_power();
    return ESP_OK;
}

board_capabilities_t board_capabilities(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const board_capabilities_t capabilities = s_caps;
    portEXIT_CRITICAL(&s_status_mux);
    return capabilities;
}

esp_err_t board_imu_read(board_imu_sample_t *sample)
{
    if (!sample) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_status_mux);
    esp_err_t result = s_imu_error;
    if (result == ESP_OK && (!s_caps.imu || !s_imu_sample_valid)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (time_us < s_imu_sample.timestamp_us || time_us - s_imu_sample.timestamp_us > IMU_MAX_AGE_US) {
            result = ESP_ERR_TIMEOUT;
            s_imu_error = result;
            s_imu_sample_valid = false;
            s_caps.imu = false;
        } else {
            *sample = s_imu_sample;
        }
    }
    portEXIT_CRITICAL(&s_status_mux);
    return result;
}

esp_err_t board_power_read(board_power_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_status_mux);
    const uint64_t time_us = (uint64_t)esp_timer_get_time();
    if (s_power.gauge_available && (time_us < s_power.timestamp_us ||
            time_us - s_power.timestamp_us > POWER_MAX_AGE_US)) {
        const uint64_t previous_us = s_power.timestamp_us;
        s_power = power_unknown(ESP_ERR_TIMEOUT);
        s_power.timestamp_us = previous_us;
        s_caps.battery = false;
    }
    *status = s_power;
    portEXIT_CRITICAL(&s_status_mux);
    return status->error;
}

esp_err_t board_brightness_set(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_status_mux);
    esp_err_t result = ESP_OK;
    bool changed = false;
    if (!s_caps.brightness || !s_power_task) {
        result = ESP_ERR_INVALID_STATE;
    } else if (s_brightness.requested && s_brightness.requested_percent == percent &&
            (s_brightness.pending || (s_brightness.applied_known && s_brightness.error == ESP_OK &&
             s_brightness.applied_percent == percent))) {
        /* Coalesce repeated UI values and keep per-frame writes off the bus. */
    } else if (s_brightness_generation == UINT64_MAX) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        ++s_brightness_generation;
        s_brightness.requested = true;
        s_brightness.requested_percent = (uint8_t)percent;
        s_brightness.pending = true;
        changed = true;
    }
    portEXIT_CRITICAL(&s_status_mux);
    if (changed) {
        xTaskNotifyGive(s_power_task);
    }
    return result;
}

board_brightness_status_t board_brightness_status(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const board_brightness_status_t status = s_brightness;
    portEXIT_CRITICAL(&s_status_mux);
    return status;
}

lv_display_t *board_display_get(void)
{
    return s_display;
}

bool board_display_lock(uint32_t timeout_ms)
{
    if (!s_display) {
        return false;
    }
    if (timeout_ms != UINT32_MAX && timeout_ms > INT32_MAX) {
        return false;
    }
    return bsp_display_lock(timeout_ms == UINT32_MAX ? -1 : (int32_t)timeout_ms);
}

void board_display_unlock(void)
{
    if (s_display) {
        bsp_display_unlock();
    }
}

bool board_haptics_enabled(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const bool enabled = s_caps.haptics && s_dispatch.enabled;
    portEXIT_CRITICAL(&s_status_mux);
    return enabled;
}

void board_haptics_enable(bool enabled)
{
    bool changed = false;
    portENTER_CRITICAL(&s_status_mux);
    const bool requested = enabled && s_caps.haptics;
    if (haptics_dispatch_enable_locked(&s_dispatch, requested)) {
        s_pending = false;
        changed = true;
    }
    portEXIT_CRITICAL(&s_status_mux);
    if (changed && s_haptic_task) {
        xTaskNotifyGive(s_haptic_task);
    }
}

void board_haptic_feedback(board_feedback_t event)
{
    haptics_event_t haptic;
    switch (event) {
    case BOARD_FEEDBACK_CONFIRM: haptic = HAPTICS_CONFIRM; break;
    case BOARD_FEEDBACK_MODE: haptic = HAPTICS_MODE; break;
    case BOARD_FEEDBACK_LOCK: haptic = HAPTICS_LOCK; break;
    case BOARD_FEEDBACK_ERROR: haptic = HAPTICS_ERROR; break;
    default: return;
    }
    bool accepted = false;
    const uint64_t time_ms = now_ms();
    portENTER_CRITICAL(&s_status_mux);
    if (s_caps.haptics && s_dispatch.enabled && !s_pending) {
        s_pending_event = haptic;
        s_pending_ms = time_ms;
        s_pending = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_status_mux);
    if (accepted && s_haptic_task) {
        xTaskNotifyGive(s_haptic_task);
    }
}

bool board_haptics_active(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const bool active = s_motor_active || s_pending;
    portEXIT_CRITICAL(&s_status_mux);
    return active;
}
