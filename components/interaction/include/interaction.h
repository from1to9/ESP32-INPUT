// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "input_core.h"
#include <stddef.h>

#define INTERACTION_PREVIEW_CAPACITY 64U
#define INTERACTION_GRID_COLUMNS 3U
#define INTERACTION_GRID_ROWS 3U
#define INTERACTION_GRID_CELLS (INTERACTION_GRID_COLUMNS * INTERACTION_GRID_ROWS)
#define INTERACTION_GRID_CENTER 4U
#define INTERACTION_CLICK_WINDOW_MS 280U
#define INTERACTION_HOLD_MS 900U
#define INTERACTION_FREEZE_MS 150U
#define INTERACTION_CALIBRATION_SAMPLES 64U

typedef enum {
    /* DIGITS is reserved; the combined Letters page already contains 0..9. */
    INTERACTION_LETTERS, INTERACTION_DIGITS, INTERACTION_SYMBOLS
} interaction_page_t;
typedef enum {
    INTERACTION_EVENT_NONE = 0,
    INTERACTION_EVENT_DOWN = 1,
    INTERACTION_EVENT_SINGLE = 2,
    INTERACTION_EVENT_DOUBLE = 4,
    INTERACTION_EVENT_HOLD = 8
} interaction_event_t;

typedef struct {
    uint64_t timestamp_us;
    /* Right-handed display frame: X right, Y top, Z outward. Both sensors
     * require the same proper rotation; stationary specific force points up. */
    float gx_dps, gy_dps, gz_dps;
    float ax_g, ay_g, az_g;
} interaction_imu_sample_t;

typedef struct {
    float pointer_gain_x, pointer_gain_y; /* 0.25..100 relative HID units/degree. */
    bool invert_x, invert_y;
    uint32_t freeze_ms; /* 0..1000, applied after touch/button/haptic transitions. */
    float deadzone_dps;
    float filter_seconds;
    float selector_cell_degrees;
    float selector_hysteresis_degrees;
    int8_t selector_axis_x, selector_axis_y; /* Signed 1=X, 2=Y, 3=Z; distinct axes. */
} interaction_config_t;

typedef enum {
    INTERACTION_MOTION_CALIBRATED = 1U,
    INTERACTION_MOTION_GRAVITY_READY = 2U,
    INTERACTION_MOTION_GRAVITY_PREDICTED = 4U,
    INTERACTION_MOTION_GEOMETRY_LIMITED = 8U,
    INTERACTION_MOTION_OUTPUT_SUPPRESSED = 16U
} interaction_motion_status_t;
typedef enum {
    INTERACTION_MOTION_BLOCK_NONE, INTERACTION_MOTION_BLOCK_CALIBRATION,
    INTERACTION_MOTION_BLOCK_SAMPLE, INTERACTION_MOTION_BLOCK_REFERENCE,
    INTERACTION_MOTION_BLOCK_GEOMETRY, INTERACTION_MOTION_BLOCK_CONTROL
} interaction_motion_block_t;

/* Observation only. Reasons identify the actual invalidation call site; counts
 * include ready->unavailable transitions, never repeated unavailable samples. */
typedef enum {
    INTERACTION_REFERENCE_NONE, INTERACTION_REFERENCE_CLOCK,
    INTERACTION_REFERENCE_CALIBRATE, INTERACTION_REFERENCE_CANCEL,
    INTERACTION_REFERENCE_MISSING, INTERACTION_REFERENCE_INVALID_SAMPLE,
    INTERACTION_REFERENCE_SAMPLE_TIME, INTERACTION_REFERENCE_HAPTIC_AGE,
    INTERACTION_REFERENCE_HAPTIC_NORMALIZE, INTERACTION_REFERENCE_ACCEL_NORM,
    INTERACTION_REFERENCE_PREDICT_NORMALIZE, INTERACTION_REFERENCE_CORRECT_NORMALIZE,
    INTERACTION_REFERENCE_TRUST_AGE, INTERACTION_REFERENCE_REASON_COUNT
} interaction_reference_reason_t;
enum {
    INTERACTION_REFERENCE_SAFE_NORM = 1U, INTERACTION_REFERENCE_NEAR_ONE = 2U,
    INTERACTION_REFERENCE_GENTLE = 4U, INTERACTION_REFERENCE_INNOVATION = 8U
};
enum {
    INTERACTION_REFERENCE_ENABLED = 1U, INTERACTION_REFERENCE_OUTPUT_READY = 2U,
    INTERACTION_REFERENCE_MOUSE = 4U, INTERACTION_REFERENCE_OWNER = 8U,
    INTERACTION_REFERENCE_LOCKED = 16U, INTERACTION_REFERENCE_TOUCH = 32U,
    INTERACTION_REFERENCE_GESTURE = 64U, INTERACTION_REFERENCE_MENU = 128U,
    INTERACTION_REFERENCE_HAPTIC = 256U, INTERACTION_REFERENCE_BUTTON = 512U,
    INTERACTION_REFERENCE_FREEZE = 1024U
};
typedef struct {
    interaction_imu_sample_t sample, previous;
    float bias[3], gravity_before[3], previous_accel[3], dt, norm;
    uint64_t trusted_us, now_ms;
    uint32_t context, checks_known, checks_pass;
    bool sample_known, previous_known, gravity_ready, accel_seen, dt_known, norm_known;
} interaction_reference_frame_t;
typedef struct {
    uint32_t rejected, near_one, gentle, innovation, haptic;
    bool saturated, first_known;
    interaction_reference_frame_t first;
} interaction_reference_episode_t;
typedef struct {
    bool known;
    interaction_reference_reason_t reason;
    interaction_reference_episode_t episode;
    interaction_reference_frame_t terminal;
} interaction_reference_incident_t;
typedef enum {
    INTERACTION_REFERENCE_RESET_NONE, INTERACTION_REFERENCE_RESET_ENABLE,
    INTERACTION_REFERENCE_RESET_RECENTER
} interaction_reference_reset_t;
typedef struct {
    /* Binding identity rejects copied preview models. Borrowed storage must
     * outlive its model and is owned by that same coordinator, not an ISR. */
    const void *owner;
    uint32_t losses[INTERACTION_REFERENCE_REASON_COUNT], active_losses;
    bool saturated, previous_known;
    interaction_imu_sample_t previous;
    interaction_reference_frame_t work;
    interaction_reference_episode_t episode;
    interaction_reference_incident_t first_active;
    uint32_t enable_resets, recenter_resets;
    interaction_reference_reset_t last_reset;
    uint64_t last_reset_ms; /* Model clock; known only when last_reset != NONE. */
} interaction_reference_diagnostics_t;

typedef struct {
    bool tap_to_click;
    uint32_t tap_max_ms, double_window_ms, contact_timeout_ms, drag_max_ms;
    float tap_slop_px, double_slop_px; /* Raw logical coordinates, before gain. */
} interaction_touch_config_t;

typedef enum {
    INTERACTION_TOUCH_BEGIN, INTERACTION_TOUCH_MOVE, INTERACTION_TOUCH_CONTACT,
    INTERACTION_TOUCH_END, INTERACTION_TOUCH_CANCEL
} interaction_touch_phase_t;
typedef struct {
    interaction_touch_phase_t phase;
    uint64_t gesture, epoch, ui_revision, timestamp_ms;
    /* MOVE is raw relative dx/dy. BEGIN/CONTACT/END are absolute logical x/y.
     * CONTACT is fresh physical evidence even without movement, not polling a
     * cached down flag. Feed ordered callback times before the current tick. */
    float x, y;
} interaction_touch_event_t;
typedef enum {
    INTERACTION_TOUCH_NONE, INTERACTION_TOUCH_CLICK,
    INTERACTION_TOUCH_LEFT_DOWN, INTERACTION_TOUCH_LEFT_UP,
    INTERACTION_TOUCH_RELEASE_REQUIRED
} interaction_touch_effect_t;
typedef struct {
    interaction_touch_effect_t effect;
    uint64_t token;
    bool allow_motion;
} interaction_touch_result_t;

/* Fixed-storage recognizer state; coordinator must use the functions below.
 * held means a down was admitted, not delivered to a host. Release cleanup is
 * latched until admission/neutral-barrier ownership is acknowledged. */
typedef struct {
    uint64_t gesture, latest_gesture, epoch, ui_revision;
    input_owner_t source;
    uint64_t started_ms, contact_ms, last_ms, observed_ms, tap_ms, serial;
    float start_x, start_y, x, y, tap_x, tap_y;
    bool active, candidate, second, held, have_tap, clock_seen, now_seen, exhausted;
    interaction_touch_result_t pending;
} interaction_touch_state_t;

/* Snapshot for UI: empty grid cells are unavailable. group=-1 selects groups;
 * otherwise cells contain characters. Geometry is three columns/three rows,
 * row-major 0..8. Groups reset to center 4. Children occupy cells 0..3 (or 0..2)
 * and reset to nearest available center 1; empty cells never receive focus. */
typedef struct {
    input_mode_t mode;
    interaction_page_t page;
    bool lowercase, keyboard_motion;
    int group;
    unsigned int focus;
    char cells[INTERACTION_GRID_CELLS][7];
    char preview[INTERACTION_PREVIEW_CAPACITY + 1];
    size_t preview_length;
    bool motion_enabled, button_down, touch_active, menu_open, haptic_active;
    unsigned int touch_divisor;  /* 1, 4 or 8. */
    bool calibrated, calibrating;
    size_t calibration_samples;
    uint32_t motion_status;
    interaction_motion_block_t motion_block_reason;
    bool sending, send_settling;
    size_t send_total, send_accepted;
    uint64_t send_token;
} interaction_view_t;

/* Single coordinator owns mutations; UI/board/transport only exchange values.
 * No HID, board, heap, threads or clock access is performed by this model. */
typedef struct {
    interaction_view_t view;
    input_state_t control;
    input_owner_t motion_source;
    interaction_config_t config;
    interaction_touch_config_t touch_config;
    interaction_touch_state_t touch;
    bool output_ready, touch_motion_ready, clock_seen, sample_seen;
    uint64_t now_ms, freeze_until_ms, sample_us;
    bool click_pending, second_click, hold_emitted, press_consumed;
    uint64_t down_ms, click_ms;
    bool send_open;
    uint64_t send_epoch;
    float bias[3], calibration_mean[3], calibration_m2[3];
    float calibration_accel_ref[3];
    uint64_t calibration_start_us;
    float gravity[3], previous_accel[3], acquire_accel[3];
    bool gravity_ready, gravity_predicted, accel_seen, geometry_ready;
    uint64_t gravity_trusted_us, gravity_acquire_us, geometry_acquire_us;
    size_t gravity_acquire_samples;
    float filtered[2], selector_angle[2], touch_carry[2];
    int selector_cell[2];
    input_motion_t pointer_motion;
    interaction_reference_diagnostics_t *reference_diagnostics;
} interaction_t;

void interaction_init(interaction_t *model);
void interaction_get_view(const interaction_t *model, interaction_view_t *view);
/* Optional fixed caller storage. Bind/reset only observations, never control
 * state. Init detaches; copied models cannot write their original's evidence.
 * Getter returns NULL for absent/stale binding. No allocation or I/O occurs. */
void interaction_reference_diagnostics_bind(interaction_t *model,
    interaction_reference_diagnostics_t *storage);
void interaction_reference_diagnostics_reset(interaction_t *model);
const interaction_reference_diagnostics_t *interaction_reference_diagnostics_get(
    const interaction_t *model);
/* Copy canonical coordinator state each tick. output_ready includes the HID
 * release gate/congestion. Epoch/mode/owner/link/lock changes reset motion and
 * local selection, cancel pending clicks and mark active sends for settlement.
 * Apply the engine's final accepted count with send_update before allowing
 * preview editing; this may happen before or after sync changes the epoch. */
void interaction_sync(interaction_t *model, const input_state_t *state,
                      bool output_ready, uint64_t now_ms);
/* Local touch motion may use bounded coordinator buffering while the transport
 * is transiently busy. This does not enable click, drag, gyro or remote output.
 * sync resets this override; the coordinator reapplies its current lifecycle gate. */
void interaction_set_touch_motion_ready(interaction_t *model, bool ready);
bool interaction_configure(interaction_t *model, const interaction_config_t *config);
bool interaction_set_motion(interaction_t *model, bool enabled);
bool interaction_set_motion_owned(interaction_t *model, input_owner_t source, bool enabled);
/* Intentional current-grip reference reset, distinct from gyro bias calibration.
 * Requires enabled Mouse intent, matching owner/source, calibration and output
 * readiness. Preserves bias/config/intent/sample-time guards and fault history.
 * Next real stable samples use ordinary acquisition; recovery emits no delta. */
bool interaction_recenter_owned(interaction_t *model, input_owner_t source);
/* Explicit local Input-setting reset while Mouse output is paused by a menu.
 * Clears the grip reference without enabling Motion or changing calibration. */
bool interaction_reset_grip_owned(interaction_t *model, input_owner_t source);
void interaction_set_keyboard_motion(interaction_t *model, bool enabled);
void interaction_set_suppressed(interaction_t *model, bool touch, bool menu,
                                bool haptic, uint64_t now_ms);
/* Preserve an asynchronous touch/button freeze even if its complete gesture
 * occurred between ticks. A later absolute deadline extends the freeze and
 * clears motion/filter/touch carry; older/repeated deadlines are idempotent.
 * Does not observe the clock, change current touch state or cancel intent. */
void interaction_freeze_until(interaction_t *model, uint64_t deadline_ms);
/* Extend settling from an original callback time using configured freeze_ms. */
void interaction_freeze_after(interaction_t *model, uint64_t event_ms);
bool interaction_set_touch_divisor(interaction_t *model, unsigned int divisor);
input_delta_t interaction_touch_move(interaction_t *model, float dx, float dy);
input_delta_t interaction_touch_move_owned(interaction_t *model, input_owner_t source, float dx, float dy);
typedef struct { int16_t x, y; } interaction_touch_delta_t;
/* Preserve the whole validated touch displacement plus fractional carry.
 * The HID engine owns bounded splitting into signed 8-bit relative reports. */
interaction_touch_delta_t interaction_touch_displacement_owned(interaction_t *model,
    input_owner_t source, float dx, float dy);

/* Defaults: enabled, press <=250 ms, second BEGIN within 280 ms/16 px of the
 * first release, tap slop 8 px, fresh contact <=1000 ms, drag <=30000 ms.
 * Bounds: press 50..500, double 100..500, contact 100..2000, drag 1000..60000
 * milliseconds; slop 2..24, double slop 2..48 pixels and >= tap slop.
 * Config changes consume the current gesture and latch held cleanup. */
void interaction_touch_defaults(interaction_touch_config_t *config);
bool interaction_touch_configure(interaction_t *model,
                                 const interaction_touch_config_t *config);
/* Only route eligible pad events here after whole-gesture wake/takeover gates.
 * tap_allowed=false latches no-click for that press (scroll/explicit drag,
 * capture loss, etc.); movement may continue for an existing explicit drag.
 * Local mode/owner/epoch/revision/lock are independently checked. Single tap
 * returns CLICK at release. Second eligible BEGIN returns LEFT_DOWN; its END
 * returns LEFT_UP, giving two clicks or a held drag, never a third click.
 * MOVE only observes raw slop; when allow_motion, call the existing fine-motion
 * path exactly once. Never emit movement before a returned down is admitted. */
interaction_touch_result_t interaction_touch_input(interaction_t *model,
    const interaction_touch_event_t *event, uint64_t now_ms,
    uint64_t current_ui_revision, bool tap_allowed);
interaction_touch_result_t interaction_touch_input_owned(interaction_t *model, input_owner_t source,
    const interaction_touch_event_t *event, uint64_t now_ms,
    uint64_t current_ui_revision, bool tap_allowed);
interaction_touch_result_t interaction_touch_tick(interaction_t *model,
    uint64_t now_ms, uint64_t current_ui_revision);
interaction_touch_result_t interaction_touch_cancel(interaction_t *model);
/* An excursion disqualifies a tap without ending a captured swipe or drag. */
void interaction_touch_disqualify_tap(interaction_t *model);
/* Call immediately for each proposed effect. true means HID queue admission,
 * or the coordinator has established its neutral cleanup barrier for
 * RELEASE_REQUIRED; never host delivery. A rejected UP returns latched
 * RELEASE_REQUIRED. Rejected down/click is consumed with no automatic retry.
 * Stale tokens cannot revive or settle another effect. */
interaction_touch_result_t interaction_touch_admit(interaction_t *model,
    uint64_t token, bool accepted);
bool interaction_touch_holding(const interaction_t *model);

/* Touch select and physical confirm share this grid/preview path. Selection is
 * local and never sends a key. Every successful confirmation recenters focus. */
bool interaction_select_cell(interaction_t *model, unsigned int cell);
bool interaction_confirm(interaction_t *model);
void interaction_back(interaction_t *model);
bool interaction_set_page(interaction_t *model, interaction_page_t page);
bool interaction_set_lowercase(interaction_t *model, bool lowercase);
bool interaction_append(interaction_t *model, unsigned char character);
bool interaction_delete(interaction_t *model);
bool interaction_clear(interaction_t *model);

typedef enum {
    INTERACTION_EDIT_ACTIVATE, INTERACTION_EDIT_CONFIRM, INTERACTION_EDIT_FOCUS,
    INTERACTION_EDIT_BACK, INTERACTION_EDIT_CASE, INTERACTION_EDIT_PAGE,
    INTERACTION_EDIT_APPEND, INTERACTION_EDIT_DELETE, INTERACTION_EDIT_CLEAR
} interaction_edit_kind_t;
typedef struct {
    interaction_edit_kind_t kind;
    unsigned int cell;
    interaction_page_t page;
    bool lowercase;
    const char *text; /* APPEND borrows length printable ASCII bytes for this call. */
    size_t length; /* APPEND byte count or DELETE count; atomic capacity checks. */
} interaction_edit_t;
/* Both native wrappers and remote coordinators use these same edit helpers.
 * source must match actual ownership; remote lease/session/UI authorization is
 * rechecked by its coordinator before every call. No local-presence claim. */
bool interaction_edit_owned(interaction_t *model, input_owner_t source, const interaction_edit_t *edit);

/* Explicit send only: begin freezes the preview and returns a unique nonzero
 * token. Coordinator enqueues its copied preview with HID, then reports engine
 * key-down acceptance. On enqueue rejection, update(token,0,false) unfreezes it.
 * Monotonic accepted counts remove exactly that prefix. active=false settles
 * completion/cancellation; stale tokens and invalid counts never change text.
 * No Enter or automatic retry is produced. A boundary prevents active=true
 * from reopening the old session. Settled progress remains visible until the
 * next successful append/delete/clear resets it. Transport acceptance is not
 * host delivery. output_ready=false alone pauses and never settles a session. */
uint64_t interaction_send_begin(interaction_t *model);
uint64_t interaction_send_begin_owned(interaction_t *model, input_owner_t source);
bool interaction_send_update(interaction_t *model, uint64_t token,
                             size_t accepted, bool active);

/* Feed debounced physical levels and poll tick every coordinator iteration.
 * First release waits 280 ms; a second DOWN before that deadline yields DOUBLE
 * on its release. HOLD fires once at >=900 ms while down, suppressing clicks.
 * DOWN freezes motion immediately. Keyboard gyro focus remains frozen through
 * pending single/double classification; the mouse release freeze stays 150 ms.
 * Caller interprets events by current mode;
 * only trusted physical HOLD may toggle lock. Clock regression drops gestures.
 * Consume the takeover press's click while still allowing a later HOLD. */
unsigned int interaction_button_update(interaction_t *model, bool down,
                                       uint64_t now_ms);
unsigned int interaction_tick(interaction_t *model, uint64_t now_ms);
void interaction_consume_press(interaction_t *model);

/* Recalibration requires >=64 consecutive stationary samples spanning >=500 ms:
 * acceleration norm within 0.08 g of 1, raw gyro <=5 dps and variance <=0.04.
 * Samples must be finite with 0<dt<=50 ms. Invalid/gapped/suppressed intervals
 * clear motion carry. Board supplies already axis-mapped dps and g units.
 * Mouse returns bounded relative deltas; keyboard updates ONLY local focus
 * with hysteresis and never confirms. Audio always returns zero. */
void interaction_calibrate(interaction_t *model);
/* Coordinator calls only for its matching active calibration job. Stops
 * accumulation and invalidates bias/reference; late samples cannot finish it.
 * A fresh explicit calibrate call is required before calibration can resume. */
void interaction_calibration_cancel(interaction_t *model);
input_delta_t interaction_motion_sample(interaction_t *model,
                                        const interaction_imu_sample_t *sample);
input_delta_t interaction_motion_sample_owned(interaction_t *model, input_owner_t source,
                                              const interaction_imu_sample_t *sample);
