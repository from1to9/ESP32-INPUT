// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    bool display;
    bool touch;
    bool lock_button;
    bool imu;
    bool haptics;
    bool battery;
    bool brightness;
} board_capabilities_t;

typedef enum {
    BOARD_FEEDBACK_CONFIRM,
    BOARD_FEEDBACK_MODE,
    BOARD_FEEDBACK_LOCK,
    BOARD_FEEDBACK_ERROR,
} board_feedback_t;

typedef enum {
    BOARD_BUTTON_DOWN,
    BOARD_BUTTON_UP,
} board_button_event_t;

typedef void (*board_button_event_callback_t)(board_button_event_t event, uint64_t timestamp_us);

typedef struct {
    float x_dps;
    float y_dps;
    float z_dps;
    float x_g;
    float y_g;
    float z_g;
    uint64_t timestamp_us;
} board_imu_sample_t;

typedef enum {
    BOARD_IMU_UNINITIALIZED,
    BOARD_IMU_ACTIVE,
    BOARD_IMU_RECOVERING,
    BOARD_IMU_FATAL,
} board_imu_phase_t;

typedef struct {
    uint64_t timestamp_us;
    bool initialization_attempted;
    bool initialization_succeeded;
    bool start_succeeded;
    bool worker_created;
    bool worker_running;
    bool available;
    bool sample_valid;
    bool sample_fresh;
    board_imu_phase_t phase;
    esp_err_t error;
    uint64_t sample_count;
    bool sample_count_saturated;
    uint64_t late_batch_count; /* Finite monotonic batches rejected for exceeding 20 ms. */
    bool late_batch_count_saturated;
    uint32_t failure_streak, retry_delay_ms;
    uint64_t recovery_count;
    bool recovery_count_saturated;
    bool batch_time_known;
    uint64_t last_batch_us, max_batch_us, last_schedule_late_us, max_schedule_late_us;
    uint64_t last_sample_us;
    bool sample_age_known;
    uint64_t sample_age_us;
    bool fault_latched;
    const char *first_fault_stage;
    esp_err_t first_fault_error;
    uint64_t first_fault_us;
    bool hardware_read_seen;
    const char *last_hardware_stage;
    esp_err_t last_hardware_error;
    uint64_t last_hardware_us;
    bool data_ready_known;
    uint8_t data_ready_bits; /* Last observed STATUS byte: gyro bit 6, accel bit 7. */
    bool read_seen;
    esp_err_t last_read_error;
    uint64_t last_read_us;
} board_imu_status_t;

typedef enum {
    BOARD_TRUTH_UNKNOWN,
    BOARD_TRUTH_NO,
    BOARD_TRUTH_YES,
} board_truth_t;

typedef struct {
    uint64_t timestamp_us;
    esp_err_t error;
    bool gauge_available;
    board_truth_t battery_present;
    board_truth_t external_power;
    board_truth_t charging;
    board_truth_t full;
    bool percent_valid;
    uint8_t percent;
    bool voltage_valid;
    uint16_t voltage_mv;
    bool current_valid;
    int16_t current_ma;
    bool temperature_valid;
    float temperature_c;
} board_power_status_t;

typedef struct {
    bool requested;
    uint8_t requested_percent;
    bool pending;
    bool applied_known;
    uint8_t applied_percent;
    uint64_t applied_at_us;
    esp_err_t error;
} board_brightness_status_t;

/* Register once before board_init to select debounced BSP down/up AI events.
 * The callback must only signal the coordinator and must not block. Raw mode
 * replaces the legacy long-press callback passed to board_init; the coordinator
 * owns mutually exclusive click/double-click/900 ms hold classification. */
esp_err_t board_button_events_register(board_button_event_callback_t callback);

/* Call once before starting transport. Without raw-event registration, a
 * non-null lock callback receives the legacy physical 900 ms hold. In raw mode
 * that callback is ignored and may be null. Callbacks must not block. The
 * coordinator owns lock/unlock and must not treat a touchscreen request as a
 * trusted physical unlock event. */
esp_err_t board_init(void (*lock_callback)(void));
board_capabilities_t board_capabilities(void);

/* Nonblocking latest sample from the 100 Hz BMI270 stream. Both acceleration
 * and angular velocity use the right-handed display frame: X right, Y top,
 * Z outward toward the viewer. For the current Mosaico panel orientation this
 * is the native BSP basis (sensorX, sensorY, sensorZ), determinant +1, selected
 * from flat and text-upright hold observations on Mosaico v1.2. These poses do
 * not physically qualify v1.0 or other board profiles. Both vectors and the
 * cached timestamp are copied unchanged. Positive angular velocity follows the
 * right-hand rule. Dynamic direction and pointer feel still require device
 * qualification. The acquisition cache uses the same BSP coordinates.
 * One official corrected Bosch full-frame read supplies gyro/acceleration and
 * its data-ready status. The adapter explicitly configures 4 g/2000 dps and
 * applies the same range scales as the BSP. This burst is not proof of an
 * atomic ODR snapshot. Acceleration is in g.
 * Acquisition batches over 20 ms are rejected rather than timestamped fresh.
 * A finite, monotonic late batch may be discarded while a prior accepted
 * sample remains within 50 ms; the sampler keeps its normal periodic schedule.
 * Discards never refresh the cached timestamp or accepted-sample count. Startup
 * still requires a coherent first sample. A stale cache disables output but
 * retains bounded recovery acquisition. Communication failures are unavailable
 * and use capped backoff; clock/nonfinite/handle/config/order faults are fatal.
 * A consumer loss invalidates the in-flight generation once. Only a subsequent
 * newly acquired fresh frame can restore capability, never user Motion intent.
 * timestamp_us is the monotonic acquisition-completion time, not wall time.
 * The same timestamp may repeat: consumers must skip duplicates and use actual
 * timestamp intervals. Samples older than 50 ms fail closed. On failure output
 * is untouched; unavailable/faulted streams return an error and drop imu. */
esp_err_t board_imu_read(board_imu_sample_t *sample);
/* Nonmutating cached diagnostics: no sensor I/O, freshness latch or recovery.
 * sample_count includes accepted samples only; late_batch_count separately
 * counts discarded finite monotonic batches over 20 ms. Both saturate. Age is
 * meaningful only when known;
 * sample_valid/available/error describe current adapter state; sample_fresh
 * additionally evaluates the unchanged 50 ms age gate without latching it.
 * Initialization, start and worker-created flags retain boot milestones, not current
 * hardware qualification. Stage strings are static English literals. The first
 * fault survives recovery/cleanup/later errors and is historical, not current
 * fatal state. phase describes current state; worker_running covers the shared
 * sensor owner, which also polls battery even if the IMU has a fatal fault.
 * failure_streak/retry_delay_ms bound communication recovery attempts; timing
 * fields observe complete read spans and skipped/woken scheduling lateness.
 * last_read covers non-null cache reads. */
board_imu_status_t board_imu_status(void);

/* Nonblocking cache, polled once per second; stale after five seconds. Always
 * fills output, including unknown/invalid fields on error. Percent is the
 * gauge's existing-profile estimate, not a voltage-derived or qualified cell
 * measurement. No profile is programmed. Charging reflects current direction;
 * zero current is unknown. This BSP cannot independently sense external power. */
esp_err_t board_power_read(board_power_status_t *status);

/* Accept the latest brightness request [0,100] for the low-priority worker.
 * ESP_OK means queued/coalesced; inspect status for actual driver acceptance.
 * Hardware attempts are limited to one per 20 ms; pending values coalesce.
 * Zero sets OLED brightness to zero. Values 1..100 restore illumination without
 * pausing LVGL/touch, Bluetooth or the coordinator. No deep standby is entered. */
esp_err_t board_brightness_set(int percent);
board_brightness_status_t board_brightness_status(void);

lv_display_t *board_display_get(void);
/* UINT32_MAX waits indefinitely. Other values are milliseconds. */
bool board_display_lock(uint32_t timeout_ms);
void board_display_unlock(void);

/* Session-only preference, enabled by default when the motor is available.
 * These functions are task-context only and do not wait for a pulse. */
bool board_haptics_enabled(void);
void board_haptics_enable(bool enabled);
void board_haptic_feedback(board_feedback_t event);
/* Gate motion while a pulse is pending/active or motor-off is unconfirmed. */
bool board_haptics_active(void);

typedef enum { BOARD_HAPTIC_TEST, BOARD_HAPTIC_RECOVER } board_haptic_operation_t;
typedef enum {
    BOARD_HAPTIC_NONE, BOARD_HAPTIC_PENDING, BOARD_HAPTIC_ACTIVE,
    BOARD_HAPTIC_COMPLETED, BOARD_HAPTIC_NO_EFFECT, BOARD_HAPTIC_CANCELLED,
    BOARD_HAPTIC_FAILED, BOARD_HAPTIC_UNCERTAIN,
} board_haptic_state_t;
typedef struct {
    uint64_t request_id, generation, requested_ms, started_ms, finished_ms;
    board_haptic_operation_t operation;
    board_feedback_t pattern;
    board_haptic_state_t state;
    bool on_confirmed, off_confirmed, worker_available;
    esp_err_t error;
} board_haptic_status_t;
/* Bounded copied request to the existing haptics worker. Four native profiles
 * only: confirm 24ms/35%, mode 45ms/55%, lock 65ms/65%, error 165ms/65% with
 * its existing off gap. Cooldown/disabled/stale requests report NO_EFFECT.
 * COMPLETED requires actual on and final off driver acknowledgements; recovery
 * proves off and leaves admission disabled. No SDK work in the submission or
 * nonmutating status snapshot. New accepted IDs never wrap or alias old work. */
esp_err_t board_haptics_request(board_haptic_operation_t operation, board_feedback_t pattern, uint64_t *request_id);
/* Cancels only the matching diagnostic. Preserves the configured admission
 * preference, invalidates pending nonzero writes immediately, and lets the
 * same worker confirm motor off before publishing CANCELLED. */
esp_err_t board_haptics_cancel(uint64_t request_id);
board_haptic_status_t board_haptics_status(void);

#ifdef __cplusplus
}
#endif
