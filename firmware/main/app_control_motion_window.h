// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"

#define CONTROL_MOTION_WINDOW_SIZE 2U
typedef struct {
    uint64_t key;
    bool relative, ready;
} control_motion_slot_t;
typedef struct {
    control_motion_slot_t slots[CONTROL_MOTION_WINDOW_SIZE];
} control_motion_window_t;

/* The transport owner serializes add/remove. Completion publication may mark
 * either key ready under the listener lock. No terminal holds a command. */
static inline bool control_motion_prepare(const control_motion_window_t *window,
                                          cp_motion_terminal_t *reply)
{
    unsigned count = 0;
    bool only_relative = true;
    for (unsigned i = 0; i < CONTROL_MOTION_WINDOW_SIZE; ++i) {
        if (window->slots[i].key) {
            ++count;
            only_relative = only_relative && window->slots[i].relative;
        }
    }
    reply->motion_only = count != 0;
    return count == 0 || (count == 1 && only_relative && reply->mode == CP_TERMINAL_MOVE_WINDOW);
}

static inline bool control_motion_track(control_motion_window_t *window, const cp_motion_terminal_t *reply)
{
    if (!reply->accepted_key) return true;
    for (unsigned i = 0; i < CONTROL_MOTION_WINDOW_SIZE; ++i) {
        if (!window->slots[i].key) {
            window->slots[i] = (control_motion_slot_t){.key = reply->accepted_key,
                .relative = reply->accepted_relative};
            return true;
        }
    }
    return false;
}

static inline control_motion_slot_t *control_motion_find(control_motion_window_t *window, uint64_t key)
{
    for (unsigned i = 0; i < CONTROL_MOTION_WINDOW_SIZE; ++i) {
        if (key && window->slots[i].key == key) return &window->slots[i];
    }
    return NULL;
}
