// SPDX-License-Identifier: Apache-2.0
#include "app_mouse_phase.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#define SLOT_US 2000000U
#define SLOT_COUNT 10U
#define WINDOW_US (SLOT_US * SLOT_COUNT)

typedef struct {
    uint32_t max_us[MOUSE_PHASE_COUNT];
    uint16_t events[MOUSE_EVENT_COUNT];
    uint16_t turns, states;
} mouse_phase_slot_t;

/* All access belongs to the coordinator. Events use the most recent phase
 * boundary, and each duration belongs to the interval in which it started. */
static EXT_RAM_BSS_ATTR struct {
    mouse_phase_slot_t slots[SLOT_COUNT];
    uint64_t start_us, boundary_us, observed_end_us;
    unsigned next_report;
    bool started, active;
} s_phase;

bool app_mouse_phase_active(void) { return s_phase.active; }

void app_mouse_phase_start(void)
{
    if (s_phase.started) return;
    s_phase.started = s_phase.active = true;
    s_phase.start_us = s_phase.boundary_us = (uint64_t)esp_timer_get_time();
}

static mouse_phase_slot_t *current_slot(void)
{
    uint32_t elapsed_us = (uint32_t)(s_phase.boundary_us - s_phase.start_us);
    return &s_phase.slots[elapsed_us / SLOT_US];
}

void app_mouse_phase_mark(app_mouse_phase_t phase, uint64_t now_us)
{
    if (!s_phase.active || now_us < s_phase.boundary_us) return;
    const uint64_t end_us = s_phase.start_us + WINDOW_US;
    const uint64_t stop_us = now_us < end_us ? now_us : end_us;
    mouse_phase_slot_t *slot = current_slot();
    uint32_t duration = (uint32_t)(stop_us - s_phase.boundary_us);
    if (duration > slot->max_us[phase]) slot->max_us[phase] = duration;
    s_phase.boundary_us = stop_us;
    if (now_us >= end_us) {
        s_phase.observed_end_us = now_us;
        s_phase.active = false;
    } else if (phase == MOUSE_PHASE_PAUSE) {
        uint16_t *turns = &current_slot()->turns;
        if (*turns < UINT16_MAX) ++*turns;
    }
}

void app_mouse_phase_event(app_mouse_event_t event)
{
    if (!s_phase.active) return;
    uint16_t *count = &current_slot()->events[event];
    if (*count < UINT16_MAX) ++*count;
}

void app_mouse_phase_state(unsigned states)
{
    if (s_phase.active) current_slot()->states |= (uint16_t)states;
}

void app_mouse_phase_report(void)
{
    if (!s_phase.started || s_phase.active || s_phase.next_report > SLOT_COUNT) return;
    unsigned index = s_phase.next_report++;
    if (!index) {
        ESP_LOGI("MousePhase", "start_us=%llu end_us=%llu observed_end_us=%llu interval_us=%u "
                 "trigger=local_audio_to_mouse state_bits=not_ready/congested/release/lock/remote/"
                 "overlay/idle/multi/not_mouse/switching/usb/no_gesture",
                 (unsigned long long)s_phase.start_us,
                 (unsigned long long)(s_phase.start_us + WINDOW_US),
                 (unsigned long long)s_phase.observed_end_us, SLOT_US);
        return;
    }
    const mouse_phase_slot_t *s = &s_phase.slots[index - 1];
    const uint32_t *m = s->max_us;
    const uint16_t *e = s->events;
    ESP_LOGI("MousePhase", "slot=%u turns=%u states=0x%x max_us[pause/link/fifo/input/hid/services/media/view/health]="
             "%u/%u/%u/%u/%u/%u/%u/%u/%u contacts=%u pad[begin/move/end/cancel]=%u/%u/%u/%u "
             "blocked[route/stale/gate/live/gesture]=%u/%u/%u/%u/%u multi_cancel=%u enqueue[ok/fail]=%u/%u "
             "send[ok/fail]=%u/%u cancel[link/expiry/priority/reset/input]=%u/%u/%u/%u/%u",
             index - 1, (unsigned)s->turns, (unsigned)s->states,
             (unsigned)m[MOUSE_PHASE_PAUSE], (unsigned)m[MOUSE_PHASE_LINK], (unsigned)m[MOUSE_PHASE_FIFO],
             (unsigned)m[MOUSE_PHASE_INPUT], (unsigned)m[MOUSE_PHASE_HID], (unsigned)m[MOUSE_PHASE_SERVICES],
             (unsigned)m[MOUSE_PHASE_MEDIA], (unsigned)m[MOUSE_PHASE_VIEW], (unsigned)m[MOUSE_PHASE_HEALTH],
             (unsigned)e[MOUSE_EVENT_CONTACT], (unsigned)e[MOUSE_EVENT_BEGIN], (unsigned)e[MOUSE_EVENT_MOVE],
             (unsigned)e[MOUSE_EVENT_END], (unsigned)e[MOUSE_EVENT_CANCEL], (unsigned)e[MOUSE_EVENT_ROUTE],
             (unsigned)e[MOUSE_EVENT_STALE], (unsigned)e[MOUSE_EVENT_GATE], (unsigned)e[MOUSE_EVENT_LIVE],
             (unsigned)e[MOUSE_EVENT_GESTURE],
             (unsigned)e[MOUSE_EVENT_MULTI], (unsigned)e[MOUSE_EVENT_ENQUEUE], (unsigned)e[MOUSE_EVENT_REJECT],
             (unsigned)e[MOUSE_EVENT_SEND_OK], (unsigned)e[MOUSE_EVENT_SEND_FAIL], (unsigned)e[MOUSE_EVENT_LINK],
             (unsigned)e[MOUSE_EVENT_EXPIRY], (unsigned)e[MOUSE_EVENT_PRIORITY], (unsigned)e[MOUSE_EVENT_RESET],
             (unsigned)e[MOUSE_EVENT_CANCEL_INPUT]);
}
