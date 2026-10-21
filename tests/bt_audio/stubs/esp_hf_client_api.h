// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_HF_CLIENT_CONNECTION_STATE_EVT, ESP_HF_CLIENT_AUDIO_STATE_EVT,
    ESP_HF_CLIENT_PROF_STATE_EVT } esp_hf_client_cb_event_t;
typedef enum { ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED, ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING,
    ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED, ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED,
    ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING } esp_hf_client_connection_state_t;
typedef enum { ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED, ESP_HF_CLIENT_AUDIO_STATE_CONNECTING,
    ESP_HF_CLIENT_AUDIO_STATE_CONNECTED, ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC,
    ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_LC3 } esp_hf_client_audio_state_t;
typedef enum { ESP_HF_INIT_SUCCESS, ESP_HF_INIT_ALREADY, ESP_HF_INIT_FAIL,
    ESP_HF_DEINIT_SUCCESS } esp_hf_prof_state_t;
typedef union {
    struct { esp_hf_client_connection_state_t state; uint8_t remote_bda[6]; } conn_stat;
    struct { esp_hf_client_audio_state_t state; uint8_t remote_bda[6]; } audio_stat;
    struct { esp_hf_prof_state_t state; } prof_stat;
} esp_hf_client_cb_param_t;
typedef void (*esp_hf_client_cb_t)(esp_hf_client_cb_event_t, esp_hf_client_cb_param_t *);
esp_err_t esp_hf_client_register_callback(esp_hf_client_cb_t);
esp_err_t esp_hf_client_init(void);
esp_err_t esp_hf_client_register_data_callback(void (*receive)(const uint8_t *, uint32_t),
    uint32_t (*send)(uint8_t *, uint32_t));
esp_err_t esp_hf_client_connect(const uint8_t *);
esp_err_t esp_hf_client_disconnect(const uint8_t *);
esp_err_t esp_hf_client_connect_audio(const uint8_t *);
esp_err_t esp_hf_client_disconnect_audio(const uint8_t *);
void esp_hf_client_outgoing_data_ready(void);
