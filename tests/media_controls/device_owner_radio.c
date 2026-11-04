// SPDX-License-Identifier: Apache-2.0
#define main radio_fixture_main
#define esp_timer_get_time fixture_bt_time
#include "../bt_audio/test_bt_audio.c"
#undef main

void device_radio_incoming_music(bool pcm)
{
    music_format(ESP_A2D_SBC_CIE_SF_44K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 3);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    if (pcm) {
        uint8_t decoded[4096] = {0};
        clock_us = 1250000;
        a2_rx(decoded, sizeof(decoded));
    }
}

void device_radio_init(void)
{
    bond_count = shared_count = 2;
    assert(bt_audio_init() == ESP_OK);
    initialized(); bt_audio_service(); defer_btc = true;
}
void device_radio_dispatch(void) { if (queued_btc) run_btc(); }
unsigned device_radio_calls(void) { return dispatch_calls; }
void device_radio_before_lock(void (*callback)(void)) { before_lock = callback; }
void device_radio_hfp(bool foreign)
{
    esp_hf_client_cb_param_t event = {.conn_stat.state = ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED};
    memcpy(event.conn_stat.remote_bda, foreign ? other : peer, 6);
    hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, &event);
}
void device_radio_speaker(void) { music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3); }
void device_radio_voice(void) { voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC); }
void device_radio_auth(bool success) { auth_result(peer, success); }
void device_radio_catalogue(esp_err_t error, bool empty)
{
    bond_list_error = error;
    bond_count = shared_count = empty ? 0 : 2;
}
unsigned device_radio_discoveries(void) { return discoverable_writes; }
