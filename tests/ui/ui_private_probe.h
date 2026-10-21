// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>

/* Observation only, called by native tests outside UI dispatch. */
bool ui_private_password_contains(const char *value);
bool ui_private_buffers_cleared(void);
bool ui_private_password_tail_cleared(void);
bool ui_private_public_state_contains(const char *value);
