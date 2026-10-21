// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_ui.h"
#include "esp_err.h"

#define APP_PRESENTER_PERIOD_MS 100U
/* Match the UI's owned text bounds, including the terminating NUL. */
#define APP_PRESENTER_MESSAGE_BYTES 160U
#define APP_PRESENTER_TARGET_BYTES 96U

typedef enum {
    APP_PRESENTER_ACCEPTED,
    APP_PRESENTER_UNAVAILABLE,
    APP_PRESENTER_INVALID,
    APP_PRESENTER_STALE
} app_presenter_result_t;

typedef struct {
    bool initialized;
    esp_err_t init_error;
    bool has_published;
    bool has_applied;
    bool pending;
    bool applying;
    uint64_t published_count;
    uint64_t applied_count;
    uint64_t coalesced_count;
    uint64_t stale_count;
    uint64_t published_epoch;
    uint64_t published_revision;
    uint64_t applied_epoch;
    uint64_t applied_revision;
    uint64_t publish_age_ms; /* Meaningful only when has_published. */
    uint64_t apply_age_ms;   /* Since completion; only when has_applied. */
    uint64_t pending_age_ms; /* Since first outstanding publish; only when pending. */
    uint64_t last_apply_latency_ms; /* Latest consumed publication to completion. */
} app_presenter_status_t;

/* Startup only, after app_ui_create, under the existing LVGL/display lock.
 * Creates one LVGL timer; failure is retained in status and rejects publish.
 * An explicit retry after failure is allowed; a second successful init is not.
 * No runtime display-lock acquisition, worker task or work queue is used. */
esp_err_t app_presenter_init(void);

/* Task context, not ISR. Copies into one bounded static latest-value slot,
 * including message/target strings. Caller storage may expire after return.
 * A short critical section protects copies; never waits for rendering, the
 * display mutex, a queue or device I/O. New pending state replaces old state.
 * First epoch/revision (including zero) is accepted. Thereafter neither may
 * decrease; equal values allow changing telemetry. Values are never rewritten.
 * ACCEPTED means queued for presentation, not rendered or hardware-acknowledged.
 * The coordinator must continue rejecting intents with obsolete identities.
 * The caller remains responsible for wiping its own credential storage. */
app_presenter_result_t app_presenter_publish(const app_view_t *view);

/* Bounded observation only; never renders or consumes pending state. Counts
 * saturate at UINT64_MAX. Ages use the monotonic clock, clamped on regression.
 * No secret or text content is returned. */
app_presenter_status_t app_presenter_status(void);

#define APP_PRESENTER_OWNER_MAX_AGE_MS 250U
typedef struct {
    bool bound, indicator_armed, owner_alive, clock_valid;
    bool indicator_transfer_confirmed;
    bool usable; /* Matching transfer plus live LVGL owner; brightness is separate. */
    uint64_t display_generation, capture_generation, indicator_revision, refresh_sequence;
    uint64_t completed_us; /* Actual observed panel IO completion; never heartbeat time. */
    uint64_t owner_heartbeat_ms;
    uint64_t completion_age_ms, heartbeat_age_ms;
    esp_err_t error;
} app_presenter_indicator_status_t;

/* Startup under the existing LVGL lock, after UI and Board observer setup.
 * Bind one 480 x 480, unrotated, zero-offset display. Installs refresh/delete
 * event observers only; no extra flush, wait or framebuffer. Binding failure
 * keeps ordinary UI presentation available but provides no capture permission. */
esp_err_t app_presenter_bind_display(lv_display_t *display);
/* Task-context copied observation, with no LVGL API or display-lock acquisition.
 * Exact Board-confirmed coverage/submission/completion is required initially.
 * An unchanged marker may retain that proof while the separate owner heartbeat
 * remains fresh; an empty refresh never manufactures/renews completed_us.
 * The coordinator must additionally require acknowledged positive brightness,
 * active display power and its independent privacy/authority gates. */
app_presenter_indicator_status_t app_presenter_indicator_status(void);
