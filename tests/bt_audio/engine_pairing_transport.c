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
void test_transport_teardown(void) { free(allocation); }
