// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Shared by the existing TinyUSB component and every consumer. This adds one
 * UAC2 function to the same stack as CDC/HID; managed sources remain untouched.
 * Mono PCM16 at 48 kHz, at most 49 samples per one-millisecond USB transaction.
 * Four milliseconds of stack FIFO per direction; larger application queues
 * belong in PSRAM under the audio manager's allocation policy. */
#define CFG_TUD_AUDIO 1
#define CFG_TUD_AUDIO_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 98
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX 98
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 392
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ 392
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1
