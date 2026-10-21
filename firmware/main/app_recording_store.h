// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_store.h"

/* Allocate a stable store endpoint in PSRAM. All physical opening, recovery,
 * key/nonce commits and explicit provisioning run through port.service in the
 * audio worker. Init never erases or claims a partition. Call before passing
 * port() to app_media_runtime_init; the port remains valid while unavailable. */
typedef struct {
    void *clock_context;
    ae_result_t (*wall_time)(void *, uint64_t *seconds);
} app_recording_store_config_t;
typedef struct {
    bool initialized, preparing, byte_region_owned;
    ae_store_status_t store;
    ae_store_capabilities_t capabilities;
    ae_result_t error;
} app_recording_store_snapshot_t;
ae_result_t app_recording_store_init(const app_recording_store_config_t *);
ae_store_port_t app_recording_store_port(void);
bool app_recording_store_snapshot(app_recording_store_snapshot_t *);
