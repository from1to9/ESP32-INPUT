// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BT_AUDIO_CONNECT_HFP, BT_AUDIO_CONNECT_A2DP,
    BT_AUDIO_DISCONNECT_HFP, BT_AUDIO_DISCONNECT_A2DP,
    BT_AUDIO_OPEN_VOICE, BT_AUDIO_CLOSE_VOICE, BT_AUDIO_FORGET_CLASSIC
} bt_audio_command_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Post to the BTC queue that owns Classic profile events. Only the posted
 * function may invoke the synchronous profile handler below. */
esp_err_t bt_audio_btc_post(void (*process)(void *));
esp_err_t bt_audio_btc_execute(bt_audio_command_t command, const uint8_t peer[6]);

#ifdef __cplusplus
}
#endif
