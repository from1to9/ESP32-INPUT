// SPDX-License-Identifier: Apache-2.0
/* Compose the unchanged production connectivity owner and real UI. Only its
 * public cached runtime/radio boundaries are synthetic, as in host owner tests. */
#include "../../firmware/main/app_connectivity_controls.c"
#include "../connectivity_controls/review_fixture.h"
#include "ui_trust_review_fixture.h"
#include <assert.h>
static app_control_runtime_snapshot_t fixture_runtime;
static app_control_trust_review_t fixture_review;
static bool fixture_initialized;
static unsigned fixture_approvals;
static uint64_t fixture_now = 1000;
void app_wifi_get_snapshot(app_wifi_snapshot_t *out) { *out = (app_wifi_snapshot_t){0}; }
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out) { *out = (app_wifi_profiles_snapshot_t){0}; }
ble_hid_snapshot_t ble_hid_snapshot(void) { return (ble_hid_snapshot_t){0}; }
ble_hid_bond_list_t ble_hid_bond_list(void) { return (ble_hid_bond_list_t){0}; }
ble_hid_target_snapshot_t ble_hid_target_snapshot(void) { return (ble_hid_target_snapshot_t){0}; }
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *out) { *out = fixture_runtime; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out) { *out = fixture_runtime.trust; }
bool app_control_runtime_commissioning_review(app_control_trust_review_t *out) { *out = fixture_review; return fixture_runtime.trust.review_pending; }
esp_err_t app_wifi_profile_join(const char *id, uint64_t revision) { (void)id; (void)revision; assert(false); return ESP_FAIL; }
esp_err_t app_wifi_profile_forget(const char *id, uint64_t revision) { (void)id; (void)revision; assert(false); return ESP_FAIL; }
esp_err_t app_wifi_profiles_reorder(const char ids[8][33], uint8_t count, uint64_t revision)
{ (void)ids; (void)count; (void)revision; assert(false); return ESP_FAIL; }
esp_err_t ble_hid_target_preferences_request(const ble_hid_target_preferences_request_t *request, uint64_t *id)
{ (void)request; (void)id; assert(false); return ESP_FAIL; }
esp_err_t app_control_runtime_open_commissioning(void) { assert(false); return ESP_FAIL; }
void app_control_runtime_close_commissioning(void) { fixture_runtime.trust.import_window = false; }
esp_err_t app_control_runtime_approve_commissioning(const char digest[65])
{ assert(!memcmp(digest, fixture_review.digest, 65)); ++fixture_approvals; return ESP_OK; }
static void fixture_refresh(app_view_t *view)
{
    app_connectivity_controls_context_t context = {.epoch = view->epoch, .ui_revision = view->ui_revision,
        .mode = view->mode, .overlay = view->overlay, .locked = view->locked, .screen_off = view->screen_off};
    app_connectivity_controls_context(&context); app_connectivity_controls_effect_t effect;
    app_connectivity_controls_poll(++fixture_now, &effect); app_connectivity_controls_fill(view);
}
void ui_trust_fixture_open(app_view_t *view, ui_trust_fixture_kind_t kind)
{
    if (!fixture_initialized) { assert(app_connectivity_controls_init()); fixture_initialized = true; }
    trust_review_fixture(&fixture_review, kind == UI_TRUST_PUBLIC_CAPACITY);
    if (kind == UI_TRUST_PACKAGE_CAPACITY) trust_package_review_fixture(&fixture_review);
    if (kind == UI_TRUST_MINIMAL) {
        fixture_review.capture_grant_count = fixture_review.clock_server_count = 0;
        fixture_review.update_source_present = fixture_review.update_policy_present = false;
    }
    ++fixture_runtime.trust.revision;
    fixture_runtime.initialized = fixture_runtime.trust.initialized = fixture_runtime.trust.known = true;
    fixture_runtime.trust.import_window = fixture_runtime.trust.review_pending = true;
    memcpy(fixture_runtime.trust.candidate_digest, fixture_review.digest, 65);
    view->overlay = APP_OVERLAY_API_REVIEW; ++view->ui_revision; fixture_refresh(view);
}
void ui_trust_fixture_handle(app_view_t *view, const app_intent_t *intent)
{
    app_connectivity_controls_effect_t effect;
    assert(app_connectivity_controls_handle(intent, &effect));
    if (effect.invalidate_ui) ++view->ui_revision;
    fixture_refresh(view);
}
void ui_trust_fixture_change(app_view_t *view, unsigned invalid_field)
{
    trust_review_fixture(&fixture_review, false);
    if (invalid_field == 0) fixture_review.port = 0;
    else if (invalid_field == 1) fixture_review.clock_server_count = APP_CLOCK_SERVER_CAPACITY + 1;
    else if (invalid_field == 2) memset(fixture_review.clock_servers[1], 'x', sizeof(fixture_review.clock_servers[1]));
    else if (invalid_field == 3) fixture_review.update_tls_ca_sha256[0] = 0;
    else if (invalid_field == 4) fixture_review.update_product[0] = 0;
    else if (invalid_field == 5) memset(fixture_review.update_layout, 'x', sizeof(fixture_review.update_layout));
    else { fixture_review.port = 9443; fixture_review.digest[0] = 'f'; }
    memcpy(fixture_runtime.trust.candidate_digest, fixture_review.digest, 65);
    ++fixture_runtime.trust.revision; fixture_refresh(view);
}
void ui_trust_fixture_finish(app_view_t *view)
{
    fixture_runtime.trust.import_window = fixture_runtime.trust.review_pending = false;
    fixture_runtime.trust.present = true; ++fixture_runtime.trust.revision;
    memcpy(fixture_runtime.trust.active_digest, fixture_review.digest, 65); fixture_refresh(view);
}
unsigned ui_trust_fixture_approvals(void) { return fixture_approvals; }
