// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "passport_control.h"
#include "control_registry.h"
// Called after the protocol is initialized, before starting the NimBLE task.
void passport_management_install(void);
// NimBLE owner-only backend; admission and completion are distinct.
cp_error_t passport_ble_remote_begin(const cp_command_t *, uint64_t, cp_result_t *);
bool passport_ble_remote_poll(uint64_t, cp_result_t *);
