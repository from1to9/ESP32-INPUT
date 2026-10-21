// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "spoken_letters.h"

/* One private worker owns AFE/MultiNet. Init starts asynchronous model loading;
 * AE_OK means worker admission, never model availability. Read the snapshot.
 * Each start requires a new actual local recognition job and consumer binding;
 * a token alone cannot reopen old buffered audio from a previous window.
 * It neither opens the codec nor creates microphone authority. */
ae_result_t app_spoken_letters_init(void);
ae_result_t app_spoken_letters_begin(const sl_binding_t *);
/* Copy-only capture sink; kind must already be AE_CAPTURE_RECOGNITION at its
 * manager-owned caller. AE_AGAIN before begin/after terminal state retains
 * data only in the manager's bounded ring. Stop the capture job on a terminal
 * speech snapshot. Queue overflow stops ingress and is visible as a fault. */
ae_result_t app_spoken_letters_capture(const ae_id_t *, const ae_frame_t *);
/* Audio manager capture_cancel uses the exact job and old consumer generation.
 * UI/context cancellation passes the active binding; old cancellation cannot
 * cancel a replacement. Ingress closes synchronously before this returns. */
void app_spoken_letters_cancel(const ae_id_t *, uint64_t consumer_generation);
/* Manager capture_cancel callback: close PCM and drain queued frames. Preserve
 * only an already published, unexpired candidate for this exact job/consumer.
 * Closure during recognition cancels that window; it cannot publish later.
 * Parent privacy/lock/mode/context handling must additionally use strong cancel. */
void app_spoken_letters_capture_closed(const ae_id_t *, uint64_t consumer_generation);
bool app_spoken_letters_snapshot(sl_view_t *);
/* Trusted local confirmation only, after parent checks current mode/ownership
 * and preview capacity. Returns one ASCII character; never sends HID. */
ae_result_t app_spoken_letters_confirm(uint64_t token, uint64_t epoch,
    uint64_t ui_revision, char *candidate);
