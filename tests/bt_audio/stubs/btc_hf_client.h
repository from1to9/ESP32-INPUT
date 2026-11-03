// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "btc_av.h"
typedef union { bt_bdaddr_t connect, disconnect, connect_audio, disconnect_audio; } btc_hf_client_args_t;
enum { BTC_HF_CLIENT_CONNECT_EVT, BTC_HF_CLIENT_DISCONNECT_EVT,
       BTC_HF_CLIENT_CONNECT_AUDIO_EVT, BTC_HF_CLIENT_DISCONNECT_AUDIO_EVT };
