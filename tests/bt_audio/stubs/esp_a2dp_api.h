// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_A2D_MCT_SBC 0
#define ESP_A2D_SBC_CIE_SF_16K 8
#define ESP_A2D_SBC_CIE_SF_32K 4
#define ESP_A2D_SBC_CIE_SF_44K 2
#define ESP_A2D_SBC_CIE_SF_48K 1
#define ESP_A2D_SBC_CIE_CH_MODE_MONO 8
#define ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL 4
#define ESP_A2D_SBC_CIE_CH_MODE_STEREO 2
#define ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO 1
typedef struct { uint8_t samp_freq, ch_mode; } esp_a2d_cie_sbc_t;
typedef struct { uint8_t type; union { esp_a2d_cie_sbc_t sbc_info; } cie; } esp_a2d_mcc_t;
typedef enum { ESP_A2D_PROF_STATE_EVT, ESP_A2D_CONNECTION_STATE_EVT,
    ESP_A2D_AUDIO_CFG_EVT, ESP_A2D_AUDIO_STATE_EVT } esp_a2d_cb_event_t;
typedef enum { ESP_A2D_CONNECTION_STATE_DISCONNECTED, ESP_A2D_CONNECTION_STATE_CONNECTING,
    ESP_A2D_CONNECTION_STATE_CONNECTED, ESP_A2D_CONNECTION_STATE_DISCONNECTING } esp_a2d_connection_state_t;
typedef enum { ESP_A2D_AUDIO_STATE_SUSPEND, ESP_A2D_AUDIO_STATE_STARTED } esp_a2d_audio_state_t;
typedef enum { ESP_A2D_DEINIT_SUCCESS, ESP_A2D_INIT_SUCCESS } esp_a2d_init_state_t;
typedef union {
    struct { esp_a2d_init_state_t init_state; } a2d_prof_stat;
    struct { esp_a2d_connection_state_t state; uint8_t remote_bda[6]; uint16_t conn_hdl; } conn_stat;
    struct { uint8_t remote_bda[6]; uint16_t conn_hdl; esp_a2d_mcc_t mcc; } audio_cfg;
    struct { esp_a2d_audio_state_t state; uint8_t remote_bda[6]; uint16_t conn_hdl; } audio_stat;
} esp_a2d_cb_param_t;
typedef void (*esp_a2d_cb_t)(esp_a2d_cb_event_t, esp_a2d_cb_param_t *);
esp_err_t esp_a2d_register_callback(esp_a2d_cb_t);
esp_err_t esp_a2d_sink_register_data_callback(void (*)(const uint8_t *, uint32_t));
esp_err_t esp_a2d_sink_init(void);
esp_err_t esp_a2d_sink_connect(const uint8_t *);
esp_err_t esp_a2d_sink_disconnect(const uint8_t *);
