// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Wire values for the optional input_prefs/resume_v1 key. Byte fields only.
 * No bond keys, input state, live tokens or capture permissions belong here. */
enum { APP_RESUME_HFP = 1, APP_RESUME_A2DP = 2 };
typedef struct {
    uint8_t route;        /* 0 default, 1 local, 2 HFP, 3 A2DP, 4 USB. */
    uint8_t peer[6];      /* Selected Classic address; never a BLE destination. */
    uint8_t services;     /* Successfully connected bonded services to reconnect. */
    uint8_t disconnected; /* Explicitly disconnected services require a new local choice. */
    uint8_t speaker;      /* Actual Classic playback route, zero when not armed. */
} app_boot_audio_t;
typedef struct {
    uint8_t mode;         /* 0 Mouse, 1 Keyboard, 2 Audio. */
    app_boot_audio_t audio;
} app_boot_choices_t;
