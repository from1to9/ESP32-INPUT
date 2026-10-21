// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool failed;
    bool mounted;
    bool suspended;
    bool enabled;
    bool ready;
    bool congested;
    uint8_t keyboard_leds;
    esp_err_t error;
    uint64_t generation;
    uint64_t accepted;
    uint64_t submitted;
    uint64_t completed;
} usb_hid_snapshot_t;

/* Idempotent shared USB manager start: one composite CDC + keyboard/mouse
 * TinyUSB installation. usb_console_start() starts the same owner. Input is
 * disabled initially. This implements HID input, not USB audio/UAC. */
esp_err_t usb_hid_init(void);
usb_hid_snapshot_t usb_hid_snapshot(void);

/* Single coordinator transport selection. Release the old transport and wait
 * for its completion before disabling it. Disabling cancels unsubmitted work.
 * Disabling during dispatch/in-flight output quarantines HID and latches
 * failed, because a submitted report cannot safely be recalled. CDC remains
 * installed. A real USB reset/detach is required to clear uncertain endpoint
 * ownership; re-enabling never replays a report. */
void usb_hid_set_enabled(bool enabled);

/* Nonblocking task-context admission, exactly one slot including in-flight
 * work. ID 1 takes 8 keyboard bytes; ID 2 takes 4 mouse bytes, WITHOUT an ID
 * prefix. true means copied into the USB adapter, not USB/host completion.
 * The USB task rechecks session/lifecycle before endpoint submission. False
 * means no admission. Congestion includes the occupied slot; pace the engine
 * from snapshots and cancel work after any generation change. */
bool usb_hid_send_generation(uint64_t generation, uint8_t report_id,
    const uint8_t *data, size_t length);

/* Coordinator recovery after release exhaustion: invalidate generation,
 * cancel unsubmitted input and quarantine HID until a real USB reset/detach.
 * CDC remains available. This is not proof of release/disconnect at the host.
 * Obsolete generations are rejected. No host remote wakeup is issued. */
bool usb_hid_recover_generation(uint64_t generation);

/* Shared-manager hooks; not application transport APIs. */
esp_err_t usb_hid_prepare(void);
void usb_hid_stack_started(esp_err_t error);
void usb_hid_stack_event(bool mounted, bool suspended);

#ifdef __cplusplus
}
#endif
