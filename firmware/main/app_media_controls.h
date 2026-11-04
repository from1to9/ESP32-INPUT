// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_ui.h"

/* One coordinator owner. All runtime/store reads are bounded cached copies;
 * mutations only submit to their worker. No LVGL, codec, filesystem or radio IO.
 * Initialize after the media runtime/store endpoints have been initialized. */
typedef struct {
    uint64_t epoch, ui_revision;
    input_mode_t mode;
    app_overlay_t overlay;
    app_keyboard_submode_t keyboard_submode;
    bool locked, screen_off;
    bool bluetooth_audio_devices; /* Explicit coordinator-owned local management context. */
    app_local_access_t local_access;
    size_t preview_characters; /* Actual draft size; confirm needs free capacity. */
} app_media_controls_context_t;
typedef struct {
    bool handled;
    bool open_overlay;
    app_overlay_t overlay;
    app_confirmation_t confirmation;
    char preview_character; /* Append once through interaction; never send HID. */
    char message[97];
} app_media_controls_effect_t;

bool app_media_controls_init(void);
/* Called synchronously only after applying this owner's open-confirmation effect.
 * Bind its review to the input epoch produced by opening the modal. */
void app_media_controls_review_opened(app_confirmation_t confirmation, uint64_t epoch);
void app_media_controls_context(const app_media_controls_context_t *);
/* Coordinator transition boundary: call before publishing loss of the local
 * device-management epoch, page or access. Cancels only this owner's receipt
 * and follow-up voice phase. The BT lock orders revocation against its final
 * dispatch claim; a claim that already won is not retroactively withdrawn. */
void app_media_controls_revoke_devices(void);
/* Parent must first validate normal physical-touch/current input identity.
 * This layer also validates mode/panel, media/object/candidate identity and
 * explicit immutable confirmations. Connection selection uses audio-only
 * routes, preserves usable Bluetooth profiles, and waits for acknowledged
 * hardware stop before admitting a change. It never changes the HID target
 * or treats selected transport/profile support as an active audio stream.
 * Live role profile changes require the same observed Bluetooth peer.
 * Each role's start_block is the owning admission result; can_start is true
 * exactly for ALLOWED. UI copy must not infer a different readiness state.
 * Return false for an unrelated UI action. */
bool app_media_controls_handle(const app_intent_t *, app_media_controls_effect_t *);
/* Run from the coordinator. Poll consumes only this layer's retained tokens,
 * copies query payload before acknowledging, and never retries a mutation. */
void app_media_controls_poll(uint64_t now_ms, app_media_controls_effect_t *);
/* Copies media/review/storage plus the legacy audio/recognition fields and
 * capture_indicator_required/generation/state from observed owner state.
 * APP_CLOSE remains parent-owned; context invalidates a departed confirmation.
 * Speech context cancellation remains in app_media_runtime_environment. */
void app_media_controls_fill(app_view_t *);
