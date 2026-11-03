// SPDX-License-Identifier: Apache-2.0
/* Reuse SDK boundary callbacks; the composition links the production transport. */
#define main bt_fixture_main
#include "test_bt_audio.c"
#undef main
void test_transport_setup(void)
{
    assert(bt_audio_init() == ESP_OK); initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC); bt_audio_service();
}
void test_transport_foreign_event(void) { remove_result(other); bt_audio_service(); }
void test_transport_replace_voice(void)
{
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
}
bool test_transport_microphone_frame(void)
{
    uint8_t pcm[320]; return hf_tx(pcm, sizeof(pcm)) == sizeof(pcm);
}
ae_result_t test_transport_playback_frame(ae_frame_t *frame)
{
    uint8_t pcm[320] = {42}; hf_rx(pcm, sizeof(pcm)); return bt_audio_receive(frame);
}
void test_transport_teardown(void) { free(allocation); }
