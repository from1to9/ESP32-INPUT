// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_ui.h"

/* One coordinator owner, after connectivity runtimes initialize. All reads are
 * cached bounded copies; mutations submit only to the existing owning worker.
 * Parent retains physical-intent/remote-ownership validation and main state. */
typedef struct {
    uint64_t epoch, ui_revision;
    input_mode_t mode;
    app_overlay_t overlay;
    app_settings_page_t settings_page;
    app_confirmation_t confirmation;
    /* release_pending belongs to the selected Bluetooth input transport;
     * it is not a pending release on an independently selected USB link. */
    bool locked, screen_off, release_pending, transport_switch_pending;
    bool bluetooth_audio_devices;
} app_connectivity_controls_context_t;
typedef struct {
    bool handled, open_overlay, invalidate_ui, release_input;
    app_overlay_t overlay;
    app_confirmation_t confirmation;
    uint64_t confirmation_host_generation;
    char message[97];
} app_connectivity_controls_effect_t;

bool app_connectivity_controls_init(void);
void app_connectivity_controls_context(const app_connectivity_controls_context_t *);
bool app_connectivity_controls_handle(const app_intent_t *, app_connectivity_controls_effect_t *);
void app_connectivity_controls_poll(uint64_t now_ms, app_connectivity_controls_effect_t *);
/* Called only after the parent's fresh physical Add/Forget-all confirmation.
 * Revision capture and start remain separate to reject changed review data. */
bool app_connectivity_controls_bluetooth_revisions(uint64_t *bond_revision, uint64_t *preferences_revision);
/* Carry the caller's admitted-host generation, or zero for no admitted host. */
bool app_connectivity_controls_bluetooth_start(app_pairing_operation_t, uint64_t host_generation,
    uint64_t bond_revision, uint64_t preferences_revision, app_connectivity_controls_effect_t *);
bool app_connectivity_controls_bluetooth_busy(void);
void app_connectivity_controls_bluetooth_stop(app_connectivity_controls_effect_t *);
/* Owns api/saved_wifi/bluetooth_target and actual saved BLE row metadata only.
 * Also owns confirmed native pairing/removal workflows and their actual ACKs.
 * Does not alter legacy scan/password/setup, media or main settings.
 * Classic audio peer choice remains app_media_controls' single owner. */
void app_connectivity_controls_fill(app_view_t *);
