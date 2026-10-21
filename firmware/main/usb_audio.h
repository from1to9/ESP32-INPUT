// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_io.h"
#include "esp_err.h"

typedef struct {
    bool initialized, stack_ready, mounted, suspended;
    bool playback_selected, capture_selected, prepared;
    bool cleanup_pending;
    uint64_t link_generation, audio_generation;
    uint64_t dropped_playback_frames, rejected_capture_frames;
    esp_err_t error;
} usb_audio_snapshot_t;

/* One UAC2 function in the existing CDC/HID TinyUSB stack. Initialization
 * allocates bounded PSRAM queues and one worker; it never opens the codec.
 * USB alternate-interface selection only reports a host request. An explicit
 * manager preparation plus local capture consent is still required. */
esp_err_t usb_audio_init(void);
void usb_audio_stack_started(esp_err_t result);
void usb_audio_stack_event(bool mounted, bool suspended);
ae_route_port_t usb_audio_route_port(void);
/* Manager polls copied, complete mono PCM16 48 kHz/10 ms playback frames.
 * No capture activation, codec calls or host-identity inference occurs here. */
ae_result_t usb_audio_receive(ae_frame_t *frame);
usb_audio_snapshot_t usb_audio_snapshot(void);
