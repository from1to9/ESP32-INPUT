// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "audio_io.h"

/* Call init once at boot, then obtain the singleton port for the audio manager.
 * Every port call is a nonblocking request/poll: AE_AGAIN means retry the same
 * operation/arguments. One private worker owns the ES8311 and I2S0. Callers must
 * serialize port requests; safety mute/stop can preempt an unfinished request.
 * No SDK pointer, retained caller buffer or callback enters the worker.
 *
 * Capture is mono PCM16LE from the left I2S slot. Stereo playback is explicitly
 * downmixed to the board's mono speaker. No resampling is performed. Disabling
 * capture stops RX DMA and discards buffered samples; this does not claim that
 * the analog ADC is powered off while the speaker remains active. Qualification
 * of the physical microphone/speaker, levels and simultaneous IO is outstanding.
 */
typedef struct {
    bool worker_started;
    bool probe_complete;
    ae_result_t probe_result;
    bool hardware_observed;
    bool prepared;
    bool capture_running;
    bool playback_running;
    bool speaker_muted;
    bool cleanup_uncertain;
    uint64_t generation;
    ae_format_t format;
    uint8_t roles;
    ae_result_t last_result;
    int last_sdk_error;
} board_audio_status_t;

ae_result_t board_audio_mosaico_init(void);
ae_codec_port_t board_audio_mosaico_port(void);
board_audio_status_t board_audio_mosaico_status(void);
