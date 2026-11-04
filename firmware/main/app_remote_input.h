// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_registry.h"
#include "hid_engine.h"
#include "interaction.h"

#define APP_REMOTE_INPUT_JOBS 8U
#define APP_REMOTE_INPUT_LEASE_MAX_MS 3000U
#define APP_REMOTE_INPUT_DRAG_MAX_MS 30000U

typedef struct app_remote_input app_remote_input_t;
typedef enum {
    APP_REMOTE_PRODUCER_SCRIPT, APP_REMOTE_PRODUCER_DEVICE_IMU,
    APP_REMOTE_PRODUCER_DEVICE_TOUCH, APP_REMOTE_PRODUCER_SELECTION
} app_remote_input_producer_t;
typedef enum {
    APP_REMOTE_CHANGED_OWNER = 1U, APP_REMOTE_CHANGED_UI = 2U,
    APP_REMOTE_CHANGED_DRAFT = 4U, APP_REMOTE_CHANGED_SELECTION = 8U,
    APP_REMOTE_CHANGED_INPUT = 16U
} app_remote_input_change_t;
typedef struct {
    uint64_t transport_generation, ui_revision, draft_revision, selection_revision;
    uint64_t state_sequence, management_generation;
    char target_id[CP_TARGET_CAPACITY];
    bool remote_allowed, local_busy, screen_awake, overlay_open;
    bool transition_busy, native_send_active, selection_visible;
    uint8_t shortcut_modifier; /* 0x01 Ctrl or 0x08 Command. */
} app_remote_input_state_t;
typedef struct {
    hid_engine_t *engine;
    interaction_t *interaction;
    void *user;
    /* Read current coordinator-owned state. No clock, UI, network or SDK I/O. */
    bool (*observe)(void *, app_remote_input_state_t *);
    /* Current per-credential authority; absence preserves standalone legacy ports. */
    bool (*authorize)(void *, const cp_identity_t *, cp_operation_id_t);
    /* Called after successful model/state changes. Parent increments the shared
     * revisions and updates its native view state, including remote owner.
     * Native edits must update these same revisions. Increments must be checked
     * for exhaustion before mutation; the adapter rejects exhausted state. */
    void (*changed)(void *, unsigned flags);
    /* Admit an async progress/terminal result to parent's bounded completion
     * queue. False retains it locally; it never repeats the HID operation.
     * Publish the new device/owner state BEFORE completing acquire/renew. */
    bool (*complete)(void *, const cp_result_t *);
    /* Actual common coordinator paths for transport select/recover, mode,
     * keyboard submode, semantic ui.* and non-pad virtual touch. Mode/motion
     * delegates commit the desired engine epoch synchronously before returning;
     * motion.recenter instead applies the explicit shared reference reset
     * synchronously without changing epoch, Motion intent, bias or settings.
     * APPLIED acknowledges that reset, not subsequent acquisition readiness.
     * parent owns later neutral-barrier/provider completions. No success stubs:
     * provider absence is unavailable. Parent owns their async completions. */
    bool (*delegate_available)(void *, cp_operation_id_t, char reason[CP_REASON_CAPACITY]);
    cp_error_t (*delegate)(void *, const cp_command_t *, cp_result_t *);
} app_remote_input_port_t;
typedef struct {
    bool pending, active, drag_active, virtual_touch_active;
    uint64_t lease_expires_ms, input_epoch;
    uint8_t lease_id[CP_ID_BYTES], owner_session[CP_ID_BYTES], owner_principal[CP_ID_BYTES];
    app_remote_input_producer_t producer;
    unsigned pending_jobs;
} app_remote_input_view_t;

/* One serialized coordinator owner. No allocation, threads, SDK, transport or
 * direct UI calls. Network callbacks only enqueue immutable cp_command_t.
 * Parent injects the SAME engine/model used by native input, never a sidecar.
 * observe/changed/complete/delegate are bounded non-reentrant owner callbacks. */
size_t app_remote_input_bytes(void);
/* Caller storage must satisfy both this alignment and the byte requirement. */
size_t app_remote_input_alignment(void);
cp_error_t app_remote_input_init(void *memory, size_t bytes,
    const app_remote_input_port_t *, app_remote_input_t **);
/* Pure operation ownership for coordinator dispatch, independent of runtime
 * availability. Owned unavailable UI commands must never fall through to a
 * different adapter and bypass these input authority gates. The read-only
 * selection query is advertised here but remains served by the read composer. */
bool app_remote_input_owns(cp_operation_id_t);
bool app_remote_input_available(app_remote_input_t *, cp_operation_id_t,
    char reason[CP_REASON_CAPACITY]);
/* Recheck current deadline, epoch/target/transport, visible UI revision, mode,
 * lock, actual readiness, lease identity/expiry/producer and local activity.
 * Immediate model effects return APPLIED. Queued HID/barrier work returns
 * ACCEPTED and later completes through the complete callback. Unowned operations
 * return CP_NOT_IMPLEMENTED before observation, replay or lease bookkeeping. */
cp_error_t app_remote_input_execute(app_remote_input_t *, const cp_command_t *,
    uint64_t now_ms, cp_result_t *);
/* Call before admissions and after the sole hid_engine_step. No extra report
 * sender or step loop is introduced. Expiry/safety cancels via the same engine
 * and preserves the reserved neutral release barrier. */
void app_remote_input_service(app_remote_input_t *, uint64_t now_ms);
/* Last ordinary remote-report gate. It never mutates the input engine or writes
 * storage; rejection lets the engine cancel and run its reserved neutral barrier.
 * The authority provider may latch expiry in RAM. Never gate the neutral barrier. */
bool app_remote_input_report_authorized(const app_remote_input_t *, uint64_t now_ms);
/* Process ONE existing, properly mapped board sample. Returns true when
 * consumed, including calibration/reference tracking with output suppressed.
 * Local ownership uses the native source; an active DEVICE_IMU Mouse lease or
 * visible SELECTION Keyboard lease uses the remote source. Other remote roles
 * cannot inject gyro output. Pointer deltas enqueue on the SAME engine here;
 * parent must skip its old motion-model/enqueue path when true. No sensor read.
 * Focus changes update the shared selection/UI revisions through changed(). */
bool app_remote_input_motion_sample(app_remote_input_t *, const interaction_imu_sample_t *, uint64_t now_ms);
/* Trusted protocol/coordinator safety latch, not a wire-presence assertion.
 * identity=NULL means global safety; matching identity only revokes its lease.
 * Parent must enqueue/latch this from network callbacks, never call off-owner. */
void app_remote_input_revoke(app_remote_input_t *, cp_safety_reason_t,
    const cp_identity_t *, uint64_t now_ms);
void app_remote_input_snapshot(const app_remote_input_t *, app_remote_input_view_t *);

/* DEVICE_TOUCH uses authorized virtual touch.* requests on the shared model;
 * it never adopts physical contact or changes physical-takeover priority.
 * DEVICE_IMU admission requires Mouse mode; SELECTION requires a visible
 * Keyboard grid. Non-Mouse DEVICE_TOUCH requires a real UI touch provider.
 *
 * Safety release/cancel has independent bounded completion capacity and may
 * pass ordinary deadlines/provider loss. It never cancels replacement-target
 * work: old target/epoch cleanup remains pending or explicitly unverified.
 * Virtual contacts use server execution time, at most 32 distinct gesture IDs
 * per lease, deadman <=1000 ms and hard lifetime <=30000 ms. They never renew
 * physical presence, screen wake or capture consent. Non-pad surfaces require
 * the parent's actual revision-bound, non-physical UI hit provider.
 * Text result counts are actual shared-engine key-down/key-up acceptances;
 * SENT means transport acceptance, never observed host delivery.
 */

