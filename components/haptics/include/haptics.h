// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HAPTICS_CONFIRM,
    HAPTICS_MODE,
    HAPTICS_LOCK,
    HAPTICS_ERROR,
} haptics_event_t;

typedef enum {
    HAPTICS_OK,
    HAPTICS_SUPPRESSED,
    HAPTICS_DISABLED,
    HAPTICS_CANCELLED,
    HAPTICS_UNAVAILABLE,
    HAPTICS_INVALID_EVENT,
    HAPTICS_HARDWARE_ERROR,
    HAPTICS_TIME_ERROR,
} haptics_result_t;

typedef enum {
    HAPTICS_WRITE_OK,
    HAPTICS_WRITE_CANCELLED,
    HAPTICS_WRITE_FAILED,
} haptics_write_result_t;

/* OK means the driver accepted strength [0,100], where zero stops output.
 * CANCELLED rejects a nonzero write before hardware access; zero must never be
 * cancelled. The callback must be bounded, must not delay for the pulse and
 * must not call back into the policy. One owner serializes all policy calls. */
typedef haptics_write_result_t (*haptics_write_t)(void *context, uint8_t strength_percent);

typedef struct {
    haptics_write_t write;
    void *context;
    uint64_t last_ms;
    uint64_t started_ms;
    uint64_t last_write_ms;
    uint64_t next_allowed_ms;
    uint64_t last_stop_ms;
    haptics_event_t event;
    haptics_result_t fault;
    uint8_t strength;
    bool supported;
    bool available;
    bool enabled;
    bool playing;
    bool error_gap_seen;
    bool off_pending;
} haptics_t;

/* All times are monotonic uint64_t milliseconds. Initialization proves motor
 * off before enabling admission. Unsupported hardware never calls write. */
haptics_result_t haptics_init(haptics_t *state, bool supported,
    haptics_write_t write, void *context, uint64_t now_ms);
haptics_result_t haptics_request(haptics_t *state, haptics_event_t event,
    uint64_t now_ms);
/* Call at least every 10 ms while operating. Late polls skip expired phases;
 * they never replay or extend a pulse. No pending event queue is maintained. */
haptics_result_t haptics_poll(haptics_t *state, uint64_t now_ms);
/* Disable cancels all phases and writes off before returning. Enable does not
 * clear faults, shorten cooldowns or restore cancelled events. */
haptics_result_t haptics_set_enabled(haptics_t *state, bool enabled,
    uint64_t now_ms);
/* Explicit recovery proves off and resets the clock anchor, leaving admission
 * disabled. The caller must first repair/reinitialize its hardware driver. */
haptics_result_t haptics_recover(haptics_t *state, uint64_t now_ms);
bool haptics_available(const haptics_t *state);
bool haptics_enabled(const haptics_t *state);
/* True for accepted nonzero output or an unconfirmed stop, for motion gates. */
bool haptics_active(const haptics_t *state);
