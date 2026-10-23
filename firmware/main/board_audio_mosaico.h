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
