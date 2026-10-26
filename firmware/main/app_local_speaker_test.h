// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>

/* Diagnostic-image boot owner only, before the normal media manager exists.
 * One finite run per boot. True permits handover after confirmed shutdown or
 * a skip before acquiring the codec. No acoustic outcome is inferred. */
bool app_local_speaker_test_run(bool physically_locked);
/* Nonblocking physical-button/lock cancellation; latched even before run. */
void app_local_speaker_test_cancel(void);
