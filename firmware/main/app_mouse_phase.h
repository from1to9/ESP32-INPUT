// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_APP_MOUSE_PHASE_DIAGNOSTICS) && CONFIG_APP_MOUSE_PHASE_DIAGNOSTICS
#define APP_MOUSE_PHASE_ENABLED 1

typedef enum {
    MOUSE_PHASE_PAUSE, MOUSE_PHASE_LINK, MOUSE_PHASE_FIFO, MOUSE_PHASE_INPUT,
    MOUSE_PHASE_HID, MOUSE_PHASE_SERVICES, MOUSE_PHASE_MEDIA, MOUSE_PHASE_VIEW,
    MOUSE_PHASE_HEALTH, MOUSE_PHASE_COUNT
} app_mouse_phase_t;

typedef enum {
    MOUSE_EVENT_CONTACT, MOUSE_EVENT_BEGIN, MOUSE_EVENT_MOVE, MOUSE_EVENT_END,
    MOUSE_EVENT_CANCEL, MOUSE_EVENT_ROUTE, MOUSE_EVENT_STALE, MOUSE_EVENT_GATE, MOUSE_EVENT_LIVE,
    MOUSE_EVENT_GESTURE, MOUSE_EVENT_MULTI, MOUSE_EVENT_ENQUEUE, MOUSE_EVENT_REJECT,
    MOUSE_EVENT_SEND_OK, MOUSE_EVENT_SEND_FAIL, MOUSE_EVENT_LINK, MOUSE_EVENT_EXPIRY,
    MOUSE_EVENT_PRIORITY, MOUSE_EVENT_RESET, MOUSE_EVENT_CANCEL_INPUT, MOUSE_EVENT_COUNT
} app_mouse_event_t;

enum {
    MOUSE_STATE_NOT_READY = 1U << 0, MOUSE_STATE_CONGESTED = 1U << 1,
    MOUSE_STATE_RELEASING = 1U << 2, MOUSE_STATE_LOCKED = 1U << 3,
    MOUSE_STATE_REMOTE = 1U << 4, MOUSE_STATE_OVERLAY = 1U << 5,
    MOUSE_STATE_IDLE = 1U << 6, MOUSE_STATE_MULTI = 1U << 7,
    MOUSE_STATE_NOT_MOUSE = 1U << 8, MOUSE_STATE_SWITCHING = 1U << 9,
    MOUSE_STATE_USB = 1U << 10, MOUSE_STATE_NO_GESTURE = 1U << 11
};

bool app_mouse_phase_active(void);
void app_mouse_phase_start(void);
void app_mouse_phase_mark(app_mouse_phase_t phase, uint64_t now_us);
void app_mouse_phase_event(app_mouse_event_t event);
void app_mouse_phase_state(unsigned states);
void app_mouse_phase_report(void);
#else
#define APP_MOUSE_PHASE_ENABLED 0
#define app_mouse_phase_active() false
#define app_mouse_phase_start() ((void)0)
#define app_mouse_phase_mark(...) ((void)0)
#define app_mouse_phase_event(...) ((void)0)
#define app_mouse_phase_state(...) ((void)0)
#define app_mouse_phase_report() ((void)0)
#endif
