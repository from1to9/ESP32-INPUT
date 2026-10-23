// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Serialized coordinator observer, after PSRAM initialization. Reads cached
 * owner snapshots at most every 100 ms; never services a producer or changes
 * audio/HID policy. Logs semantic/error transitions and 10-second summaries.
 * Private copied metadata is wiped before return. No IDs, peers, PCM or
 * recognition candidates are retained or logged. Sampling may miss shorter
 * transitions; this is not a physical timing or sound qualification. */
void app_audio_diagnostics_poll(uint64_t now_ms);
