// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"

#define APP_RUNTIME_DIAGNOSTICS_RESOURCE "/api/v1/diagnostics/runtime"

/* Authenticated transport owns scope checks. Copies cached owner observations
 * only; never initializes, services or commands a producer. The boot identity
 * is the telemetry boot, independent from HTTP session/server lifetimes.
 * One serialized PSRAM workspace; contention returns BUSY. Output <=4096 bytes,
 * no private peer/network names, addresses, credentials, job identities or PCM.
 * Component snapshots are individually copied, not a synchronized hardware
 * measurement. Uninitialized health is null; errors retain actual knownness. */
cp_error_t app_runtime_diagnostics_read(char *json, size_t capacity, size_t *length);
