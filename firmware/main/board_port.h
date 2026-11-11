// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_imu_stream.h"
#include "board_sensor_journal.h"
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

/* Accepted samples strictly between two known samples in the current
 * source generation. The ending sample must still be fresh. Older frames
 * support attitude prediction only. Zero means no history was copied. */
enum { BOARD_IMU_HISTORY_CAPACITY = 20 };
size_t board_imu_copy_history(uint64_t after_us, uint64_t through_us,
    board_imu_sample_t samples[BOARD_IMU_HISTORY_CAPACITY - 2]);
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

/* Optional sole-consumer ordered delivery from the existing sensor owner.
 * Attach after IMU initialization was attempted (including failed startup).
 * One nonzero token owns the stream until detach; attach/fence start a new
 * delivery epoch and flush queued history. A pre-boundary acquisition cannot
 * enter the new epoch. Neither action changes the legacy cache or hardware
 * capability, recovery state, first-fault history or Motion intent.
 * Drain is nonblocking, copies at most capacity [1,8], and performs no I2C.
 * ESP_OK means a valid query: only DATA/count>0 delivers samples. All other
 * states have count zero and leave frame storage untouched. A discontinuity
 * is acknowledged before later DATA; reasons may combine across boundaries.
 * Overflow drops the entire queued segment plus its incoming frame. Stale
 * queued history (>50 ms) is dropped as one segment, even if latest is fresh.
 * Sequence counts accepted publications separately from diagnostic counters;
 * sequence/epoch are copied atomically with each unchanged native sample.
 * Exhausted identifiers latch delivery FAILED rather than wrapping. A valid
 * query still returns that state; source_error independently observes current
 * cache/hardware availability and may be ESP_OK. Stale/future latest-cache
 * observations return ESP_ERR_TIMEOUT without changing hardware capability,
 * cache validity, recovery generation, fault history or legacy read markers.
 * Only legacy consumption/acquisition latch source health. Invalid arguments
 * or tokens leave outputs untouched.
 * Use caller-owned persistent batch storage; model work belongs outside the
 * lock. Recheck freshness and your control epoch before use/output. Fence does
 * not retract a batch already copied by the caller. */
esp_err_t board_imu_stream_attach(board_imu_consumer_t *consumer);
esp_err_t board_imu_stream_fence(board_imu_consumer_t consumer, uint64_t *delivery_epoch);
esp_err_t board_imu_stream_drain(board_imu_consumer_t consumer,
    board_imu_frame_t *frames, size_t capacity, board_imu_batch_t *batch);
esp_err_t board_imu_stream_detach(board_imu_consumer_t consumer);

/* Nonblocking cache, polled once per second; stale after five seconds. Always
 * fills output, including unknown/invalid fields on error. Percent is the
 * gauge's existing-profile estimate, not a voltage-derived or qualified cell
 * measurement. No profile is programmed. Charging reflects current direction;
 * zero current is unknown. This BSP cannot independently sense external power. */
esp_err_t board_power_read(board_power_status_t *status);
/* Same copied/derived freshness result as board_power_read, without modifying
 * any cache, capability or history. Return is the observed power error. */
esp_err_t board_power_observe(board_power_status_t *status);

/* Stateless read-only cursors: after is the last delivered global record ID,
 * zero starts from ID1. GAP returns count0, exact lost_records and next_after
 * (oldest-1); adopt that cursor explicitly before resuming. Future cursors and
 * invalid arguments return INVALID_ARG with all outputs untouched. DATA and
 * EMPTY may coexist with journal EXHAUSTED, preserving its retained records.
 * Each query atomically copies consistent bounds plus at most eight records.
 * ESP_OK means a valid query, not a healthy hardware source. Root API owns boot
 * identity validation; journal IDs are scoped to that boot. Storage is fixed
 * external RAM, initialized once by board_init; no allocation or sensor I/O. */
esp_err_t board_sensor_journal_read(uint64_t after, board_sensor_record_t *records,
    size_t capacity, board_sensor_batch_t *batch);
/* Same batch metadata without records; no cursor acknowledgement/mutation. */
esp_err_t board_sensor_journal_status(board_sensor_batch_t *batch);
/* Task-context tee of existing touch/button observations only. Validates kind,
 * flags, payload and unknown/reserved fields; cannot inject IMU/power events.
 * Caller sets sequence=observed_us=0; board assigns its ID/publication time.
 * Returns INVALID_ARG for malformed input, INVALID_STATE if not retained.
 * This never changes input authority, touch callbacks or any sensor state. */
esp_err_t board_sensor_journal_publish_input(const board_sensor_record_t *record);

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
