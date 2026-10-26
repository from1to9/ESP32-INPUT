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
 * downmixed to the board's mono speaker. No resampling is performed. Stopping
 * speaker output keeps muted TX master clocks when slave RX is active; this
 * never opens the playback permission gate. Old TX DMA is zeroed before clocks
 * resume. Clock generation and acknowledged speaker mute have separate status.
 * Stopping capture stops RX DMA and discards buffered samples; this does not claim that
 * the analog ADC is powered off while the speaker remains active. Qualification
 * of the physical microphone/speaker, levels and simultaneous IO is outstanding.
 */
typedef enum {
    BOARD_AUDIO_SDK_NONE, BOARD_AUDIO_SDK_I2C_READ, BOARD_AUDIO_SDK_I2C_WRITE,
    BOARD_AUDIO_SDK_I2S_ALLOCATE, BOARD_AUDIO_SDK_I2S_INIT_TX, BOARD_AUDIO_SDK_I2S_INIT_RX,
    BOARD_AUDIO_SDK_I2S_ENABLE_TX, BOARD_AUDIO_SDK_I2S_ENABLE_RX,
    BOARD_AUDIO_SDK_I2S_DISABLE_TX, BOARD_AUDIO_SDK_I2S_DISABLE_RX,
    BOARD_AUDIO_SDK_I2S_DELETE_TX, BOARD_AUDIO_SDK_I2S_DELETE_RX,
    BOARD_AUDIO_SDK_I2S_PRELOAD_TX, BOARD_AUDIO_SDK_I2S_READ, BOARD_AUDIO_SDK_I2S_WRITE,
    BOARD_AUDIO_SDK_PA_CONFIG, BOARD_AUDIO_SDK_PA_WRITE, BOARD_AUDIO_SDK_BUS_INIT,
    BOARD_AUDIO_SDK_DEVICE_ADD, BOARD_AUDIO_SDK_DEVICE_REMOVE,
    BOARD_AUDIO_SDK_POWER_VCC, BOARD_AUDIO_SDK_POWER_CODEC
} board_audio_sdk_operation_t;

static inline const char *board_audio_sdk_operation_name(board_audio_sdk_operation_t operation)
{
    switch (operation) {
    case BOARD_AUDIO_SDK_NONE: return "none";
    case BOARD_AUDIO_SDK_I2C_READ: return "i2c_master_transmit_receive";
    case BOARD_AUDIO_SDK_I2C_WRITE: return "i2c_master_transmit";
    case BOARD_AUDIO_SDK_I2S_ALLOCATE: return "i2s_new_channel";
    case BOARD_AUDIO_SDK_I2S_INIT_TX: return "i2s_init_tx";
    case BOARD_AUDIO_SDK_I2S_INIT_RX: return "i2s_init_rx";
    case BOARD_AUDIO_SDK_I2S_ENABLE_TX: return "i2s_enable_tx";
    case BOARD_AUDIO_SDK_I2S_ENABLE_RX: return "i2s_enable_rx";
    case BOARD_AUDIO_SDK_I2S_DISABLE_TX: return "i2s_disable_tx";
    case BOARD_AUDIO_SDK_I2S_DISABLE_RX: return "i2s_disable_rx";
    case BOARD_AUDIO_SDK_I2S_DELETE_TX: return "i2s_delete_tx";
    case BOARD_AUDIO_SDK_I2S_DELETE_RX: return "i2s_delete_rx";
    case BOARD_AUDIO_SDK_I2S_PRELOAD_TX: return "i2s_channel_preload_data";
    case BOARD_AUDIO_SDK_I2S_READ: return "i2s_channel_read";
    case BOARD_AUDIO_SDK_I2S_WRITE: return "i2s_channel_write";
    case BOARD_AUDIO_SDK_PA_CONFIG: return "gpio_config_pa";
    case BOARD_AUDIO_SDK_PA_WRITE: return "gpio_set_level_pa";
    case BOARD_AUDIO_SDK_BUS_INIT: return "bsp_i2c_init";
    case BOARD_AUDIO_SDK_DEVICE_ADD: return "i2c_master_bus_add_device";
    case BOARD_AUDIO_SDK_DEVICE_REMOVE: return "i2c_master_bus_rm_device";
    case BOARD_AUDIO_SDK_POWER_VCC: return "bsp_power_set_vcc_3v3";
    case BOARD_AUDIO_SDK_POWER_CODEC: return "bsp_power_set_codec_3v3";
    default: return "unknown";
    }
}

typedef struct {
    bool worker_started;
    bool probe_complete;
    ae_result_t probe_result;
    bool hardware_observed;
    bool prepared;
    bool capture_running;
    bool playback_running;
    bool speaker_muted;
    bool speaker_mute_confirmed;
    bool tx_clock_running; /* Can supply RX clocks while speaker stays muted. */
    bool cleanup_uncertain;
    uint64_t generation;
    ae_format_t format;
    uint8_t roles;
    ae_result_t last_result;
    int last_sdk_error;
    board_audio_sdk_operation_t last_sdk_operation;
    int first_sdk_error; /* Retained until reboot, including through cleanup. */
    board_audio_sdk_operation_t first_sdk_operation;
} board_audio_status_t;

ae_result_t board_audio_mosaico_init(void);
ae_codec_port_t board_audio_mosaico_port(void);
board_audio_status_t board_audio_mosaico_status(void);

/* Task-only cached observation, separate from the internal control mailbox.
 * Scope resets on an owning prepare attempt, retains through stop. Samples
 * are post-downmix mono time samples (one of the duplicated PCM16 I2S slots).
 * Only the valid reported write prefix contributes, including SDK errors;
 * this is not DMA consumption, job delivery, analog output or audibility. */
typedef struct {
    uint64_t generation;
    uint64_t write_calls, failed_writes, invalid_prefix_writes;
    uint64_t reported_samples, reported_nonzero_samples;
    int last_write_sdk_result;
    uint16_t reported_peak_abs; /* 0..32768; INT16_MIN has magnitude 32768. */
    bool generation_known, write_result_known, last_prefix_valid, pcm_known, saturated;
    /* Requests and actual existing register reads have distinct validity.
     * Matches compares that read with the requested mute/volume write. These
     * historical acknowledgements never cause an extra I2C transaction. */
    bool volume_requested_known, volume_read_known, volume_matches;
    uint8_t volume_requested, volume_register;
    bool mute_read_known, mute_requested, mute_matches;
    uint8_t mute_register;
} board_audio_pcm_snapshot_t;

/* Task context only. Copies under the existing owner lock; no IO or mutation.
 * Before any write/read the corresponding known flag is false, not silence. */
void board_audio_pcm_snapshot(board_audio_pcm_snapshot_t *out);
