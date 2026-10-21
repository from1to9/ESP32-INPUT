// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_SETTINGS_RECORD_SIZE 16U
#define DEVICE_SETTINGS_SCHEMA_VERSION 2U
#define DEVICE_SETTINGS_MIN_BRIGHTNESS 10U
#define DEVICE_SETTINGS_MAX_BRIGHTNESS 100U
#define DEVICE_SETTINGS_BRIGHTNESS_STEP 10U
#define DEVICE_IDLE_AI_CLICK_WINDOW_MS 280U

typedef enum { DEVICE_THEME_LIGHT, DEVICE_THEME_DARK } device_theme_t;
typedef enum { DEVICE_HOST_CTRL, DEVICE_HOST_COMMAND } device_host_modifier_t;
typedef enum {
    DEVICE_TRANSPORT_BLUETOOTH = 0, DEVICE_TRANSPORT_USB = 1
} device_transport_t;
typedef enum {
    DEVICE_IDLE_OFF = 0, DEVICE_IDLE_1_MIN = 1, DEVICE_IDLE_5_MIN = 5,
    DEVICE_IDLE_10_MIN = 10, DEVICE_IDLE_30_MIN = 30
} device_idle_timeout_t;
typedef struct {
    device_theme_t theme;
    device_host_modifier_t host_modifier;
    bool haptics_enabled;
    uint8_t brightness_percent;
    device_idle_timeout_t idle_timeout;
    device_transport_t preferred_transport;
} device_settings_t;

typedef enum {
    DEVICE_SETTINGS_OK,
    DEVICE_SETTINGS_INVALID_ARGUMENT,
    DEVICE_SETTINGS_INVALID_LENGTH,
    DEVICE_SETTINGS_INVALID_MAGIC,
    DEVICE_SETTINGS_UNSUPPORTED_VERSION,
    DEVICE_SETTINGS_INVALID_CHECKSUM,
    DEVICE_SETTINGS_INVALID_VALUE
} device_settings_status_t;
typedef enum {
    DEVICE_SETTINGS_APPLY_INVALID,
    DEVICE_SETTINGS_APPLY_UNCHANGED,
    DEVICE_SETTINGS_APPLY_CHANGED
} device_settings_apply_t;

/* Defaults: Dark, Ctrl, haptics enabled, brightness 70%, timeout five minutes,
 * preferred transport Bluetooth. Actual switching belongs to the coordinator.
 * Brightness is 10..100 inclusive, in steps of ten percentage points.
 * No hardware availability, persistence state or credentials are inferred. */
void device_settings_defaults(device_settings_t *settings);
bool device_settings_validate(const device_settings_t *settings);
bool device_settings_equal(const device_settings_t *a, const device_settings_t *b);
/* Only CHANGED should schedule a coalesced save in the external NVS adapter.
 * This component never writes, allocates, sleeps or accesses hardware. */
device_settings_apply_t device_settings_apply(device_settings_t *current,
                                              const device_settings_t *candidate);
/* Exact 16-byte little-endian wire record; no C struct/padding serialization.
 * Encode schema 2 (transport at byte 11). Decode schema 1 only with its reserved
 * byte 11 zero, migrating transport to Bluetooth. Unknown versions are rejected.
 * Decode failures leave output untouched, never silently install defaults. */
device_settings_status_t device_settings_encode(const device_settings_t *settings,
                                                uint8_t *record, size_t length);
device_settings_status_t device_settings_decode(const uint8_t *record, size_t length,
                                                device_settings_t *settings);
bool device_idle_timeout_ms(device_idle_timeout_t timeout, uint64_t *milliseconds);

typedef enum { DEVICE_INPUT_TOUCH, DEVICE_INPUT_AI, DEVICE_INPUT_SOURCE_COUNT } device_input_source_t;
typedef enum {
    DEVICE_INPUT_BEGIN, DEVICE_INPUT_MOVE, DEVICE_INPUT_END,
    DEVICE_INPUT_ACTION, DEVICE_INPUT_CANCEL
} device_input_phase_t;
typedef enum {
    DEVICE_INPUT_PASS, DEVICE_INPUT_CONSUME, DEVICE_INPUT_INVALID
} device_input_route_t;

/* Single coordinator owns this model. These are desired display states, not
 * board acknowledgements. asleep does not alter HID/link/lock/draft/motion.
 * Timeout applies in ALL power states, including unknown; no power source or
 * USB detection is inferred. Gyro/transport/render polling is NOT activity. */
typedef struct {
    device_idle_timeout_t timeout;
    bool asleep;
    bool wake_capture;
    bool clock_regressed; /* Result of the latest clock-observing call. */
    uint64_t now_ms;
    uint64_t last_activity_ms;
    uint64_t latest_token[DEVICE_INPUT_SOURCE_COUNT];
    uint64_t consumed_through[DEVICE_INPUT_SOURCE_COUNT];
    bool down[DEVICE_INPUT_SOURCE_COUNT];
    bool action_seen[DEVICE_INPUT_SOURCE_COUNT];
    bool ai_wake_waiting;
    uint64_t ai_release_ms;
} device_idle_t;

void device_idle_init(device_idle_t *policy, uint64_t now_ms);
/* Same timeout snapshots do not renew idle. A changed timeout wakes and starts
 * a full new interval; Off wakes immediately. Invalid timeout is rejected. */
bool device_idle_configure(device_idle_t *policy, device_idle_timeout_t timeout,
                            uint64_t now_ms);
/* Call only for an explicit changed setting or meaningful local control action;
 * never from gyro/RSSI/network/render polling. Wakes and renews the deadline.
 * Returns whether desired asleep changed. Does not release wake capture. */
bool device_idle_activity(device_idle_t *policy, uint64_t now_ms);
/* Record a real callback timestamp once it changes, even if its queued intent
 * is later discarded by priority handling. Never wakes by itself or changes
 * the observed clock/capture state. When awake, only newer activity extends
 * idle; repeated/older timestamps are harmless. Never call for gyro/radio. */
void device_idle_note_physical_activity(device_idle_t *policy, uint64_t activity_ms);
/* Returns whether desired asleep changed. Exact elapsed >= timeout blanks only
 * when enabled and no physical input is currently held/captured.
 * A backward clock wakes, restarts idle and consumes old/in-flight gestures. */
bool device_idle_tick(device_idle_t *policy, uint64_t now_ms);
/* Tokens are nonzero, strictly increasing PER SOURCE on each BEGIN. Use the
 * same token for MOVE/END/CANCEL and delayed click/double/hold ACTION callbacks.
 * PASS alone permits normal handling; CONSUME and INVALID must never reach HID,
 * typing, motion toggles, lock toggles or local-takeover handlers.
 * A valid BEGIN while asleep wakes and captures its whole gesture. All inputs
 * beginning before every captured contact releases are captured too. After a
 * captured AI release, capture remains until 280 ms of quiet, consuming a whole
 * wake double-click sequence. Exact boundary permits a new, fresh gesture.
 * This AI guard does not consume a fresh TOUCH after all captured contacts have
 * released. Touch-only wake capture ends on release. Consumed
 * token watermarks reject delayed callbacks even after newer gestures begin.
 * ACTION is at most once per token; stale/duplicate/malformed events do not
 * renew idle. ACTION while asleep is consumed without waking. CANCEL consumes
 * the cancelled gesture's later callbacks. Polling alone never counts activity.
 * During wake_capture suppress gyro via existing coordinator gates; suppress
 * touch movement according to its per-event route (a fresh touch can PASS
 * during the AI-only guard). Preserve motion intent. Pause keyboard focus while asleep.
 * Hardware wake failures must remain input-blocked in the owning adapter. */
device_input_route_t device_idle_input(device_idle_t *policy,
    device_input_source_t source, device_input_phase_t phase, uint64_t token,
    uint64_t now_ms);
