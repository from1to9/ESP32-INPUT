// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_media_ui.h"

/* Copy only public admission fields from the coordinator's existing presenter
 * candidate. This is not a display acknowledgement or another readiness read. */
void app_audio_diagnostics_media_view(uint64_t observed_ms, const app_media_view_t *view);

/* Copied from the coordinator's existing permission observation. This record
 * never grants capture permission or reads the display a second time. */
typedef struct {
    bool unlocked, privacy_latched, maintenance, screen_visible;
    bool bound, armed, transfer_confirmed, owner_alive, clock_valid, usable;
    uint64_t display_generation, capture_generation, indicator_revision;
    uint64_t heartbeat_age_ms, completion_age_ms;
    int display_error;
} app_audio_capture_context_t;
void app_audio_diagnostics_capture_context(uint64_t observed_ms,
    const app_audio_capture_context_t *context);

/* Serialized coordinator observer, after PSRAM initialization. Reads cached
 * owner snapshots at most every 100 ms; never services a producer or changes
 * audio/HID policy. Logs semantic/error transitions and 10-second summaries.
 * Private copied metadata is wiped before return. No IDs, peers, PCM or
 * recognition candidates are retained or logged. Sampling may miss shorter
 * transitions; this is not a physical timing or sound qualification. */
void app_audio_diagnostics_poll(uint64_t now_ms);
