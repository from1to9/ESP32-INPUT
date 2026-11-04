// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEVICE_SETTINGS_RECORD_SIZE 56U
#define DEVICE_SETTINGS_V3_RECORD_SIZE 40U
#define DEVICE_SETTINGS_LEGACY_RECORD_SIZE 16U
#define DEVICE_SETTINGS_SCHEMA_VERSION 5U
#define DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES 18U
#define DEVICE_SETTINGS_BLUETOOTH_DEFAULT_NAME "ESP32"
#define DEVICE_SETTINGS_V5_DEFAULT_NAME "ESP32-INPUT"
#define DEVICE_INPUT_NAME_SUFFIX "-IN"
#define DEVICE_AUDIO_NAME_SUFFIX "-AUD"
#define DEVICE_INPUT_NAME_MAX (DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + sizeof(DEVICE_INPUT_NAME_SUFFIX) - 1U)
#define DEVICE_AUDIO_NAME_MAX (DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + sizeof(DEVICE_AUDIO_NAME_SUFFIX) - 1U)
/* The sole legacy alias. All other valid base names remain literal. */
static inline const char *device_base_name(const char *name)
{
    return !strcmp(name, DEVICE_SETTINGS_V5_DEFAULT_NAME) ? DEVICE_SETTINGS_BLUETOOTH_DEFAULT_NAME : name;
}
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
    bool tap_to_click;
    char bluetooth_name[DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + 1U];
    uint16_t motion_gain_x_centi, motion_gain_y_centi; /* 25..10000; HID units/degree * 100. */
    uint16_t motion_deadzone_mdps; /* 0..10000; milli-degrees/second. */
    uint16_t motion_filter_ms, motion_freeze_ms; /* Each 0..1000 milliseconds. */
    bool motion_invert_x, motion_invert_y;
    bool navigation;
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

/* Defaults: Dark, Ctrl, haptics enabled, brightness 50%, timeout five minutes,
 * preferred transport Bluetooth, tap-to-click enabled, base device name ESP32;
 * motion X/Y gain 4, deadzone 0.5 dps, filter 30 ms, freeze 150 ms, no inversion;
 * optional Navigation layout disabled.
 * Actual switching/name application belongs to the coordinator/adapters.
 * Brightness is 10..100 inclusive, in steps of ten percentage points.
 * No hardware availability, persistence state or credentials are inferred. */
void device_settings_defaults(device_settings_t *settings);
bool device_settings_validate(const device_settings_t *settings);
/* Shared BLE/settings validator: explicit 1..18 UTF-8 bytes, excluding NUL.
 * Reject malformed/overlong UTF-8, surrogates, NUL/C0/C1 controls and outer
 * ASCII space. Input need not be terminated; this never reads beyond length. */
bool device_settings_input_name_valid(const char *utf8, size_t length);
bool device_settings_bluetooth_name_valid(const char *utf8, size_t length);
bool device_settings_equal(const device_settings_t *a, const device_settings_t *b);
/* Only CHANGED should schedule a coalesced save in the external NVS adapter.
 * This component never writes, allocates, sleeps or accesses hardware. */
device_settings_apply_t device_settings_apply(device_settings_t *current,
                                              const device_settings_t *candidate);
/* Encode exact 56-byte schema 5, never C struct/padding serialization. Decode
 * also accepts 16-byte schema 1/2, 40-byte schema 3 and 56-byte schema 4. Preserve every old
 * value and default only absent fields. V1 byte 11 must be zero (Bluetooth).
 * V3/v4: tap flag at 12, name length at 13, zero-padded name[19] at 14..32,
 * reserved zero at 33..35. V4 LE uint16 gain X/Y at 36/38, deadzone at 40,
 * filter/freeze at 42/44; inversion bits X=1/Y=2 at 46; v4 47..51 zero.
 * V5 keeps the v4 layout, uses byte 47 for Navigation (0/1), 48..51 zero.
 * CRC32 at the last four bytes covers all preceding bytes in each schema.
 * Unknown versions are rejected. Failures leave output untouched;
 * decoding/migration never writes storage or silently replaces bad data. */
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
/* Explicit coordinator sleep request, independent of the saved idle timeout.
 * Rejects held/wake-captured input and clock rollback. On admission, consume
 * all old gesture tokens so delayed callbacks cannot activate after sleep.
 * The next physical BEGIN must wake through the normal whole-gesture gate.
 * This changes desired policy only; callers must first stop protected capture
 * and verify its hardware-off acknowledgement, then observe brightness-off.
 * True means admitted (including already asleep), not a hardware receipt. */
bool device_idle_request_sleep(device_idle_t *policy, uint64_t now_ms);
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
