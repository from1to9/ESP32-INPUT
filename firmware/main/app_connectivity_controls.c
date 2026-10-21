// SPDX-License-Identifier: Apache-2.0
#include "app_connectivity_controls.h"
#include "app_wifi_profiles.h"
#include "app_control_runtime.h"
#include "ble_hid.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#if ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define RESULT_TIMEOUT_MS UINT64_C(30000)
_Static_assert(APP_API_REVIEW_PAGES <= 64, "Review navigation bitmap is bounded");
_Static_assert(APP_CONTROL_TRUST_REVIEW_ABI >= 2, "Native review requires complete public trust authority");
typedef enum { P_NONE, P_WIFI_JOIN, P_WIFI_PREFER, P_WIFI_FORGET, P_BLE, P_API_OPEN,
    P_API_CLOSE, P_API_APPROVE } pending_t;
typedef struct {
    bool initialized, clock_known, clock_failed, target_known;
    uint64_t now_ms, started_ms, api_revision, api_trust_revision;
    uint64_t before_wifi_revision, before_catalog_revision, before_trust_revision, ble_request_id, ble_revision;
    pending_t pending;
    app_connectivity_controls_context_t context;
    app_wifi_snapshot_t wifi;
    app_wifi_profiles_snapshot_t profiles;
    ble_hid_snapshot_t bluetooth;
    ble_hid_bond_list_t bonds;
    ble_hid_target_snapshot_t preference;
    app_control_runtime_snapshot_t runtime;
    app_control_trust_review_t review;
    app_api_view_t api;
    app_api_view_t api_identity;
    app_saved_wifi_view_t saved;
    app_bluetooth_target_view_t target;
    app_bluetooth_target_key_t selected_target;
    char expected_profile[33], expected_ssid[33];
    char expected_order[APP_WIFI_PROFILE_CAPACITY][APP_WIFI_PROFILE_ID_CAPACITY];
    unsigned wifi_page, review_page;
    uint64_t reviewed_pages;
    bool confirm_owned, confirm_valid, confirm_entering;
    uint64_t confirm_epoch, confirm_ui_revision, confirm_profile_revision;
    char confirm_profile[33];
    char expected_digest[65], message[97];
} connectivity_controls_t;
/* Only the serialized coordinator accesses this cache. It contains no lock,
 * DMA buffer, interrupt state or callback-owned memory. */
static EXT_RAM_BSS_ATTR connectivity_controls_t controls;

static bool text(const char *value, size_t capacity)
{
    const char *end = memchr(value, 0, capacity);
    if (!end) return false;
    for (const char *p = value; p != end; ++p) if ((unsigned char)*p < 32U || (unsigned char)*p == 127U) return false;
    return true;
}
static bool hex_text(const char *value, size_t digits)
{
    if (!text(value, digits + 1U) || value[digits]) return false;
    for (size_t i = 0; i < digits; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
static void say(app_connectivity_controls_effect_t *effect, const char *message)
{
    snprintf(controls.message, sizeof(controls.message), "%s", message);
    if (effect) { effect->handled = true; snprintf(effect->message, sizeof(effect->message), "%s", message); }
}
static const char *error_text(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "Request accepted; waiting for observed completion.";
    case ESP_ERR_NOT_SUPPORTED: return "This saved identity or operation is not supported.";
    case ESP_ERR_NO_MEM: return "No capacity is available.";
    case ESP_ERR_NOT_FOUND: return "The saved item is no longer available.";
    case ESP_ERR_INVALID_ARG: return "The request is invalid.";
    default: return "State changed or the service is unavailable. Review current controls.";
    }
}
static bool local_ready(void)
{
    return controls.initialized && controls.clock_known && !controls.clock_failed &&
        !controls.context.locked && !controls.context.screen_off;
}
static bool idle(void) { return local_ready() && controls.pending == P_NONE; }
static bool same_key(const app_bluetooth_target_key_t *key, const ble_hid_bond_t *row)
{
    return row->address_type_known && key->address_type == row->address_type &&
        !memcmp(key->address, row->address, 6) && key->bond_id_known == row->bond_id_known &&
        (!key->bond_id_known || !memcmp(key->bond_id, row->bond_id, BLE_HID_BOND_ID_BYTES));
}
static app_bluetooth_target_key_t ui_key(const ble_hid_bond_t *row)
{
    app_bluetooth_target_key_t key = {.address_type = row->address_type, .bond_id_known = row->bond_id_known};
    memcpy(key.address, row->address, 6); if (row->bond_id_known) memcpy(key.bond_id, row->bond_id, sizeof(key.bond_id)); return key;
}
static ble_hid_target_t owner_key(const ble_hid_bond_t *row)
{
    ble_hid_target_t key = {.address_type = row->address_type, .bond_id_known = row->bond_id_known};
    memcpy(key.address, row->address, 6); if (row->bond_id_known) memcpy(key.bond_id, row->bond_id, sizeof(key.bond_id)); return key;
}
static int find_bond(const app_bluetooth_target_key_t *key)
{
    if (!controls.bonds.known || controls.bonds.updating || controls.bonds.count > BLE_HID_MAX_BONDS) return -1;
    for (unsigned i = 0; i < controls.bonds.count; ++i) if (same_key(key, &controls.bonds.bonds[i])) return (int)i;
    return -1;
}
static int find_profile(const char id[33])
{
    if (!hex_text(id, 32) || !controls.profiles.known || controls.profiles.uncertain ||
        controls.profiles.count > APP_WIFI_PROFILE_CAPACITY) return -1;
    for (unsigned i = 0; i < controls.profiles.count; ++i)
        if (hex_text(controls.profiles.profiles[i].id, 32) && !memcmp(id, controls.profiles.profiles[i].id, 33)) return (int)i;
    return -1;
}
static bool wifi_idle(void)
{
    return idle() && controls.wifi.initialized && controls.profiles.known && !controls.profiles.uncertain &&
        !controls.profiles.operation_pending && !controls.wifi.operation_pending && !controls.wifi.cleanup_pending &&
        !controls.wifi.setup_active && !controls.wifi.storage_uncertain;
}
static bool ble_idle(void)
{
    return idle() && controls.preference.loaded && controls.preference.request_status != BLE_HID_TARGET_PENDING &&
        controls.preference.request_status != BLE_HID_TARGET_APPLYING && controls.preference.request_status != BLE_HID_TARGET_UNCERTAIN &&
        controls.bonds.known && !controls.bonds.updating && controls.bluetooth.initialized && !controls.bluetooth.failed &&
        !controls.bluetooth.connected && !controls.context.release_pending && !controls.context.transport_switch_pending;
}
static void begin(pending_t operation)
{ controls.pending = operation; controls.started_ms = controls.now_ms; }

/* Owner-formatted review pages have a conservative native layout bound: five
 * lines of forty characters/page. Overflow disables approval, never truncates. */
typedef struct { unsigned page, column, lines; size_t used; bool valid; } writer_t;
static void write_char(writer_t *w, char value)
{
    if (!w->valid) return;
    if (value != '\n' && w->column == 40) write_char(w, '\n');
    if (!w->valid) return;
    if (w->lines == 5 || w->used >= 255) {
        ++w->page; w->used = 0; w->column = w->lines = 0;
        if (w->page >= APP_API_REVIEW_PAGES) { w->valid = false; return; }
    }
    controls.api.review_pages[w->page][w->used++] = value;
    controls.api.review_pages[w->page][w->used] = 0;
    if (value == '\n') { ++w->lines; w->column = 0; } else ++w->column;
}
static void write_line(writer_t *w, const char *format, ...)
{
    char line[768]; va_list args; va_start(args, format); int length = vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (length < 0 || (size_t)length >= sizeof(line)) { w->valid = false; return; }
    for (int i = 0; i < length; ++i) write_char(w, line[i]);
    write_char(w, '\n');
}
static void id_text(const uint8_t id[CP_ID_BYTES], char out[33])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < CP_ID_BYTES; ++i) { out[i * 2U] = digits[id[i] >> 4]; out[i * 2U + 1U] = digits[id[i] & 15U]; }
    out[32] = 0;
}
static bool all_reviewed(unsigned count)
{
    if (!count || count > APP_API_REVIEW_PAGES) return false;
    uint64_t mask = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1U;
    return controls.reviewed_pages == mask;
}
static bool build_review(void)
{
    memset(controls.api.review_pages, 0, sizeof(controls.api.review_pages));
    const app_control_trust_review_t *review = &controls.review;
    if (!hex_text(review->digest, 64) || !text(review->host, sizeof(review->host)) || !review->host[0] ||
        !review->principal_count || review->principal_count > CP_MAX_PRINCIPALS || review->capture_grant_count > CP_MAX_CAPTURE_GRANTS ||
        !hex_text(review->certificate_pem_sha256, 64)) return false;
    writer_t writer = {.valid = true};
    write_line(&writer, "Replace unknown store: %s", review->replace_unknown_store ? "yes" : "no");
    write_line(&writer, "TLS certificate SHA-256: %s", review->certificate_pem_sha256);
    if (!review->port || review->clock_server_count > APP_CLOCK_SERVER_CAPACITY) return false;
    write_line(&writer, "HTTPS listener port: %u", review->port);
    write_line(&writer, "Clock servers (unauthenticated SNTP):");
    if (!review->clock_server_count) write_line(&writer, "none");
    for (unsigned i = 0; i < review->clock_server_count; ++i) {
        if (!text(review->clock_servers[i], sizeof(review->clock_servers[i])) || !review->clock_servers[i][0]) return false;
        write_line(&writer, "%s", review->clock_servers[i]);
    }
    for (unsigned i = 0; i < review->principal_count; ++i) {
        if (review->principal_scopes[i] & ~CP_SCOPE_ALL || review->principals[i].scopes != review->principal_scopes[i]) return false;
        char principal[33]; id_text(review->principals[i].id, principal);
        if (!memcmp(review->principals[i].id, (uint8_t[CP_ID_BYTES]){0}, CP_ID_BYTES)) return false;
        write_line(&writer, "Principal %u: %s", i + 1U, principal);
        write_line(&writer, "Enabled: %s; native-only: %s", review->principals[i].enabled ? "yes" : "no", review->principals[i].native_only ? "yes" : "no");
        write_line(&writer, "Permissions:");
        if (!review->principal_scopes[i]) write_line(&writer, "none");
        for (size_t bit = 0; bit < 64; ++bit) if (review->principal_scopes[i] & (UINT64_C(1) << bit)) {
            const char *name = cp_scope_name(bit); if (!name) return false; write_line(&writer, "%s", name);
        }
    }
    for (unsigned i = 0; i < review->capture_grant_count; ++i) {
        const cp_capture_grant_t *grant = &review->grants[i]; char id[33], principal[33];
        id_text(grant->id, id); id_text(grant->principal_id, principal);
        static const char *const purposes[] = {"speech", "live", "recording", "background"};
        if ((unsigned)grant->purpose >= sizeof(purposes) / sizeof(purposes[0])) return false;
        write_line(&writer, "Grant %u: %s", i + 1U, id); write_line(&writer, "Principal: %s", principal);
        write_line(&writer, "Purpose: %s; revision: %" PRIu64, purposes[grant->purpose], grant->revision);
        write_line(&writer, "Expires Unix: %" PRIu64, grant->expires_unix_seconds);
        write_line(&writer, "Duration: %us; bytes: %" PRIu64, grant->maximum_duration_seconds, grant->maximum_bytes);
        write_line(&writer, "Retention: %us", grant->maximum_retention_seconds);
        write_line(&writer, "Live: %s; file: %s; download: %s", grant->live_destination ? "yes" : "no", grant->file_destination ? "yes" : "no", grant->owner_download ? "yes" : "no");
    }
    write_line(&writer, "Firmware source: %s", review->update_source_present ? "configured" : "none");
    if (review->update_source_present) {
        if (!text(review->update_catalog_url, sizeof(review->update_catalog_url)) || !review->update_catalog_url[0] ||
            !text(review->update_signing_key_id, sizeof(review->update_signing_key_id)) || !review->update_signing_key_id[0] ||
            !hex_text(review->update_public_key_sha256, 64)) return false;
        write_line(&writer, "Catalog: %s", review->update_catalog_url); write_line(&writer, "Signing key: %s", review->update_signing_key_id);
        write_line(&writer, "Public key SHA-256: %s", review->update_public_key_sha256);
        if (!text(review->update_product, sizeof(review->update_product)) || !review->update_product[0] ||
            !text(review->update_board, sizeof(review->update_board)) || !review->update_board[0] ||
            !text(review->update_layout, sizeof(review->update_layout)) || !review->update_layout[0] ||
            !hex_text(review->update_tls_ca_sha256, 64)) return false;
        write_line(&writer, "Product: %s; board: %s", review->update_product, review->update_board);
        write_line(&writer, "Layout: %s", review->update_layout);
        write_line(&writer, "Minimum release: %u; private source: %s", review->update_minimum_release_index, review->update_allow_private_source ? "yes" : "no");
        write_line(&writer, "Source TLS CA SHA-256: %s", review->update_tls_ca_sha256);
    }
    write_line(&writer, "Firmware update policy: %s", review->update_policy_present ? "configured" : "none");
    if (review->update_policy_present) {
        const app_update_policy_t *policy = &review->update_policy; char id[33], principal[33];
        id_text(policy->id, id); id_text(policy->principal_id, principal);
        if (!text(policy->allowed_digest, sizeof(policy->allowed_digest))) return false;
        write_line(&writer, "Policy: %s", id); write_line(&writer, "Principal: %s", principal);
        write_line(&writer, "Revision: %" PRIu64 "; expires Unix: %" PRIu64, policy->revision, policy->expires_unix_seconds);
        write_line(&writer, "Maximum job: %ums; bytes: %u", policy->maximum_job_ms, policy->maximum_bytes);
        write_line(&writer, "Download: %s; activate: %s", policy->allow_download ? "yes" : "no", policy->allow_activate ? "yes" : "no");
        write_line(&writer, "Restart: %s; owner-loss continue: %s", policy->allow_restart ? "yes" : "no", policy->continue_after_owner_loss ? "yes" : "no");
        write_line(&writer, "Signed releases: %s; exact digest:", policy->allow_signed_releases ? "yes" : "no");
        write_line(&writer, "%s", policy->allowed_digest[0] ? policy->allowed_digest : "none");
    }
    controls.api.review_page_count = writer.page < APP_API_REVIEW_PAGES ? writer.page + 1U : APP_API_REVIEW_PAGES;
    return writer.valid;
}
static void rebuild(void)
{
    bool ready = local_ready();
    controls.saved = (app_saved_wifi_view_t){.known = controls.profiles.known && controls.profiles.count <= APP_SAVED_WIFI_ROWS,
        .uncertain = controls.profiles.uncertain || controls.wifi.storage_uncertain,
        .pending = controls.profiles.operation_pending || (controls.pending >= P_WIFI_JOIN && controls.pending <= P_WIFI_FORGET),
        .revision = controls.profiles.revision, .page = controls.wifi_page};
    if (controls.saved.known) controls.saved.count = controls.profiles.count;
    if (controls.saved.page * 4U >= controls.saved.count) controls.saved.page = controls.wifi_page = 0;
    for (size_t i = 0; i < controls.saved.count; ++i) {
        const app_wifi_profile_view_t *row = &controls.profiles.profiles[i]; app_saved_wifi_row_t *out = &controls.saved.rows[i];
        bool valid = hex_text(row->id, 32) && text(row->ssid, sizeof(row->ssid)) && row->ssid[0];
        if (valid) { memcpy(out->id, row->id, sizeof(out->id)); snprintf(out->label, sizeof(out->label), "%s", row->ssid); }
        out->enabled = row->enabled; out->current = row->current; out->priority = row->priority;
        out->can_join = valid && row->enabled && wifi_idle() && controls.wifi.state != APP_WIFI_CONNECTING && controls.wifi.state != APP_WIFI_OBTAINING_IP;
        out->can_prefer = valid && wifi_idle() && i > 0; out->can_forget = valid && wifi_idle();
    }
    if (!controls.saved.known) snprintf(controls.saved.reason, sizeof(controls.saved.reason), "Saved network status unavailable.");
    else if (controls.saved.uncertain) snprintf(controls.saved.reason, sizeof(controls.saved.reason), "Saved network outcome uncertain. Verify storage before retrying.");
    else snprintf(controls.saved.reason, sizeof(controls.saved.reason), "%s", controls.message);
    controls.target = (app_bluetooth_target_view_t){.bond_revision = controls.bonds.revision, .preferences_revision = controls.preference.revision,
        .pending = controls.pending == P_BLE || controls.preference.request_status == BLE_HID_TARGET_PENDING || controls.preference.request_status == BLE_HID_TARGET_APPLYING,
        .uncertain = controls.preference.request_status == BLE_HID_TARGET_UNCERTAIN};
    int found = controls.target_known ? find_bond(&controls.selected_target) : -1;
    if (found >= 0) {
        const ble_hid_bond_t *row = &controls.bonds.bonds[found]; controls.target.known = true; controls.target.target = ui_key(row);
        controls.target.selected = row->is_selected;
        snprintf(controls.target.name, sizeof(controls.target.name), "Saved input host");
        snprintf(controls.target.identity, sizeof(controls.target.identity), "%02X:%02X:%02X:%02X:%02X:%02X (type %u)", row->address[0], row->address[1], row->address[2], row->address[3], row->address[4], row->address[5], row->address_type);
        controls.target.can_select = ble_idle() && row->selection_supported; controls.target.can_prefer = ble_idle() && found > 0;
        snprintf(controls.target.reason, sizeof(controls.target.reason), "%s", !row->selection_supported ? "Exact selection is unavailable for this private or unverified identity." :
            controls.bluetooth.connected ? "Release input and disconnect the current host before changing targets." : controls.message);
    } else snprintf(controls.target.reason, sizeof(controls.target.reason), "The saved target is unavailable or changed. Select a fresh row.");
    controls.api = (app_api_view_t){.known = controls.runtime.initialized && controls.runtime.trust.initialized,
        .tls = controls.runtime.server.tls, .commissioning_open = controls.runtime.commissioning_open,
        .usb_import_active = controls.runtime.usb_import_active, .remaining_known = controls.runtime.trust.import_window && controls.runtime.trust.window_remaining_ms != 0,
        .remaining_seconds = controls.runtime.trust.window_remaining_ms / 1000U + (controls.runtime.trust.window_remaining_ms % 1000U != 0 ? 1U : 0U),
        .review_pending = controls.runtime.trust.review_pending};
    controls.api.ready = controls.api.known && controls.runtime.server.state == APP_CONTROL_READY && controls.api.tls &&
        text(controls.runtime.server.address, sizeof(controls.runtime.server.address)) && !strncmp(controls.runtime.server.address, "https://", 8);
    if (controls.api.ready) memcpy(controls.api.url, controls.runtime.server.address, sizeof(controls.api.url));
    controls.api.can_open = ready && controls.pending == P_NONE && controls.api.known && !controls.api.commissioning_open && !controls.api.usb_import_active;
    controls.api.can_close = ready && controls.api.known && controls.api.commissioning_open;
    bool review_valid = controls.api.review_pending && controls.runtime.commissioning_open &&
        app_control_runtime_commissioning_review(&controls.review) &&
        !memcmp(controls.review.digest, controls.runtime.trust.candidate_digest, sizeof(controls.review.digest));
    if (review_valid) {
        memcpy(controls.api.review_digest, controls.review.digest, sizeof(controls.api.review_digest));
        memcpy(controls.api.review_host, controls.review.host, sizeof(controls.api.review_host));
        controls.api.review_complete = build_review();
    }
    const app_api_view_t *old = &controls.api_identity;
    bool changed = controls.api_trust_revision != controls.runtime.trust.revision || old->known != controls.api.known ||
        old->commissioning_open != controls.api.commissioning_open || old->review_complete != controls.api.review_complete ||
        old->ready != controls.api.ready || old->tls != controls.api.tls || old->usb_import_active != controls.api.usb_import_active ||
        memcmp(old->review_digest, controls.api.review_digest, sizeof(old->review_digest)) ||
        memcmp(old->review_host, controls.api.review_host, sizeof(old->review_host)) ||
        memcmp(old->review_pages, controls.api.review_pages, sizeof(old->review_pages)) || memcmp(old->url, controls.api.url, sizeof(old->url));
    if (changed) {
        controls.api_identity = controls.api; controls.api_trust_revision = controls.runtime.trust.revision;
        controls.reviewed_pages = 0; controls.review_page = 0;
        if (controls.api_revision != UINT64_MAX) ++controls.api_revision; else controls.clock_failed = true;
    }
    controls.api.revision = controls.api_revision;
    if (controls.review_page >= controls.api.review_page_count) controls.review_page = 0;
    controls.api.review_page = controls.review_page;
    if (controls.context.overlay == APP_OVERLAY_API_REVIEW && controls.api.review_complete) controls.reviewed_pages |= UINT64_C(1) << controls.review_page;
    controls.api.can_approve = idle() && controls.context.overlay == APP_OVERLAY_API_REVIEW && controls.api.review_complete &&
        controls.api.commissioning_open && all_reviewed(controls.api.review_page_count);
    snprintf(controls.api.reason, sizeof(controls.api.reason), "%s", controls.api.review_pending && !controls.api.review_complete ?
        "The complete identity and permissions cannot be displayed; approval is unavailable." : controls.message);
}

static void refresh_owners(void)
{
    app_wifi_get_snapshot(&controls.wifi); app_wifi_profiles_snapshot(&controls.profiles);
    controls.bluetooth = ble_hid_snapshot(); controls.bonds = ble_hid_bond_list(); controls.preference = ble_hid_target_snapshot();
    app_control_runtime_snapshot(&controls.runtime);
    /* The runtime's published management snapshot may lag its immediate local
     * window gate. Use the trust owner's current cached gate before admission. */
    app_control_trust_snapshot(&controls.runtime.trust);
    controls.runtime.commissioning_open = controls.runtime.trust.import_window;
}

bool app_connectivity_controls_init(void)
{
    if (controls.initialized) return false;
    memset(&controls, 0, sizeof(controls)); controls.initialized = true; controls.api_revision = 1; return true;
}
void app_connectivity_controls_context(const app_connectivity_controls_context_t *context)
{
    if (!context || !controls.initialized) return;
    if (controls.confirm_owned) {
        bool confirm = context->overlay == APP_OVERLAY_CONFIRM && context->confirmation == APP_CONFIRM_WIFI_FORGET;
        if (controls.confirm_entering && confirm) {
            /* Parent open_overlay cancels HID and advances epoch/revision.
             * Bind the one requested review transition, then freeze identity. */
            controls.confirm_epoch = context->epoch; controls.confirm_ui_revision = context->ui_revision; controls.confirm_entering = false;
        } else if (controls.confirm_entering && (context->overlay != APP_OVERLAY_WIFI_PROFILES ||
            context->epoch != controls.confirm_epoch || context->ui_revision != controls.confirm_ui_revision)) {
            controls.confirm_valid = false; controls.confirm_owned = false;
        } else if (!controls.confirm_entering && (!confirm || context->epoch != controls.confirm_epoch ||
            context->ui_revision != controls.confirm_ui_revision)) controls.confirm_valid = false;
        if (context->locked || context->screen_off) controls.confirm_valid = false;
        if (!confirm && !controls.confirm_entering) controls.confirm_owned = false;
    }
    if (context->overlay != APP_OVERLAY_API_REVIEW || context->locked || context->screen_off || context->epoch != controls.context.epoch)
        controls.reviewed_pages = 0;
    controls.context = *context;
}
static bool submit_wifi(pending_t operation, const char id[33], app_connectivity_controls_effect_t *effect)
{
    int found = find_profile(id); if (found < 0 || !wifi_idle()) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return false; }
    esp_err_t error;
    controls.before_wifi_revision = controls.wifi.revision; controls.before_catalog_revision = controls.profiles.revision;
    memcpy(controls.expected_profile, id, 33); memcpy(controls.expected_ssid, controls.profiles.profiles[found].ssid, 33);
    if (operation == P_WIFI_JOIN) error = app_wifi_profile_join(id, controls.profiles.revision);
    else if (operation == P_WIFI_FORGET) error = app_wifi_profile_forget(id, controls.profiles.revision);
    else {
        unsigned next = 0; memset(controls.expected_order, 0, sizeof(controls.expected_order));
        memcpy(controls.expected_order[next++], id, 33);
        for (unsigned i = 0; i < controls.profiles.count; ++i) if ((int)i != found) memcpy(controls.expected_order[next++], controls.profiles.profiles[i].id, 33);
        error = app_wifi_profiles_reorder((const char (*)[APP_WIFI_PROFILE_ID_CAPACITY])controls.expected_order, controls.profiles.count, controls.profiles.revision);
    }
    if (error == ESP_OK) begin(operation);
    say(effect, error_text(error)); return error == ESP_OK;
}
static void handle_wifi(const app_saved_wifi_request_t *request, app_connectivity_controls_effect_t *effect)
{
    if (controls.context.overlay != APP_OVERLAY_WIFI_PROFILES || request->revision != controls.profiles.revision || !controls.saved.known) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
    if (request->operation == APP_SAVED_WIFI_PAGE) {
        if (request->page < 2 && request->page * 4U < controls.saved.count) { controls.wifi_page = request->page; effect->invalidate_ui = true; } return;
    }
    int found = find_profile(request->id);
    if (found < 0 || !wifi_idle()) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
    if (request->operation == APP_SAVED_WIFI_FORGET_REQUEST) {
        if (!controls.saved.rows[found].can_forget) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
        controls.confirm_owned = controls.confirm_valid = controls.confirm_entering = true;
        controls.confirm_epoch = controls.context.epoch; controls.confirm_ui_revision = controls.context.ui_revision;
        controls.confirm_profile_revision = controls.profiles.revision; memcpy(controls.confirm_profile, request->id, 33);
        effect->open_overlay = true; effect->overlay = APP_OVERLAY_CONFIRM; effect->confirmation = APP_CONFIRM_WIFI_FORGET; return;
    }
    if (request->operation == APP_SAVED_WIFI_JOIN && controls.saved.rows[found].can_join) (void)submit_wifi(P_WIFI_JOIN, request->id, effect);
    else if (request->operation == APP_SAVED_WIFI_PREFER && controls.saved.rows[found].can_prefer) (void)submit_wifi(P_WIFI_PREFER, request->id, effect);
    else say(effect, error_text(ESP_ERR_INVALID_STATE));
}
static void handle_bluetooth(const app_bluetooth_target_request_t *request, app_connectivity_controls_effect_t *effect)
{
    bool page = controls.context.overlay == APP_OVERLAY_BLUETOOTH_TARGET ||
        (controls.context.overlay == APP_OVERLAY_SETTINGS && controls.context.settings_page == APP_SETTINGS_BLUETOOTH);
    int found = find_bond(&request->target);
    if (!page || found < 0 || request->bond_revision != controls.bonds.revision || request->preferences_revision != controls.preference.revision) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
    if (request->operation == APP_BLUETOOTH_TARGET_DETAILS) {
        controls.selected_target = request->target; controls.target_known = true;
        effect->open_overlay = true; effect->overlay = APP_OVERLAY_BLUETOOTH_TARGET; return;
    }
    if (controls.context.overlay != APP_OVERLAY_BLUETOOTH_TARGET || !ble_idle() ||
        (request->operation != APP_BLUETOOTH_TARGET_SELECT && request->operation != APP_BLUETOOTH_TARGET_PREFER) ||
        (request->operation == APP_BLUETOOTH_TARGET_SELECT && !controls.bonds.bonds[found].selection_supported)) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
    ble_hid_target_preferences_request_t update = {.expected_bond_revision = request->bond_revision,
        .expected_revision = request->preferences_revision, .selected = controls.preference.selected, .target = controls.preference.target, .count = controls.bonds.count};
    if (request->operation == APP_BLUETOOTH_TARGET_SELECT) { update.selected = true; update.target = owner_key(&controls.bonds.bonds[found]); }
    unsigned next = 0;
    if (request->operation == APP_BLUETOOTH_TARGET_PREFER) update.order[next++] = owner_key(&controls.bonds.bonds[found]);
    for (unsigned i = 0; i < controls.bonds.count; ++i) if (request->operation != APP_BLUETOOTH_TARGET_PREFER || (int)i != found) update.order[next++] = owner_key(&controls.bonds.bonds[i]);
    esp_err_t error = ble_hid_target_preferences_request(&update, &controls.ble_request_id);
    if (error == ESP_OK) { controls.ble_revision = controls.preference.revision; begin(P_BLE); }
    say(effect, error_text(error));
}
static void handle_api(const app_api_request_t *request, app_connectivity_controls_effect_t *effect)
{
    bool page = controls.context.overlay == APP_OVERLAY_API_CONNECTION || controls.context.overlay == APP_OVERLAY_API_REVIEW;
    if (!page || request->revision != controls.api.revision) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return; }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (request->operation == APP_API_REVIEW_PAGE) {
        unsigned current = controls.review_page;
        if (controls.context.overlay == APP_OVERLAY_API_REVIEW && controls.api.review_complete && request->page < controls.api.review_page_count &&
            (request->page + 1U == current || request->page == current + 1U) &&
            !memcmp(request->review_digest, controls.api.review_digest, 65)) {
            controls.review_page = request->page; controls.reviewed_pages |= UINT64_C(1) << request->page; effect->invalidate_ui = true;
        } else say(effect, error_text(error));
        return;
    }
    if (request->operation == APP_API_WINDOW_CLOSE && controls.api.can_close) {
        app_control_runtime_close_commissioning();
        if (controls.pending == P_NONE || controls.pending == P_API_OPEN) begin(P_API_CLOSE);
        error = ESP_OK;
    }
    else if (request->operation == APP_API_WINDOW_OPEN && controls.api.can_open) { error = app_control_runtime_open_commissioning(); if (error == ESP_OK) begin(P_API_OPEN); }
    else if (request->operation == APP_API_REVIEW_APPROVE && controls.api.can_approve &&
        request->page == controls.review_page && request->page + 1U == controls.api.review_page_count &&
        !memcmp(request->review_digest, controls.api.review_digest, 65)) {
        error = app_control_runtime_approve_commissioning(request->review_digest);
        if (error == ESP_OK) { memcpy(controls.expected_digest, request->review_digest, 65); controls.before_trust_revision = controls.runtime.trust.revision; begin(P_API_APPROVE); }
    }
    say(effect, error_text(error));
}
bool app_connectivity_controls_handle(const app_intent_t *intent, app_connectivity_controls_effect_t *effect)
{
    if (!effect) return false;
    *effect = (app_connectivity_controls_effect_t){0};
    bool confirm = intent && intent->action == APP_CONFIRM && controls.confirm_owned;
    if (!intent || (!confirm && intent->action != APP_API_COMMISSIONING && intent->action != APP_WIFI_PROFILE_CONTROL && intent->action != APP_BLUETOOTH_TARGET_CONTROL)) return false;
    if (intent->action == APP_API_COMMISSIONING && intent->api.operation != APP_API_WINDOW_OPEN &&
        intent->api.operation != APP_API_WINDOW_CLOSE && intent->api.operation != APP_API_REVIEW_PAGE &&
        intent->api.operation != APP_API_REVIEW_APPROVE) return false;
    effect->handled = true; refresh_owners(); rebuild();
    if (!local_ready() || intent->epoch != controls.context.epoch || intent->ui_revision != controls.context.ui_revision) { say(effect, error_text(ESP_ERR_INVALID_STATE)); return true; }
    if (confirm) {
        bool valid = controls.confirm_valid && !controls.confirm_entering && controls.context.overlay == APP_OVERLAY_CONFIRM &&
            controls.context.confirmation == APP_CONFIRM_WIFI_FORGET && intent->ui_revision == controls.confirm_ui_revision &&
            controls.confirm_profile_revision == controls.profiles.revision;
        controls.confirm_owned = controls.confirm_valid = false;
        if (intent->x == 1 && valid) (void)submit_wifi(P_WIFI_FORGET, controls.confirm_profile, effect);
        else say(effect, intent->x == 0 ? "Forget cancelled." : error_text(ESP_ERR_INVALID_STATE));
        effect->open_overlay = true; effect->overlay = APP_OVERLAY_WIFI_PROFILES;
    } else if (intent->action == APP_WIFI_PROFILE_CONTROL) handle_wifi(&intent->wifi_profile, effect);
    else if (intent->action == APP_BLUETOOTH_TARGET_CONTROL) handle_bluetooth(&intent->bluetooth_target, effect);
    else handle_api(&intent->api, effect);
    rebuild(); return true;
}
void app_connectivity_controls_poll(uint64_t now_ms, app_connectivity_controls_effect_t *effect)
{
    if (effect) *effect = (app_connectivity_controls_effect_t){0};
    if (!controls.initialized) return;
    if (controls.clock_known && now_ms < controls.now_ms) controls.clock_failed = true;
    controls.clock_known = true; controls.now_ms = now_ms;
    refresh_owners();
    pending_t pending = controls.pending; const char *message = NULL;
    if (pending == P_BLE) {
        if (controls.preference.request_id != controls.ble_request_id) message = "Preference state changed; the result is unconfirmed.";
        else if (controls.preference.request_status == BLE_HID_TARGET_APPLIED) message = controls.preference.durable &&
            controls.preference.revision > controls.ble_revision ? "Input host preference saved." : "Preference outcome is unconfirmed.";
        else if (controls.preference.request_status == BLE_HID_TARGET_FAILED) message = "Preference was not saved. Previous policy remains active.";
        else if (controls.preference.request_status == BLE_HID_TARGET_UNCERTAIN) message = "Preference outcome uncertain. Restart and inspect stored state.";
    } else if (pending >= P_WIFI_JOIN && pending <= P_WIFI_FORGET) {
        bool changed = controls.wifi.revision != controls.before_wifi_revision;
        if (controls.profiles.uncertain || controls.wifi.storage_uncertain) message = "Saved network outcome uncertain. Verify storage before retrying.";
        else if (!controls.wifi.operation_pending && !controls.profiles.operation_pending && changed) {
            if (pending == P_WIFI_JOIN && controls.wifi.lan_ready && controls.wifi.state == APP_WIFI_CONNECTED &&
                !memcmp(controls.wifi.ssid, controls.expected_ssid, 33)) message = "Connected to the selected saved network.";
            else if (pending != P_WIFI_JOIN && controls.profiles.known && controls.profiles.revision > controls.before_catalog_revision) {
                bool verified = pending == P_WIFI_FORGET ? find_profile(controls.expected_profile) < 0 : true;
                if (pending == P_WIFI_PREFER) for (unsigned i = 0; i < controls.profiles.count; ++i)
                    if (memcmp(controls.profiles.profiles[i].id, controls.expected_order[i], 33)) verified = false;
                message = verified ? (pending == P_WIFI_FORGET ? "Saved network forgotten." : "Saved network priority verified.") : "Saved network state changed; result unconfirmed.";
            } else if (controls.wifi.last_error != ESP_OK || controls.wifi.connection_error != ESP_OK) message = "Network operation failed. Review the observed network state.";
        }
    } else if (pending == P_API_OPEN && controls.runtime.commissioning_open) message = "Local USB enrollment window is open.";
    else if (pending == P_API_CLOSE && !controls.runtime.commissioning_open && !controls.runtime.usb_import_active) message = "Local USB enrollment window is closed.";
    else if (pending == P_API_APPROVE) {
        if (controls.runtime.trust.uncertain) message = "Identity storage outcome uncertain. Inspect stored trust before retrying.";
        else if (controls.runtime.trust.known && controls.runtime.trust.present && !controls.runtime.trust.review_pending &&
            !controls.runtime.trust.import_window && controls.runtime.trust.revision > controls.before_trust_revision) {
#if APP_CONTROL_TRUST_REVIEW_ABI >= 2
            message = !memcmp(controls.runtime.trust.active_digest, controls.expected_digest, 65) ?
                "Reviewed API identity stored. HTTPS readiness is reported separately." : "Trust storage changed; the exact identity is unconfirmed.";
#else
            message = "Trust storage changed; the exact identity is unconfirmed.";
#endif
        }
        else if (!controls.runtime.trust.import_window && !controls.runtime.trust.review_pending) message = "Enrollment ended without verified storage.";
    }
    if (pending != P_NONE && !message && now_ms >= controls.started_ms && now_ms - controls.started_ms >= RESULT_TIMEOUT_MS)
        message = "Outcome not confirmed. Inspect current state before retrying.";
    if (message) { controls.pending = P_NONE; say(effect, message); }
    if ((controls.context.locked || controls.context.screen_off) && controls.runtime.commissioning_open) app_control_runtime_close_commissioning();
    rebuild();
}
void app_connectivity_controls_fill(app_view_t *view)
{
    if (!view || !controls.initialized) return;
    rebuild();
    view->api = controls.api; view->saved_wifi = controls.saved; view->bluetooth_target = controls.target;
    view->wifi_lan_service_url[0] = 0;
    const char *service_message;
    if (!controls.api.known) {
        view->wifi_lan_service = APP_UI_LAN_SERVICE_UNAVAILABLE; service_message = "HTTPS service status unavailable.";
    } else if (controls.api.ready) {
        view->wifi_lan_service = APP_UI_LAN_SERVICE_READY; service_message = "HTTPS listener is ready.";
        if (strlen(controls.api.url) < sizeof(view->wifi_lan_service_url))
            memcpy(view->wifi_lan_service_url, controls.api.url, strlen(controls.api.url) + 1U);
        else service_message = "HTTPS ready; Web/API panel shows the complete address.";
    } else if (controls.runtime.server.state == APP_CONTROL_ERROR || controls.runtime.server.state == APP_CONTROL_READY) {
        view->wifi_lan_service = APP_UI_LAN_SERVICE_ERROR; service_message = "A running TLS listener is not verified.";
    } else if (controls.runtime.server.state == APP_CONTROL_STARTING) {
        view->wifi_lan_service = APP_UI_LAN_SERVICE_STARTING; service_message = "HTTPS listener is starting.";
    } else {
        view->wifi_lan_service = APP_UI_LAN_SERVICE_DISABLED;
        service_message = controls.runtime.server.state == APP_CONTROL_STOPPING ? "HTTPS listener is stopping." : "HTTPS listener is stopped.";
    }
    snprintf(view->wifi_lan_service_message, sizeof(view->wifi_lan_service_message), "%s", service_message);
    view->bluetooth_hosts_available = controls.bonds.known && !controls.bonds.updating && controls.bonds.count <= APP_UI_BOND_CAPACITY;
    view->bluetooth_host_count = view->bluetooth_hosts_available && controls.bonds.count <= APP_UI_BOND_CAPACITY ? controls.bonds.count : 0;
    memset(view->bluetooth_hosts, 0, sizeof(view->bluetooth_hosts));
    for (size_t i = 0; i < view->bluetooth_host_count; ++i) {
        const ble_hid_bond_t *row = &controls.bonds.bonds[i]; app_bond_view_t *out = &view->bluetooth_hosts[i];
        if (row->address_type_known) snprintf(out->identity, sizeof(out->identity), "%02X:%02X:%02X:%02X:%02X:%02X/%u", row->address[0], row->address[1], row->address[2], row->address[3], row->address[4], row->address[5], row->address_type);
        else snprintf(out->identity, sizeof(out->identity), "%02X:%02X:%02X:%02X:%02X:%02X/?", row->address[0], row->address[1], row->address[2], row->address[3], row->address[4], row->address[5]);
        out->connected = controls.bluetooth.connected && controls.bonds.current_match_known && row->is_current;
        out->selected = row->is_selected; out->target_known = row->address_type_known;
        out->target = ui_key(row); out->bond_revision = controls.bonds.revision; out->preferences_revision = controls.preference.revision;
    }
}
