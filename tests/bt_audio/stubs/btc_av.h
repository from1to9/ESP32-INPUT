// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint8_t sig, aid, pid, act; __extension__ uint8_t arg[0]; } btc_msg_t;
typedef struct { uint8_t address[6]; } bt_bdaddr_t;
typedef union { bt_bdaddr_t connect, disconn; } btc_av_args_t;
enum { BTC_SIG_API_CALL, BTC_PID_A2DP, BTC_PID_HF_CLIENT };
enum { BTC_AV_SINK_API_CONNECT_EVT, BTC_AV_SINK_API_DISCONNECT_EVT };
extern bool g_a2dp_on_deinit, g_a2dp_sink_ongoing_deinit;
typedef struct osi_thread osi_thread_t;
#define OSI_THREAD_MAX_TIMEOUT UINT32_MAX
#define BT_STATUS_SUCCESS 0
osi_thread_t *btc_get_current_thread(void);
bool osi_thread_post(osi_thread_t *owner, void (*run)(void *), void *context, int queue, uint32_t timeout);
int btc_inter_profile_call(btc_msg_t *message);
