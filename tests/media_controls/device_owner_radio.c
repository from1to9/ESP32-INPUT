// SPDX-License-Identifier: Apache-2.0
#define main radio_fixture_main
#define esp_timer_get_time fixture_bt_time
#include "../bt_audio/test_bt_audio.c"
#undef main

void device_radio_init(void)
{
    bond_count = shared_count = 2;
    assert(bt_audio_init() == ESP_OK);
    initialized(); bt_audio_service(); defer_btc = true;
}
void device_radio_dispatch(void) { if (queued_btc) run_btc(); }
unsigned device_radio_calls(void) { return dispatch_calls; }
void device_radio_hfp(bool foreign)
{
    esp_hf_client_cb_param_t event = {.conn_stat.state = ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED};
    memcpy(event.conn_stat.remote_bda, foreign ? other : peer, 6);
    hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, &event);
}
void device_radio_speaker(void) { music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3); }
void device_radio_voice(void) { voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC); }
