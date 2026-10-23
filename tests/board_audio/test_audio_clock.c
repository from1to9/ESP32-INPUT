// SPDX-License-Identifier: Apache-2.0
/* Real board adapter + pinned codec; I2S boundary models the actual shared
 * TX-master/RX-slave clock dependency, not an always-producing microphone. */
#define main board_fixture_main
#include "test_board_audio.c"
#undef main

static void mute_role(ae_role_t role, uint64_t generation, ae_result_t expected)
{
    assert(port.mute(NULL, role, true, generation) == AE_AGAIN); drain();
    assert(port.mute(NULL, role, true, generation) == expected);
}

static void read_frame(uint64_t generation)
{
    uint8_t data[960]; size_t actual = 0;
    assert(port.read(NULL, generation, data, sizeof(data), &actual) == AE_AGAIN); drain();
    ae_result_t result = port.read(NULL, generation, data, sizeof(data), &actual);
    if (result != AE_OK || actual != sizeof(data))
        fprintf(stderr, "Cold microphone result=%d bytes=%zu TX_master=%d RX_slave=%d SDK_error=%d\n",
            (int)result, actual, tx_channel.enabled, rx_channel.enabled, board_audio_mosaico_status().last_sdk_error);
    assert(result == AE_OK && actual == sizeof(data));
}

int main(void)
{
    port = board_audio_mosaico_port();
    assert(board_audio_mosaico_init() == AE_OK); drain();
    const ae_format_t usb = {48000, 1, 10, AE_PCM_S16_LE};
    const uint64_t generation = 1;
    open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, generation);
    unmute_role(AE_MICROPHONE, generation);
    read_frame(generation);
    assert(!pa_level && (registers[0x31] & 0x60U) == 0x60U && !s.playback_gate);
    board_audio_status_t status = board_audio_mosaico_status();
    assert(status.tx_clock_running && status.capture_running && !status.playback_running &&
        status.speaker_muted && status.speaker_mute_confirmed);
    uint8_t pcm[960]; memset(pcm, 0x61, sizeof(pcm)); size_t actual = 0;
    assert(port.write(NULL, generation, pcm, sizeof(pcm), &actual) == AE_PRIVACY && actual == 0);
    unmute_role(AE_SPEAKER, generation);
    assert(port.write(NULL, generation, pcm, sizeof(pcm), &actual) == AE_AGAIN); drain();
    assert(port.write(NULL, generation, pcm, sizeof(pcm), &actual) == AE_OK && actual == sizeof(pcm));
    assert(dma_storage[0] == 0x61 && dma_storage[1] == 0x61);
    mute_role(AE_SPEAKER, generation, AE_OK);
    assert(s.tx_running && s.rx_running && s.capture_gate && !s.playback_gate && !pa_level);
    assert_cleared(dma_storage, sizeof(dma_storage)); read_frame(generation);
    unmute_role(AE_SPEAKER, generation);
    mute_role(AE_MICROPHONE, generation, AE_OK);
    assert(s.tx_running && !s.rx_running && !s.capture_gate && s.playback_gate && pa_level == 1);
    close_audio(generation, AE_OK);
    assert(!board_audio_mosaico_status().tx_clock_running);

    for (unsigned failed = 1; failed <= 2; ++failed) {
        uint64_t next = s.last_generation + 1;
        open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, next);
        fail_enable_channel = failed;
        assert(port.mute(NULL, AE_MICROPHONE, false, next) == AE_AGAIN); drain();
        assert(port.mute(NULL, AE_MICROPHONE, false, next) == AE_TIMEOUT && !s.capture_gate && !s.playback_gate);
        assert(!tx_channel.enabled && !rx_channel.enabled && !pa_level);
        status = board_audio_mosaico_status();
        assert(status.last_sdk_error == ESP_ERR_TIMEOUT && status.last_sdk_operation ==
            (failed == 1 ? BOARD_AUDIO_SDK_I2S_ENABLE_TX : BOARD_AUDIO_SDK_I2S_ENABLE_RX));
        assert(status.first_sdk_error == ESP_ERR_TIMEOUT && status.first_sdk_operation == BOARD_AUDIO_SDK_I2S_ENABLE_TX);
        fail_enable_channel = 0; close_audio(next, AE_OK);
        assert(board_audio_mosaico_status().last_sdk_operation == status.last_sdk_operation);
    }
    for (unsigned failed = 1; failed <= 2; ++failed) {
        uint64_t next = s.last_generation + 1;
        open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, next);
        unmute_role(AE_MICROPHONE, next); unmute_role(AE_SPEAKER, next);
        fail_disable_channel = failed;
        mute_role(failed == 1 ? AE_SPEAKER : AE_MICROPHONE, next, AE_UNCERTAIN);
        assert(board_audio_mosaico_status().cleanup_uncertain && !s.capture_gate && !s.playback_gate);
        fail_disable_channel = 0; close_audio(next, AE_OK);
    }
    uint64_t next = s.last_generation + 1;
    open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, next); unmute_role(AE_MICROPHONE, next); unmute_role(AE_SPEAKER, next);
    fail_enable_channel = 1;
    mute_role(AE_SPEAKER, next, AE_UNCERTAIN); /* Clock restart failed after old DMA purge. */
    assert(!s.capture_gate && !s.playback_gate && !pa_level && !tx_channel.enabled);
    fail_enable_channel = 0; close_audio(next, AE_OK);
    next = s.last_generation + 1; open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, next);
    unsigned old_rx_enables = rx_enables; stop_on_enable = true;
    assert(port.mute(NULL, AE_MICROPHONE, false, next) == AE_AGAIN); drain();
    assert(port.mute(NULL, AE_MICROPHONE, false, next) == AE_STALE);
    assert(rx_enables == old_rx_enables && !s.capture_gate && !s.playback_gate && !s.tx_running && !s.rx_running);
    assert(port.stop(NULL, next) == AE_OK && !s.prepared);
    next = s.last_generation + 1; open_audio(&usb, AE_MICROPHONE | AE_SPEAKER, next);
    assert_cleared(dma_storage, sizeof(dma_storage)); close_audio(next, AE_OK);
    puts("PASS: cold USB microphone needs TX master clocks while DAC/PA and speaker permission stay closed");
    puts("PASS: independent role mute, zero DMA purge, TX/RX enable/disable failure, retained operation and late stop regressions");
    return 0;
}
