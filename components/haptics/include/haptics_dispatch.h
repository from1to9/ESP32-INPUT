// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "haptics.h"

typedef void (*haptics_dispatch_lock_t)(void *context);

/* The adapter supplies one shared lock for both control mutation and the
 * complete bounded driver write. No allocation, delay, fade or reentrant
 * control callback may occur inside that driver callback. */
typedef struct {
    haptics_dispatch_lock_t enter;
    haptics_dispatch_lock_t leave;
    void *lock_context;
    haptics_write_t write;
    void *write_context;
    uint64_t generation;
    bool enabled;
    bool exhausted;
} haptics_dispatch_t;

bool haptics_dispatch_init(haptics_dispatch_t *dispatch,
    haptics_dispatch_lock_t enter, haptics_dispatch_lock_t leave,
    void *lock_context, haptics_write_t write, void *write_context);
/* Caller must hold the same lock passed to init. Returns whether state changed.
 * Generation exhaustion fails closed until reinitialization. */
bool haptics_dispatch_enable_locked(haptics_dispatch_t *dispatch, bool enabled);
/* Acquires the shared lock, checks the control generation, and keeps the lock
 * through the actual driver call. Off writes always pass the control gate.
 * A stale nonzero write is CANCELLED, never a fabricated hardware acceptance. */
haptics_write_result_t haptics_dispatch_write(haptics_dispatch_t *dispatch,
    uint64_t generation, uint8_t strength_percent);
