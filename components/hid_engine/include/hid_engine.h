// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "input_core.h"
#include <stddef.h>

#define HID_ENGINE_KEYBOARD_REPORT_ID 1U
#define HID_ENGINE_MOUSE_REPORT_ID 2U
#define HID_ENGINE_KEYBOARD_REPORT_LENGTH 8U
#define HID_ENGINE_KEYBOARD_LED_LENGTH 1U
#define HID_ENGINE_MOUSE_REPORT_LENGTH 4U
#define HID_ENGINE_QUEUE_CAPACITY 32U
#define HID_ENGINE_TEXT_MAX_LENGTH 32U
#define HID_ENGINE_MAX_SEND_FAILURES 3U
#define HID_ENGINE_CONGESTION_TIMEOUT_MS 250U
#define HID_ENGINE_MOUSE_DISPLACEMENT_MAX 4096
#define HID_ENGINE_MOUSE_BUTTON_MASK 0x07U
#define HID_ENGINE_KEY_USAGE_MAX 0x65U

/* Report-only HOGP: IDs select characteristics and are NOT payload prefixes.
 * Keyboard input: modifiers, reserved, six usages; LED output: five LED bits.
 * Mouse input: three button bits, signed X, signed Y, signed wheel (-127..127).
 * This descriptor does not advertise or implement a Boot Protocol interface. */
extern const uint8_t hid_engine_report_descriptor[];
extern const size_t hid_engine_report_descriptor_length;

typedef bool (*hid_engine_send_fn)(void *context, uint8_t report_id,
                                  const uint8_t *data, size_t length);
typedef void (*hid_engine_cancel_fn)(void);

typedef enum {
    HID_ENGINE_ENQUEUE_OK,
    HID_ENGINE_ENQUEUE_INVALID,
    HID_ENGINE_ENQUEUE_STALE,
    HID_ENGINE_ENQUEUE_BLOCKED,
    HID_ENGINE_ENQUEUE_FULL
} hid_engine_enqueue_result_t;

typedef enum {
    HID_ENGINE_STEP_IDLE,
    HID_ENGINE_STEP_BLOCKED,
    HID_ENGINE_STEP_SENT,
    HID_ENGINE_STEP_SEND_FAILED,
    HID_ENGINE_STEP_RETRY_EXHAUSTED
} hid_engine_step_result_t;

/* Internal queue storage is public only to allow static allocation. One action
 * is movement (possibly split), scroll/drag state, or a tap plus its release. */
typedef struct {
    uint64_t epoch;
    input_owner_t owner;
    input_mode_t mode;
    uint8_t data[HID_ENGINE_KEYBOARD_REPORT_LENGTH];
    uint8_t report_id;
    uint8_t length;
    bool needs_release;
    bool release_next;
    bool text_character;
    bool sets_mouse_buttons;
    bool buffered_motion;
    int16_t remaining_x, remaining_y;
    uint64_t motion_since_ms;
} hid_engine_action_t;

/* One coordinator owns ALL mutations; adapters may only read state/ready.
 * Do not mutate the embedded input_state_t with input_core APIs directly.
 * Not thread-safe or reentrant: send must copy data before returning and must
 * not call back into this engine. Acceptance is not host/UI acknowledgement. */
typedef struct {
    input_state_t state;
    hid_engine_cancel_fn before_cancel;
    bool ready;
    bool congested;
    bool congestion_cancelled;
    bool time_observed;
    uint64_t observed_ms;
    uint64_t congestion_since_ms;
    uint64_t motion_since_ms;
    size_t buffered_motion_count;
    hid_engine_action_t queue[HID_ENGINE_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    uint8_t release_stage;
    uint8_t send_failures;
    /* Last transport-accepted explicit drag state, 0 or 1. Invalidation clears
     * local intent immediately; release_pending still guards host recovery. */
    uint8_t mouse_buttons;
    size_t text_accepted;
    size_t text_completed;
    size_t text_pending;
} hid_engine_t;

void hid_engine_init(hid_engine_t *engine);
/* Optional coordinator callback, registered after init. Runs synchronously
 * before cancel advances the epoch, including transport expiry and send failure.
 * It must not reenter this engine. Unchanged observations never call it;
 * init clears the registration. Other state setters retain their own boundaries. */
void hid_engine_set_cancel_callback(hid_engine_t *engine, hid_engine_cancel_fn callback);
/* ready means the transport can accept BOTH input report IDs. Losing readiness
 * invalidates work just like disconnect. ready=true with connected=false is
 * normalized to false. After retry exhaustion, an explicit ready=true call
 * rearms the bounded recovery budget. Never replay queued work on reconnect. */
void hid_engine_set_link(hid_engine_t *engine, bool connected, bool ready);
/* Call once each coordinator tick BEFORE action admission and step, with
 * monotonic milliseconds and the current transport backpressure snapshot.
 * Congestion blocks ordinary action admission except explicit drag release (0)
 * and bounded local touch displacement. All report submissions stay blocked without
 * spending send retries. An episode >=250 ms cancels once, advances the epoch and
 * preserves the zero-release barrier until congestion clears. Short episodes
 * preserve queued actions, including mandatory key/button releases.
 * A backward timestamp cancels immediately (once within an ongoing congestion
 * episode); the supplied timestamp becomes the new observation baseline.
 * The first uncongested observation checks expiry BEFORE resetting the episode
 * timer/latch, so resuming after the deadline cannot replay stale work. Returns true
 * exactly when this observation invalidated work and requested fresh releases.
 * Retained touch displacement also expires at 250 ms from its oldest source
 * sample, including across ready intervals. Link/mode/lock changes do not reset
 * an ongoing congestion episode. */
bool hid_engine_observe_transport(hid_engine_t *engine, uint64_t now_ms,
                                  bool congested);
bool hid_engine_set_mode(hid_engine_t *engine, input_mode_t mode);
void hid_engine_set_locked(hid_engine_t *engine, bool locked);
bool hid_engine_set_owner(hid_engine_t *engine, input_owner_t owner);
bool hid_engine_set_gyro(hid_engine_t *engine, bool enabled);
bool hid_engine_set_gyro_owned(hid_engine_t *engine, input_owner_t source, bool enabled);
/* Also use cancel for target replacement without a disconnect. Always advances
 * the epoch, drops queued actions and requires both zero release reports. */
void hid_engine_cancel(hid_engine_t *engine);

/* Capture state.epoch and state.owner when creating an action. All enqueues
 * recheck link, readiness, congestion, lock, release gate, mode, owner and epoch; step
 * checks them again. Invalid/full requests never partially enqueue.
 * The explicit drag release may queue during congestion; step still waits.
 * Mouse deltas/wheel must be -127..127. Click buttons must be 1..7 (bitmask).
 * Explicit keys are single taps: modifier 0..255 and usage 4..0x65 or usage 0
 * with a nonzero modifier. Usages 1..3 are HID error codes, not keys. */
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_move(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int x, int y);
/* One live IMU delta for the coordinator's immediate HID service. Older input
 * retains the turn if queued, so IMU deltas cannot wait behind that input. */
hid_engine_enqueue_result_t hid_engine_enqueue_aim_move(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int x, int y);
/* Wide touch displacement, split into the unchanged signed 8-bit wire axes.
 * Local samples may be retained during transient congestion; remote callers
 * still receive BLOCKED so their command receipt can request a retry.
 * Only adjacent local touch actions coalesce; buttons/clicks/scroll are barriers.
 * Each axis is bounded by MOUSE_DISPLACEMENT_MAX, with atomic FULL on overflow.
 * observe_transport expires all retained motion at the existing 250 ms limit,
 * even across ready intervals. Callers must observe before servicing the queue. */
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_displacement(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int x, int y, uint64_t sample_ms, uint64_t now_ms);
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_click(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int buttons);
/* Explicit left-button drag state: buttons is 1 (hold) or 0 (release).
 * Each state change occupies one queue slot and has no automatic release.
 * A hold and subsequent movement/scroll reserve one slot for its release;
 * if the hold cannot reserve that capacity, it returns FULL without changing
 * state. A release therefore fits while an unreleased hold is queued/active.
 * Release (0) may queue while congested so backpressure cannot discard it;
 * transport submission still waits. Movement and scroll preserve the most
 * recently accepted held state. A click
 * is rejected while drag is active or queued; cancellation always clears the
 * drag and requires the normal zero-release barrier before further output. */
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_buttons(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int buttons);
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_scroll(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, int wheel);
hid_engine_enqueue_result_t hid_engine_enqueue_key(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int modifier,
    unsigned int usage);
/* Explicit composite-keyboard tap while staying in Mouse mode. Uses the same
 * epoch/owner/transport and down/up recovery path as keyboard taps, rejects an
 * active or queued drag, and never changes a keyboard preview or text batch. */
hid_engine_enqueue_result_t hid_engine_enqueue_mouse_key(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, unsigned int modifier, unsigned int usage);
/* Printable US ANSI bytes only, explicit length 1..TEXT_MAX_LENGTH; no NUL or
 * implicit Enter. Copies each character to one action (two reports).
 * Only one text batch may be queued at a time. text_accepted counts successful
 * key-down submissions in the last batch; text_completed counts matching
 * key-ups. Both survive cancellation for caller-managed unsent-preview repair.
 * A failed key-up does not cause an accepted character to be replayed. */
hid_engine_enqueue_result_t hid_engine_enqueue_text(hid_engine_t *engine,
    uint64_t epoch, input_owner_t owner, const char *text, size_t length);

/* Submit at most one report, including recovery. Releases always precede normal
 * output, even while locked/audio. A failed send invalidates all queued work.
 * Recovery retries only zero reports, up to MAX_SEND_FAILURES total failures
 * until BOTH zero reports succeed; then the budget resets. Exhaustion clears
 * ready and leaves release_pending set for explicit transport recovery.
 * The coordinator must recover or disconnect a failed transport on exhaustion;
 * no portable engine can prove release at an unreachable host.
 * Call at a transport-paced interval after observe_transport; this engine
 * supplies no clock or delay. Congestion returns BLOCKED without calling send. */
hid_engine_step_result_t hid_engine_step(hid_engine_t *engine,
    hid_engine_send_fn send, void *context);
