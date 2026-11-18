// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"

typedef struct {
    uint64_t key;
    bool relative, ready;
} control_motion_slot_t;
typedef struct {
    control_motion_slot_t slots[CP_MOTION_WINDOW_CONFIG_MAX];
} control_motion_window_t;

/* The transport owner serializes add/remove. The completion owner may mark
 * a matching slot ready under the same listener lock. No slot holds commands. */
static inline unsigned control_motion_limit(const cp_motion_terminal_t *motion)
{
    return motion->mode == CP_TERMINAL_MOVE_WINDOW && motion->width >= 2U &&
        motion->width <= CP_MOTION_WINDOW_CONFIG_MAX ? motion->width : 1U;
}

static inline bool control_motion_prepare(const control_motion_window_t *window,
                                          cp_motion_terminal_t *motion)
{
    unsigned count = 0;
    bool only_relative = true;
    for (unsigned i = 0; i < CP_MOTION_WINDOW_CONFIG_MAX; ++i) {
        if (window->slots[i].key) {
            ++count;
            only_relative = only_relative && window->slots[i].relative;
        }
    }
    motion->motion_only = count != 0;
    return count == 0 || (only_relative && count < control_motion_limit(motion));
}

static inline control_motion_slot_t *control_motion_find(control_motion_window_t *window, uint64_t key)
{
    for (unsigned i = 0; i < CP_MOTION_WINDOW_CONFIG_MAX; ++i) {
        if (key && window->slots[i].key == key) return &window->slots[i];
    }
    return NULL;
}

static inline bool control_motion_track(control_motion_window_t *window, const cp_motion_terminal_t *motion)
{
    if (!motion->accepted_key) return true;
    if (control_motion_find(window, motion->accepted_key) || (motion->motion_only && !motion->accepted_relative))
        return false;
    const unsigned limit = control_motion_limit(motion);
    for (unsigned i = 0; i < limit; ++i) {
        if (!window->slots[i].key) {
            window->slots[i] = (control_motion_slot_t){.key = motion->accepted_key,
                .relative = motion->accepted_relative};
            return true;
        }
    }
    return false;
}
