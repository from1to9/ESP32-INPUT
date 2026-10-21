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
    float gx_dps, gy_dps, gz_dps;
    float ax_g, ay_g, az_g;
} interaction_imu_sample_t;

typedef struct {
    float pointer_gain;          /* Relative HID units per degree. */
    float deadzone_dps;
    float filter_seconds;
    float selector_cell_degrees;
    float selector_hysteresis_degrees;
} interaction_config_t;

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
    bool sending, send_settling;
    size_t send_total, send_accepted;
    uint64_t send_token;
} interaction_view_t;

/* Single coordinator owns mutations; UI/board/transport only exchange values.
 * No HID, board, heap, threads or clock access is performed by this model. */
typedef struct {
    interaction_view_t view;
    input_state_t control;
    interaction_config_t config;
    bool output_ready, clock_seen, sample_seen;
    uint64_t now_ms, freeze_until_ms, sample_us;
    bool click_pending, second_click, hold_emitted, press_consumed;
    uint64_t down_ms, click_ms;
    bool send_open;
    uint64_t send_epoch;
    float bias[3], calibration_mean[3], calibration_m2[3];
    float calibration_accel_ref[3];
    uint64_t calibration_start_us;
    float filtered[2], selector_angle[2], touch_carry[2];
    int selector_cell[2];
    input_motion_t pointer_motion;
} interaction_t;

void interaction_init(interaction_t *model);
void interaction_get_view(const interaction_t *model, interaction_view_t *view);
/* Copy canonical coordinator state each tick. output_ready includes the HID
 * release gate/congestion. Epoch/mode/owner/link/lock changes reset motion and
 * local selection, cancel pending clicks and mark active sends for settlement.
 * Apply the engine's final accepted count with send_update before allowing
 * preview editing; this may happen before or after sync changes the epoch. */
void interaction_sync(interaction_t *model, const input_state_t *state,
                      bool output_ready, uint64_t now_ms);
bool interaction_configure(interaction_t *model, const interaction_config_t *config);
bool interaction_set_motion(interaction_t *model, bool enabled);
void interaction_set_keyboard_motion(interaction_t *model, bool enabled);
void interaction_set_suppressed(interaction_t *model, bool touch, bool menu,
                                bool haptic, uint64_t now_ms);
/* Preserve an asynchronous touch/button freeze even if its complete gesture
 * occurred between ticks. A later absolute deadline extends the freeze and
 * clears motion/filter/touch carry; older/repeated deadlines are idempotent.
 * Does not observe the clock, change current touch state or cancel intent. */
void interaction_freeze_until(interaction_t *model, uint64_t deadline_ms);
bool interaction_set_touch_divisor(interaction_t *model, unsigned int divisor);
input_delta_t interaction_touch_move(interaction_t *model, float dx, float dy);

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
input_delta_t interaction_motion_sample(interaction_t *model,
                                        const interaction_imu_sample_t *sample);
