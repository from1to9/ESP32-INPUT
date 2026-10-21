// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"

/* Bounded cross-task handoff. Network callbacks only copy immutable commands,
 * cached availability and identity-scoped read results. The coordinator alone
 * executes effects. Callbacks must not wait on UI, flash, radios or networks. */
#define APP_COMMAND_CAPACITY 8U
#define APP_COMMAND_SAFETY_CAPACITY CP_MAX_RESULTS
#define APP_COMMAND_READS 4U
#define APP_COMMAND_RESOURCE_BYTES 160U
#define APP_COMMAND_OPERATIONS 192U
#define APP_COMMAND_READ_MAX_AGE_MS 250U
typedef struct app_command_bridge app_command_bridge_t;
typedef struct {
    void *user;
    void (*lock)(void *);
    void (*unlock)(void *);
    uint64_t (*now_ms)(void *);
    cp_available_fn available;
    cp_dispatch_fn execute;
    cp_safety_fn safety;
    cp_read_fn read;
    bool (*complete)(void *, const cp_result_t *);
} app_command_bridge_port_t;
size_t app_command_bridge_bytes(void);
cp_error_t app_command_bridge_init(void *, size_t, const app_command_bridge_port_t *, app_command_bridge_t **);
/* Network/management worker calls. No pointer survives the call. */
bool app_command_bridge_available(void *, cp_operation_id_t, char reason[CP_REASON_CAPACITY]);
cp_error_t app_command_bridge_submit(void *, const cp_command_t *, cp_result_t *);
void app_command_bridge_safety(void *, cp_safety_reason_t, const cp_identity_t *);
cp_error_t app_command_bridge_read(void *, const cp_identity_t *, const char *, char *, size_t, size_t *);
/* Sole coordinator calls. Safety is serviced before ordinary work and while
 * a result is awaiting delivery. A failed completion never repeats its effect.
 * Reads return BUSY until the coordinator supplies a recent matching result. */
void app_command_bridge_service(app_command_bridge_t *);
void app_command_bridge_refresh(app_command_bridge_t *);
