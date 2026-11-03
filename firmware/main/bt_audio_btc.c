// SPDX-License-Identifier: Apache-2.0
/* Compiled in the pinned bt component so the message and its argument use
 * the same private SDK types as the public audio APIs and BTC handlers. */
#include "bt_audio_btc.h"
#include <stddef.h>
#include <string.h>
#include "esp_bt_main.h"
#include "esp_hf_client_api.h"
#include "btc_av.h"
#include "btc_hf_client.h"

esp_err_t bt_audio_btc_post(void (*process)(void *))
{
    osi_thread_t *owner = btc_get_current_thread();
    if (!owner) return ESP_ERR_INVALID_STATE;
    return osi_thread_post(owner, process, NULL, 0, 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_audio_btc_execute(bt_audio_command_t command, const uint8_t peer[6])
{
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) return ESP_ERR_INVALID_STATE;
    /* btc_inter_profile_call dispatches inline on BTC and borrows the frame
     * for this call. Each of the six SDK actions takes one address by value. */
    typedef struct {
        btc_msg_t message;
        union { btc_av_args_t av; btc_hf_client_args_t hf; } args;
    } control_frame_t;
    _Static_assert(offsetof(control_frame_t, args) == offsetof(btc_msg_t, arg), "BTC argument layout");
    control_frame_t frame = {.message.sig = BTC_SIG_API_CALL};
    bt_bdaddr_t *address;
    frame.message.pid = BTC_PID_HF_CLIENT;
    switch (command) {
    case BT_AUDIO_CONNECT_HFP:
        frame.message.act = BTC_HF_CLIENT_CONNECT_EVT; address = &frame.args.hf.connect; break;
    case BT_AUDIO_DISCONNECT_HFP:
        frame.message.act = BTC_HF_CLIENT_DISCONNECT_EVT; address = &frame.args.hf.disconnect; break;
    case BT_AUDIO_OPEN_VOICE:
        frame.message.act = BTC_HF_CLIENT_CONNECT_AUDIO_EVT; address = &frame.args.hf.connect_audio; break;
    case BT_AUDIO_CLOSE_VOICE:
        frame.message.act = BTC_HF_CLIENT_DISCONNECT_AUDIO_EVT; address = &frame.args.hf.disconnect_audio; break;
    case BT_AUDIO_CONNECT_A2DP: case BT_AUDIO_DISCONNECT_A2DP:
        if (g_a2dp_on_deinit || g_a2dp_sink_ongoing_deinit) return ESP_ERR_INVALID_STATE;
        frame.message.pid = BTC_PID_A2DP;
        frame.message.act = command == BT_AUDIO_CONNECT_A2DP ? BTC_AV_SINK_API_CONNECT_EVT : BTC_AV_SINK_API_DISCONNECT_EVT;
        address = command == BT_AUDIO_CONNECT_A2DP ? &frame.args.av.connect : &frame.args.av.disconn;
        break;
    default: return ESP_ERR_INVALID_ARG;
    }
    memcpy(address, peer, sizeof(*address));
    return btc_inter_profile_call(&frame.message) == BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
}
