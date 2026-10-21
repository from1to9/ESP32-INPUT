// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { INPUT_MOUSE, INPUT_KEYBOARD, INPUT_AUDIO } input_mode_t;
typedef enum { INPUT_LOCAL, INPUT_REMOTE } input_owner_t;

/* Single coordinator task owns this struct; adapters must not mutate fields. */
typedef struct {
    input_mode_t mode;
    input_owner_t owner;
    uint64_t epoch;
    bool connected, locked, gyro_enabled, release_pending;
} input_state_t;

void input_init(input_state_t *s);
bool input_set_mode(input_state_t *s, input_mode_t mode);
void input_set_connected(input_state_t *s, bool connected);
void input_set_locked(input_state_t *s, bool locked);
void input_set_owner(input_state_t *s, input_owner_t owner);
/* Also call when replacing a target without an intervening disconnect. */
void input_invalidate(input_state_t *s);
bool input_set_gyro(input_state_t *s, bool enabled);
bool input_set_gyro_owned(input_state_t *s, input_owner_t source, bool enabled);
/* ACK only after transport has accepted zero mouse AND keyboard reports. */
bool input_ack_release(input_state_t *s, uint64_t epoch);
bool input_can_emit(const input_state_t *s, uint64_t epoch,
                    input_owner_t source, input_mode_t kind);

typedef struct { float x, y; } input_motion_t;
typedef struct { int8_t x, y; } input_delta_t;
void input_motion_reset(input_motion_t *motion);
/* Already calibrated and axis-mapped dps; gain is HID units per degree.
 * allowed includes touch/menu/haptic freeze and coordinator gates.
 * Reject intervals over 50 ms, drop overflow rather than replay a backlog. */
input_delta_t input_motion_step(input_motion_t *motion, float x_dps,
    float y_dps, float dt_seconds, float gain, float deadzone_dps, bool allowed);

/* Standard 8-byte boot keyboard payload, excluding transport Report ID. */
typedef struct { uint8_t modifier, reserved, keys[6]; } input_key_report_t;
/* Printable US ANSI ASCII only. Newline/Enter is never implicit text. */
bool input_ascii_report(unsigned char character, input_key_report_t *report);
