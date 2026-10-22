// SPDX-License-Identifier: Apache-2.0
#include "board_port.h"
#include "board_display_transfer.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
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
    /* BMI270 DS000-08, Table 3: gyro startup is 45 ms at maximum ODR.
     * Allow a 100 Hz sample period, the 20 ms batch and polling margin at
     * startup; the running stream retains its separate 50 ms freshness gate. */
    IMU_START_TIMEOUT_US = 100000,
    SENSOR_STACK_BYTES = 4096,
    SENSOR_PRIORITY = 6, /* Matches configured LVGL priority, below radio/system priorities. */
    IMU_MAX_RETRY_MS = 250,
    POWER_POLL_MS = 1000,
    POWER_MAX_AGE_US = 5000000,
    POWER_MAX_BATCH_US = 20000,
    /* TI SLUSCB7A 7.3.1.3/4: >=66 us between gauge packets and up to
     * 4 ms clock stretch. Keep software/physical waits above that valid
     * response while retaining the separate 20 ms complete-batch gate. */
    POWER_I2C_PACKET_GAP_US = 100,
    POWER_I2C_SCL_WAIT_US = 5000,
    POWER_I2C_TIMEOUT_MS = 6,
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
static board_haptic_status_t s_haptic_status = {.error = ESP_ERR_INVALID_STATE};
static bool s_haptic_diagnostic_pending, s_haptic_cancel_requested;
static TaskHandle_t s_imu_task;
static bool s_imu_hardware_live;
static uint64_t s_imu_generation;
static bsp_imu_config_t s_imu_config;
static float s_imu_accel_scale, s_imu_gyro_scale;
static board_imu_sample_t s_imu_sample;
static bool s_imu_sample_valid;
static esp_err_t s_imu_error = ESP_ERR_INVALID_STATE;
static board_imu_status_t s_imu_status = {
    .error = ESP_ERR_INVALID_STATE,
    .first_fault_stage = "none",
    .last_hardware_stage = "none",
    .last_hardware_error = ESP_ERR_INVALID_STATE,
    .last_read_error = ESP_ERR_INVALID_STATE,
};
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

static haptics_event_t haptic_event(board_feedback_t pattern)
{
    switch (pattern) {
    case BOARD_FEEDBACK_MODE: return HAPTICS_MODE;
    case BOARD_FEEDBACK_LOCK: return HAPTICS_LOCK;
    case BOARD_FEEDBACK_ERROR: return HAPTICS_ERROR;
    default: return HAPTICS_CONFIRM;
    }
}

static void haptics_service(uint64_t *applied_generation)
{
    portENTER_CRITICAL(&s_status_mux);
    bool enabled = s_dispatch.enabled;
    uint64_t generation = s_dispatch.generation;
    bool pending = s_pending;
    haptics_event_t event = s_pending_event;
    uint64_t pending_ms = s_pending_ms;
    bool diagnostic = s_haptic_diagnostic_pending;
    bool cancelled = s_haptic_cancel_requested;
    board_haptic_status_t request = s_haptic_status;
    s_pending = false;
    s_haptic_diagnostic_pending = false;
    s_haptic_cancel_requested = false;
    portEXIT_CRITICAL(&s_status_mux);
    uint64_t time_ms = now_ms();
    s_dispatch_generation = generation;
    if (generation != *applied_generation || enabled != haptics_enabled(&s_haptics)) {
        (void)haptics_set_enabled(&s_haptics, false, time_ms);
        (void)haptics_set_enabled(&s_haptics, enabled, time_ms);
        *applied_generation = generation;
    }
    if (diagnostic) {
        haptics_result_t result;
        if (cancelled) result = HAPTICS_CANCELLED;
        else if (request.operation == BOARD_HAPTIC_RECOVER) result = haptics_recover(&s_haptics, time_ms);
        else if (!enabled || generation != request.generation || time_ms < request.requested_ms ||
            time_ms - request.requested_ms > HAPTICS_MAX_AGE_MS) result = HAPTICS_SUPPRESSED;
        else result = haptics_request(&s_haptics, haptic_event(request.pattern), time_ms);
        portENTER_CRITICAL(&s_status_mux);
        if (s_haptic_status.request_id == request.request_id) {
            cancelled = cancelled || s_haptic_cancel_requested;
            s_haptic_status.error = result == HAPTICS_OK || result == HAPTICS_SUPPRESSED ||
                result == HAPTICS_DISABLED || result == HAPTICS_CANCELLED ? ESP_OK : ESP_FAIL;
            s_haptic_status.off_confirmed = !s_motor_active && s_motor_error == ESP_OK;
            if (result == HAPTICS_OK && request.operation == BOARD_HAPTIC_RECOVER) {
                s_haptic_status.state = s_haptic_status.off_confirmed ? BOARD_HAPTIC_COMPLETED : BOARD_HAPTIC_UNCERTAIN;
                s_haptic_status.finished_ms = time_ms;
            } else if (result == HAPTICS_OK) {
                s_haptic_status.state = BOARD_HAPTIC_ACTIVE;
                s_haptic_status.on_confirmed = true;
                s_haptic_status.started_ms = time_ms;
            } else {
                s_haptic_status.state = cancelled && s_haptic_status.off_confirmed ? BOARD_HAPTIC_CANCELLED :
                    result == HAPTICS_SUPPRESSED || result == HAPTICS_DISABLED || result == HAPTICS_CANCELLED ?
                    BOARD_HAPTIC_NO_EFFECT : s_motor_active ? BOARD_HAPTIC_UNCERTAIN : BOARD_HAPTIC_FAILED;
                s_haptic_status.finished_ms = time_ms;
            }
        }
        portEXIT_CRITICAL(&s_status_mux);
    } else if (pending && enabled && time_ms >= pending_ms && time_ms - pending_ms <= HAPTICS_MAX_AGE_MS) {
        (void)haptics_request(&s_haptics, event, time_ms);
    }
    (void)haptics_poll(&s_haptics, time_ms);
    portENTER_CRITICAL(&s_status_mux);
    if (s_haptic_status.state == BOARD_HAPTIC_ACTIVE) {
        s_haptic_status.off_confirmed = !s_motor_active && s_motor_error == ESP_OK;
        if (!haptics_available(&s_haptics)) {
            s_haptic_status.state = s_motor_active ? BOARD_HAPTIC_UNCERTAIN : BOARD_HAPTIC_FAILED;
            s_haptic_status.error = s_motor_error == ESP_OK ? ESP_FAIL : s_motor_error;
            s_haptic_status.finished_ms = time_ms;
        } else if (s_haptic_status.generation != s_dispatch.generation || !haptics_enabled(&s_haptics)) {
            s_haptic_status.state = s_haptic_status.off_confirmed ? BOARD_HAPTIC_CANCELLED : BOARD_HAPTIC_UNCERTAIN;
            s_haptic_status.finished_ms = time_ms;
        } else if (!s_haptics.playing && s_haptic_status.off_confirmed) {
            s_haptic_status.state = BOARD_HAPTIC_COMPLETED;
            s_haptic_status.finished_ms = time_ms;
        }
    }
    portEXIT_CRITICAL(&s_status_mux);
    publish_haptics();
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
        haptics_service(&applied_generation);
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

static void imu_record_fault_locked(esp_err_t error, const char *stage)
{
    if (!s_imu_status.fault_latched) {
        s_imu_status.fault_latched = true;
        s_imu_status.first_fault_error = error;
        s_imu_status.first_fault_stage = stage;
        s_imu_status.first_fault_us = (uint64_t)esp_timer_get_time();
    }
}

static void imu_count_sample_locked(void)
{
    if (s_imu_status.sample_count != UINT64_MAX) {
        ++s_imu_status.sample_count;
    } else {
        s_imu_status.sample_count_saturated = true;
    }
}

static esp_err_t imu_failure_locked(esp_err_t error, const char *stage, bool fatal, bool attempted)
{
    if (s_imu_status.phase == BOARD_IMU_FATAL) return s_imu_error;
    if (!fatal && s_imu_status.phase != BOARD_IMU_RECOVERING) {
        if (s_imu_generation == UINT64_MAX) {
            fatal = true;
            error = ESP_ERR_INVALID_STATE;
            stage = "generation";
        } else ++s_imu_generation;
    }
    imu_record_fault_locked(error, stage);
    s_imu_status.phase = fatal ? BOARD_IMU_FATAL : BOARD_IMU_RECOVERING;
    s_imu_error = error;
    s_imu_sample_valid = false;
    s_caps.imu = false;
    if (attempted && s_imu_status.failure_streak != UINT32_MAX) ++s_imu_status.failure_streak;
    const uint32_t delays[] = {10, 20, 40, 80, 160, IMU_MAX_RETRY_MS};
    uint32_t index = s_imu_status.failure_streak ? s_imu_status.failure_streak - 1U : 0U;
    if (index >= sizeof(delays) / sizeof(delays[0])) index = sizeof(delays) / sizeof(delays[0]) - 1U;
    s_imu_status.retry_delay_ms = fatal ? 0 : delays[index];
    return s_imu_error;
}

static void imu_latch_fault(esp_err_t error, const char *stage)
{
    portENTER_CRITICAL(&s_status_mux);
    imu_failure_locked(error, stage, true, false);
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGW(TAG, "IMU stream unavailable: %s", esp_err_to_name(error));
}

static void imu_cleanup(bool initialized)
{
    s_imu_hardware_live = false;
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

typedef enum { IMU_READ_FATAL, IMU_READ_COMMUNICATION, IMU_READ_NOT_READY, IMU_READ_LATE, IMU_READ_SAMPLE } imu_read_kind_t;

static esp_err_t imu_read_hardware(board_imu_sample_t *sample, const char **stage, imu_read_kind_t *kind)
{
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    *kind = IMU_READ_FATAL;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool status_known = false;
    uint8_t status = 0;
    *stage = "get_handle";
    struct bmi2_dev *device = bsp_imu_get_handle();
    if (!device) {
        goto done;
    }
    *stage = "config";
    if (s_imu_config.accel_range != BMI2_ACC_RANGE_4G || s_imu_config.gyro_range != BMI2_GYR_RANGE_2000) goto done;
    /* Both BSP scalar getters independently fetch this complete corrected
     * Bosch frame. Fetch it once with the same driver and configured ranges. */
    struct bmi2_sens_data data = {0};
    *stage = "frame";
    const int8_t frame_result = bmi2_get_sensor_data(&data, device);
    if (frame_result != BMI2_OK) {
        if (frame_result == BMI2_E_COM_FAIL) *kind = IMU_READ_COMMUNICATION;
        result = ESP_FAIL;
        goto done;
    }
    status = data.status;
    status_known = true;
    *stage = "data_ready";
    if ((status & (BMI2_DRDY_GYR | BMI2_DRDY_ACC)) != (BMI2_DRDY_GYR | BMI2_DRDY_ACC)) {
        *kind = IMU_READ_NOT_READY;
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }
    board_imu_sample_t candidate = {
        .x_dps = data.gyr.x * s_imu_gyro_scale, .y_dps = data.gyr.y * s_imu_gyro_scale,
        .z_dps = data.gyr.z * s_imu_gyro_scale,
        .x_g = data.acc.x * s_imu_accel_scale, .y_g = data.acc.y * s_imu_accel_scale,
        .z_g = data.acc.z * s_imu_accel_scale,
    };
    candidate.timestamp_us = (uint64_t)esp_timer_get_time();
    const bool finite = isfinite(candidate.x_dps) && isfinite(candidate.y_dps) && isfinite(candidate.z_dps) &&
        isfinite(candidate.x_g) && isfinite(candidate.y_g) && isfinite(candidate.z_g);
    *stage = "batch_time";
    if (candidate.timestamp_us < started_us || candidate.timestamp_us - started_us > IMU_MAX_BATCH_US) {
        /* Only a finite, monotonic slow batch is discardable. Transport
         * failures have their separate, typed recovery path. */
        if (finite && candidate.timestamp_us >= started_us) *kind = IMU_READ_LATE;
        result = ESP_ERR_TIMEOUT;
        goto done;
    }
    *stage = "finite";
    if (!finite) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    *sample = candidate;
    *kind = IMU_READ_SAMPLE;
    *stage = "sample";
    result = ESP_OK;
done:
    portENTER_CRITICAL(&s_status_mux);
    const uint64_t ended_us = (uint64_t)esp_timer_get_time();
    if (ended_us < started_us ||
            (s_imu_status.sample_count && started_us < s_imu_sample.timestamp_us)) {
        *kind = IMU_READ_FATAL;
        *stage = "clock";
        result = ESP_ERR_TIMEOUT;
    }
    s_imu_status.hardware_read_seen = true;
    s_imu_status.last_hardware_stage = *stage;
    s_imu_status.last_hardware_error = result;
    s_imu_status.last_hardware_us = ended_us;
    s_imu_status.batch_time_known = s_imu_status.last_hardware_us >= started_us;
    s_imu_status.last_batch_us = s_imu_status.batch_time_known ? s_imu_status.last_hardware_us - started_us : 0;
    if (s_imu_status.batch_time_known && s_imu_status.last_batch_us > s_imu_status.max_batch_us)
        s_imu_status.max_batch_us = s_imu_status.last_batch_us;
    s_imu_status.data_ready_known = status_known;
    s_imu_status.data_ready_bits = status_known ? status : 0;
    if (*kind == IMU_READ_LATE) {
        if (s_imu_status.late_batch_count != UINT64_MAX) ++s_imu_status.late_batch_count;
        else s_imu_status.late_batch_count_saturated = true;
    }
    portEXIT_CRITICAL(&s_status_mux);
    return result;
}

/* A consumer loss invalidates the in-flight generation once. Only a subsequent
 * fresh acquisition may recover capability; user Motion intent stays off. */
static esp_err_t imu_update(void)
{
    portENTER_CRITICAL(&s_status_mux);
    esp_err_t result = s_imu_error;
    const bool fatal = s_imu_status.phase == BOARD_IMU_FATAL;
    const uint64_t generation = s_imu_generation;
    portEXIT_CRITICAL(&s_status_mux);
    if (fatal) {
        return result;
    }
    board_imu_sample_t candidate;
    const char *stage;
    imu_read_kind_t kind;
    result = imu_read_hardware(&candidate, &stage, &kind);
    portENTER_CRITICAL(&s_status_mux);
    if (s_imu_status.phase == BOARD_IMU_FATAL) {
        result = s_imu_error;
    } else if (kind == IMU_READ_FATAL) {
        result = imu_failure_locked(result, stage, true, true);
    } else if (generation != s_imu_generation) {
        result = s_imu_error;
    } else if (result == ESP_OK) {
        const uint64_t publish_us = (uint64_t)esp_timer_get_time();
        if (publish_us < candidate.timestamp_us) {
            result = ESP_ERR_TIMEOUT;
            result = imu_failure_locked(result, "publication_clock", true, true);
        } else if (publish_us - candidate.timestamp_us > IMU_MAX_AGE_US) {
            result = ESP_ERR_TIMEOUT;
            result = imu_failure_locked(result, "publication_age", false, true);
        } else if (s_imu_status.sample_count && candidate.timestamp_us <= s_imu_sample.timestamp_us) {
            result = ESP_ERR_INVALID_STATE;
            stage = "sample_time";
            result = imu_failure_locked(result, stage, true, true);
        } else {
            if (s_imu_status.phase == BOARD_IMU_RECOVERING) {
                if (s_imu_status.recovery_count != UINT64_MAX) ++s_imu_status.recovery_count;
                else s_imu_status.recovery_count_saturated = true;
            }
            s_imu_sample = candidate;
            s_imu_sample_valid = true;
            s_imu_status.phase = BOARD_IMU_ACTIVE;
            s_imu_status.failure_streak = 0;
            s_imu_status.retry_delay_ms = IMU_POLL_MS;
            s_imu_error = ESP_OK;
            s_caps.imu = true;
            imu_count_sample_locked();
        }
    } else if (kind == IMU_READ_NOT_READY || kind == IMU_READ_LATE) {
        /* Keep the original accepted cache only within its existing freshness
         * budget. No candidate publication, retimestamping or immediate retry. */
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        result = s_imu_sample_valid && time_us >= s_imu_sample.timestamp_us &&
            time_us - s_imu_sample.timestamp_us <= IMU_MAX_AGE_US ? ESP_OK : ESP_ERR_TIMEOUT;
        if (kind == IMU_READ_NOT_READY) stage = "sample_age";
        if (result != ESP_OK) result = imu_failure_locked(result, stage, false, true);
    } else {
        result = imu_failure_locked(result, stage, kind != IMU_READ_COMMUNICATION, true);
    }
    portEXIT_CRITICAL(&s_status_mux);
    return result;
}

static void power_update(void);
static void sensor_worker(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_status_mux);
    s_imu_status.worker_running = true;
    portEXIT_CRITICAL(&s_status_mux);
    TickType_t deadline = xTaskGetTickCount();
    uint64_t last_power_us = (uint64_t)esp_timer_get_time();
    for (;;) {
        portENTER_CRITICAL(&s_status_mux);
        const bool fatal = s_imu_status.phase == BOARD_IMU_FATAL;
        const uint32_t delay_ms = fatal ? POWER_POLL_MS :
            s_imu_status.retry_delay_ms ? s_imu_status.retry_delay_ms : IMU_POLL_MS;
        portEXIT_CRITICAL(&s_status_mux);
        TickType_t poll_ticks = pdMS_TO_TICKS(delay_ms);
        if (!poll_ticks) poll_ticks = 1;
        /* Skip missed periods after a slow bus transaction rather than issuing
         * a burst of catch-up reads with misleading sample intervals. */
        const TickType_t current = xTaskGetTickCount();
        const TickType_t elapsed = current - deadline;
        const TickType_t overdue = elapsed > poll_ticks ? elapsed - poll_ticks : 0;
        if (elapsed >= poll_ticks) {
            deadline = current;
        }
        (void)xTaskDelayUntil(&deadline, poll_ticks);
        const TickType_t woke = xTaskGetTickCount();
        const TickType_t wake_late = woke - deadline;
        portENTER_CRITICAL(&s_status_mux);
        s_imu_status.last_schedule_late_us = (uint64_t)(wake_late > overdue ? wake_late : overdue) * portTICK_PERIOD_MS * 1000U;
        if (s_imu_status.last_schedule_late_us > s_imu_status.max_schedule_late_us)
            s_imu_status.max_schedule_late_us = s_imu_status.last_schedule_late_us;
        portEXIT_CRITICAL(&s_status_mux);
        if (!fatal) (void)imu_update();
        portENTER_CRITICAL(&s_status_mux);
        const bool now_fatal = s_imu_status.phase == BOARD_IMU_FATAL;
        portEXIT_CRITICAL(&s_status_mux);
        if (now_fatal && s_imu_hardware_live) imu_cleanup(true);
        /* One owner gives the IMU first opportunity, then at most one battery
         * batch per second. Long/failed batches never enqueue catch-up work. */
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (s_gauge && (time_us < last_power_us || time_us - last_power_us >= POWER_POLL_MS * 1000U)) {
            power_update();
            last_power_us = (uint64_t)esp_timer_get_time();
        }
        if (now_fatal && !s_gauge) break;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_imu_status.worker_running = false;
    portEXIT_CRITICAL(&s_status_mux);
    vTaskDelete(NULL);
}

static void init_optional_imu(void)
{
    portENTER_CRITICAL(&s_status_mux);
    s_imu_status.initialization_attempted = true;
    portEXIT_CRITICAL(&s_status_mux);
    esp_err_t result = bsp_imu_init();
    if (result != ESP_OK) {
        imu_latch_fault(result, "init");
        imu_cleanup(false);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_imu_status.initialization_succeeded = true;
    portEXIT_CRITICAL(&s_status_mux);
    bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
    config.accel_odr = BMI2_ACC_ODR_100HZ;
    config.gyro_odr = BMI2_GYR_ODR_100HZ;
    config.accel_range = BMI2_ACC_RANGE_4G;
    config.gyro_range = BMI2_GYR_RANGE_2000;
    result = bsp_imu_start(&config);
    if (result != ESP_OK) {
        imu_latch_fault(result, "start");
        imu_cleanup(true);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_imu_status.start_succeeded = true;
    s_imu_hardware_live = true;
    s_imu_config = config;
    s_imu_accel_scale = 4.0f / 32768.0f;
    s_imu_gyro_scale = 2000.0f / 32768.0f;
    portEXIT_CRITICAL(&s_status_mux);
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    board_imu_sample_t first;
    const char *stage = "first_sample";
    imu_read_kind_t kind;
    do {
        const uint64_t before_us = (uint64_t)esp_timer_get_time();
        if (before_us < started_us || before_us - started_us >= IMU_START_TIMEOUT_US) {
            result = ESP_ERR_TIMEOUT;
            stage = "first_sample";
            break;
        }
        result = imu_read_hardware(&first, &stage, &kind);
        if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) {
            break;
        }
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        if (time_us < started_us || time_us - started_us >= IMU_START_TIMEOUT_US) {
            result = ESP_ERR_TIMEOUT;
            stage = "first_sample";
            break;
        }
        if (result == ESP_OK) {
            break;
        }
        TickType_t wait = pdMS_TO_TICKS(2);
        vTaskDelay(wait ? wait : 1);
    } while (true);
    if (result != ESP_OK) {
        imu_latch_fault(result, stage);
        imu_cleanup(true);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_imu_sample = first;
    s_imu_sample_valid = true;
    s_imu_error = ESP_OK;
    s_imu_status.phase = BOARD_IMU_ACTIVE;
    s_imu_status.retry_delay_ms = IMU_POLL_MS;
    imu_count_sample_locked();
    s_caps.imu = s_imu_error == ESP_OK;
    portEXIT_CRITICAL(&s_status_mux);
}

static board_power_status_t power_unknown(esp_err_t error)
{
    return (board_power_status_t){.error = error};
}

typedef struct {
    const char *stage;
    uint8_t command;
    bool elapsed_known;
    uint64_t elapsed_us;
    bool packet_time_known;
    uint64_t packet_us;
} power_read_diagnostic_t;

static uint16_t gauge_word(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static board_power_status_t power_read_hardware(power_read_diagnostic_t *diagnostic)
{
    *diagnostic = (power_read_diagnostic_t){.stage = "handle"};
    if (!s_gauge) {
        return power_unknown(ESP_ERR_INVALID_STATE);
    }
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    /* TI's incremental read keeps contiguous 0x06..0x0d in one packet:
     * temperature, voltage, battery flags and signed current. Three packets
     * accommodate each documented 4 ms stretch within the original batch gate.
     * Only a one-byte address pointer is written; no profile/control writes. */
    uint8_t bytes[12];
    const struct { uint8_t command, offset, bytes; } packets[] = {
        {GAUGE_OPERATION_STATUS, 0, 2}, {GAUGE_TEMPERATURE, 2, 8}, {GAUGE_PERCENT, 10, 2},
    };
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        diagnostic->stage = "read";
        diagnostic->command = packets[i].command;
        esp_rom_delay_us(POWER_I2C_PACKET_GAP_US);
        const uint64_t packet_started_us = (uint64_t)esp_timer_get_time();
        const esp_err_t result = i2c_master_transmit_receive(s_gauge, &packets[i].command, 1,
            bytes + packets[i].offset, packets[i].bytes, POWER_I2C_TIMEOUT_MS);
        const uint64_t time_us = (uint64_t)esp_timer_get_time();
        diagnostic->elapsed_known = time_us >= started_us;
        diagnostic->elapsed_us = diagnostic->elapsed_known ? time_us - started_us : 0;
        diagnostic->packet_time_known = time_us >= packet_started_us;
        diagnostic->packet_us = diagnostic->packet_time_known ? time_us - packet_started_us : 0;
        if (result != ESP_OK) {
            return power_unknown(result);
        }
        if (time_us < started_us || time_us - started_us > POWER_MAX_BATCH_US) {
            diagnostic->stage = time_us < started_us ? "clock" : "batch_time";
            return power_unknown(ESP_ERR_TIMEOUT);
        }
    }
    const uint16_t words[] = {gauge_word(bytes), gauge_word(bytes + 6), gauge_word(bytes + 4),
        gauge_word(bytes + 8), gauge_word(bytes + 2), gauge_word(bytes + 10)};
    diagnostic->stage = "gauge_state";
    diagnostic->command = GAUGE_OPERATION_STATUS;
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
    power_read_diagnostic_t diagnostic;
    const board_power_status_t status = power_read_hardware(&diagnostic);
    portENTER_CRITICAL(&s_status_mux);
    s_power = status;
    s_caps.battery = status.error == ESP_OK && status.gauge_available;
    portEXIT_CRITICAL(&s_status_mux);
    if (status.error != ESP_OK && status.error != s_power_logged_error) {
        ESP_LOGW(TAG, "Battery telemetry unavailable: %s stage=%s command=0x%02x batch_us_known=%d batch_us=%llu packet_us_known=%d packet_us=%llu",
                 esp_err_to_name(status.error), diagnostic.stage, (unsigned)diagnostic.command,
                 diagnostic.elapsed_known, (unsigned long long)diagnostic.elapsed_us,
                 diagnostic.packet_time_known, (unsigned long long)diagnostic.packet_us);
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
    for (;;) {
        brightness_update();
        const uint64_t after_us = (uint64_t)esp_timer_get_time();
        uint32_t wait_ms = POWER_POLL_MS;
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
            .scl_wait_us = POWER_I2C_SCL_WAIT_US,
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
        s_brightness.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_status_mux);
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_caps.brightness = true;
    portEXIT_CRITICAL(&s_status_mux);
}

static void start_sensor_worker(void)
{
    if (!s_imu_hardware_live && !s_gauge) return;
    if (xTaskCreate(sensor_worker, "board_sensor", SENSOR_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + SENSOR_PRIORITY, &s_imu_task) != pdPASS) {
        imu_latch_fault(ESP_ERR_NO_MEM, "worker_create");
        if (s_imu_hardware_live) imu_cleanup(true);
        portENTER_CRITICAL(&s_status_mux);
        s_power = power_unknown(ESP_ERR_NO_MEM);
        s_caps.battery = false;
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
    s_imu_status.worker_created = true;
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
    esp_lcd_panel_handle_t panel = NULL;
    result = bsp_display_new(&display_config, &panel);
    if (result != ESP_OK) return result;
    /* Capture the adapter's exact callback at registration time. Installing
     * after bsp_display_start would already have lost its private context. */
    esp_err_t observer_result = board_display_transfer_install(panel, bsp_display_get_panel_io());
    if (observer_result != ESP_OK) {
        ESP_LOGW(TAG, "Display transfer observer unavailable: %s", esp_err_to_name(observer_result));
    }
    s_display = bsp_display_start_with_config(&display_config);
    s_caps.display = s_display != NULL;
    s_caps.touch = bsp_display_get_input_dev() != NULL;
    if (!s_caps.display || !s_caps.touch) {
        ESP_LOGE(TAG, "Required display/touch unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    /* The BSP requests ten transactions; the board IO factory caps that
     * queue. Query the driver's actual DMA-rounded transfer limit. */
    size_t spi_max_bytes = 0;
    const esp_err_t spi_limit_result = spi_bus_get_max_transaction_len(BSP_LCD_SPI_HOST, &spi_max_bytes);
    if (spi_limit_result == ESP_OK) {
        ESP_LOGI(TAG, "Display SPI max_transaction_bytes=%zu pinned_requested_queue_depth=10 effective_queue_ceiling=%u",
                 spi_max_bytes, (unsigned)CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH);
    } else {
        ESP_LOGW(TAG, "Display SPI transfer limit unavailable: %s (pinned_requested_queue_depth=10 effective_queue_ceiling=%u)",
                 esp_err_to_name(spi_limit_result), (unsigned)CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH);
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
    start_sensor_worker();
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
    const uint64_t time_us = (uint64_t)esp_timer_get_time();
    esp_err_t result = s_imu_error;
    if (result == ESP_OK && (!s_caps.imu || !s_imu_sample_valid)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        if (time_us < s_imu_sample.timestamp_us || time_us - s_imu_sample.timestamp_us > IMU_MAX_AGE_US) {
            result = ESP_ERR_TIMEOUT;
            result = imu_failure_locked(result, "cache_age", time_us < s_imu_sample.timestamp_us, false);
        } else {
            *sample = s_imu_sample;
            /* The factory firmware documents a 90-degree clockwise sensor
             * mounting relative to the display. Rotate both vectors with the
             * same proper rotation; keep acquisition and diagnostics raw.
             * Provenance and deferred physical checks: tests/board_port/README.md. */
            sample->x_dps = -s_imu_sample.y_dps;
            sample->y_dps = s_imu_sample.x_dps;
            sample->x_g = -s_imu_sample.y_g;
            sample->y_g = s_imu_sample.x_g;
        }
    }
    s_imu_status.read_seen = true;
    s_imu_status.last_read_error = result;
    s_imu_status.last_read_us = time_us;
    portEXIT_CRITICAL(&s_status_mux);
    return result;
}

board_imu_status_t board_imu_status(void)
{
    portENTER_CRITICAL(&s_status_mux);
    board_imu_status_t status = s_imu_status;
    status.timestamp_us = (uint64_t)esp_timer_get_time();
    status.available = s_caps.imu;
    status.sample_valid = s_imu_sample_valid;
    status.error = s_imu_error;
    status.last_sample_us = s_imu_sample.timestamp_us;
    status.sample_age_known = status.sample_count != 0 && status.timestamp_us >= status.last_sample_us;
    status.sample_age_us = status.sample_age_known ? status.timestamp_us - status.last_sample_us : 0;
    status.sample_fresh = status.sample_valid && status.sample_age_known && status.sample_age_us <= IMU_MAX_AGE_US;
    portEXIT_CRITICAL(&s_status_mux);
    return status;
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
    if (s_caps.haptics && s_dispatch.enabled && !s_pending && !s_haptic_diagnostic_pending &&
        s_haptic_status.state != BOARD_HAPTIC_PENDING && s_haptic_status.state != BOARD_HAPTIC_ACTIVE) {
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

esp_err_t board_haptics_request(board_haptic_operation_t operation, board_feedback_t pattern, uint64_t *request_id)
{
    if (!request_id || (operation != BOARD_HAPTIC_TEST && operation != BOARD_HAPTIC_RECOVER) ||
        pattern < BOARD_FEEDBACK_CONFIRM || pattern > BOARD_FEEDBACK_ERROR) return ESP_ERR_INVALID_ARG;
    uint64_t time_ms = now_ms();
    portENTER_CRITICAL(&s_status_mux);
    if (!s_haptic_task || s_dispatch.exhausted || s_haptic_status.request_id == UINT64_MAX ||
        s_haptic_diagnostic_pending || s_haptic_status.state == BOARD_HAPTIC_PENDING ||
        s_haptic_status.state == BOARD_HAPTIC_ACTIVE || s_pending ||
        (operation == BOARD_HAPTIC_TEST && (!s_caps.haptics || !s_dispatch.enabled))) {
        portEXIT_CRITICAL(&s_status_mux); return ESP_ERR_INVALID_STATE;
    }
    if (operation == BOARD_HAPTIC_RECOVER) (void)haptics_dispatch_enable_locked(&s_dispatch, false);
    s_haptic_status = (board_haptic_status_t){.request_id = s_haptic_status.request_id + 1,
        .operation = operation, .pattern = pattern, .state = BOARD_HAPTIC_PENDING,
        .generation = s_dispatch.generation, .requested_ms = time_ms};
    *request_id = s_haptic_status.request_id;
    s_haptic_diagnostic_pending = true;
    portEXIT_CRITICAL(&s_status_mux);
    xTaskNotifyGive(s_haptic_task);
    return ESP_OK;
}

board_haptic_status_t board_haptics_status(void)
{
    portENTER_CRITICAL(&s_status_mux);
    board_haptic_status_t status = s_haptic_status;
    status.worker_available = s_haptic_task != NULL && !s_dispatch.exhausted;
    portEXIT_CRITICAL(&s_status_mux);
    return status;
}

esp_err_t board_haptics_cancel(uint64_t request_id)
{
    portENTER_CRITICAL(&s_status_mux);
    if (!s_haptic_task || s_haptic_status.request_id != request_id ||
        (s_haptic_status.state != BOARD_HAPTIC_PENDING && s_haptic_status.state != BOARD_HAPTIC_ACTIVE)) {
        portEXIT_CRITICAL(&s_status_mux); return ESP_ERR_INVALID_STATE;
    }
    bool enabled = s_dispatch.enabled;
    (void)haptics_dispatch_enable_locked(&s_dispatch, false);
    if (enabled) (void)haptics_dispatch_enable_locked(&s_dispatch, true);
    s_haptic_cancel_requested = true;
    portEXIT_CRITICAL(&s_status_mux);
    xTaskNotifyGive(s_haptic_task); return ESP_OK;
}
