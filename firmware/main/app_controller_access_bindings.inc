// SPDX-License-Identifier: Apache-2.0
/* Only the coordinator reads this cache. The access service rechecks the
 * expected revision at its write boundary and owns the durable result. */
_Static_assert(APP_CONTROL_ACCESS_CLIENTS + 1 == APP_UI_CONTROLLER_ROWS, "controller list capacity");
_Static_assert(CP_ID_BYTES == sizeof(((app_controller_key_t *)0)->id), "controller identity width");
_Static_assert((int)APP_CONTROL_ACCESS_INPUT == APP_UI_GRANT_INPUT &&
               (int)APP_CONTROL_ACCESS_MANAGE == APP_UI_GRANT_MANAGE &&
               (int)APP_CONTROL_ACCESS_UPDATE == APP_UI_GRANT_UPDATE, "controller permissions");
_Static_assert((int)APP_CONTROL_ACCESS_SIX_HOURS == APP_UI_GRANT_6H &&
               (int)APP_CONTROL_ACCESS_DAY == APP_UI_GRANT_24H &&
               (int)APP_CONTROL_ACCESS_PERMANENT == APP_UI_GRANT_PERMANENT, "controller durations");
static EXT_RAM_BSS_ATTR app_control_access_snapshot_t s_controller_catalog;
static app_controller_key_t s_controller_selection;
static uint64_t s_controller_receipt;
static app_ui_grant_op_t s_controller_operation;
static const char *s_controller_notice;

static bool controller_catalog_known(void)
{
    return s_controller_catalog.known && !s_controller_catalog.uncertain &&
        s_controller_catalog.revision && s_controller_catalog.count <= APP_CONTROL_ACCESS_CLIENTS;
}

static bool controller_mutation_busy(void)
{
    return s_controller_catalog.receipt_request &&
        (s_controller_catalog.receipt_state == CP_RESULT_ACCEPTED || s_controller_catalog.receipt_state == CP_RESULT_RUNNING);
}

static bool controller_matches(const app_controller_key_t *key)
{
    if (!controller_catalog_known() || key->revision != s_controller_catalog.revision) return false;
    if (key->pairing_request)
        return key->pairing_request == s_controller_catalog.pairing_request &&
            s_controller_catalog.pairing_remaining_ms &&
            !memcmp(key->id, s_controller_catalog.proposal.id, sizeof(key->id));
    for (unsigned i = 0; i < s_controller_catalog.count; ++i)
        if (!memcmp(key->id, s_controller_catalog.clients[i].id, sizeof(key->id))) return true;
    return false;
}

static void copy_controller_row(app_controller_row_t *row, const app_control_access_client_t *src,
                                uint64_t pairing_request)
{
    row->key.revision = s_controller_catalog.revision;
    row->key.pairing_request = pairing_request;
    memcpy(row->key.id, src->id, sizeof(row->key.id));
    /* Labels are supplied by the client. Keep control characters out of the
     * separately rendered identity and permission fields. */
    for (size_t i = 0; i < sizeof(row->label) - 1 && src->label[i]; ++i) {
        unsigned char c = (unsigned char)src->label[i];
        row->label[i] = c >= 32 && c != 127 ? (char)c : '?';
    }
    memcpy(row->fingerprint, src->fingerprint, sizeof(row->fingerprint) - 1);
    row->state = pairing_request ? APP_CONTROLLER_PAIRING :
        src->revoked ? APP_CONTROLLER_REVOKED : src->expired ? APP_CONTROLLER_EXPIRED :
        src->clock_unknown ? APP_CONTROLLER_TIME_UNKNOWN : src->enabled ? APP_CONTROLLER_AUTHORIZED : APP_CONTROLLER_DISABLED;
    row->duration = (app_ui_grant_duration_t)src->duration;
    row->permissions = src->permissions;
    row->legacy = src->legacy;
    row->connected = src->connected;
}

static void fill_controller_view(app_controllers_view_t *out)
{
    if (s_overlay != APP_OVERLAY_CONTROLLERS && s_overlay != APP_OVERLAY_CONTROLLER_ACCESS) return;
    app_control_access_snapshot(&s_controller_catalog);
    uint64_t wall_seconds;
    out->clock_known = app_control_runtime_wall_time(&wall_seconds);
    out->known = controller_catalog_known();
    out->uncertain = s_controller_catalog.uncertain;
    out->busy = controller_mutation_busy();
    out->selection = s_controller_selection;
    if (out->known) {
        if (s_controller_catalog.pairing_request && s_controller_catalog.pairing_remaining_ms)
            copy_controller_row(&out->rows[out->count++], &s_controller_catalog.proposal,
                                s_controller_catalog.pairing_request);
        app_remote_input_view_t lease = {0};
        app_remote_input_snapshot(s_remote_input, &lease);
        for (unsigned i = 0; i < s_controller_catalog.count; ++i) {
            app_controller_row_t *row = &out->rows[out->count++];
            copy_controller_row(row, &s_controller_catalog.clients[i], 0);
            row->controlling = lease.active && lease.lease_expires_ms > s_now_ms &&
                s_engine.state.owner == INPUT_REMOTE && !memcmp(row->key.id, lease.owner_principal, sizeof(row->key.id));
        }
    }
    const char *notice = s_controller_notice;
    if (s_controller_receipt) {
        bool matched = s_controller_receipt == s_controller_catalog.receipt_request;
        if (!matched) notice = "Submitted. No matching result yet.";
        else if (s_controller_catalog.receipt_state == CP_RESULT_APPLIED) {
            out->applied = true;
            notice = s_controller_operation == APP_UI_GRANT_REVOKE ? "Access revoked." :
                s_controller_operation == APP_UI_GRANT_REJECT ? "Pairing request rejected." : "Permissions saved and verified.";
        } else if (s_controller_catalog.receipt_state == CP_RESULT_ACCEPTED ||
                   s_controller_catalog.receipt_state == CP_RESULT_RUNNING) notice = "Saving. Waiting for the result.";
        else if (s_controller_catalog.receipt_error == CP_STORAGE_UNCERTAIN) notice = "Save outcome uncertain. Recheck the device list.";
        else {
            snprintf(out->feedback, sizeof(out->feedback), "Request failed: %s", cp_error_name(s_controller_catalog.receipt_error));
            return;
        }
        if (!matched || s_controller_catalog.receipt_state == CP_RESULT_ACCEPTED ||
            s_controller_catalog.receipt_state == CP_RESULT_RUNNING) out->busy = true;
    }
    if (notice) snprintf(out->feedback, sizeof(out->feedback), "%s", notice);
}

static void handle_controller_intent(const app_intent_t *intent)
{
    const app_controller_request_t *request = &intent->controller;
    if (!intent->gesture || intent->surface != APP_SURFACE_CONTROL || s_engine.state.owner != INPUT_LOCAL) return;
    app_control_access_snapshot(&s_controller_catalog);
    if (intent->action == APP_CONTROLLER_SELECT) {
        if (s_overlay != APP_OVERLAY_CONTROLLERS) return;
        if (!controller_matches(&request->key)) {
            s_controller_notice = "Selection changed. Choose a device again.";
            s_controller_receipt = 0;
            return;
        }
        s_controller_selection = request->key;
        s_controller_receipt = 0;
        s_controller_notice = NULL;
        open_overlay(APP_OVERLAY_CONTROLLER_ACCESS);
        return;
    }
    if (s_overlay != APP_OVERLAY_CONTROLLER_ACCESS || s_touch_gesture != intent->gesture ||
        !app_controller_key_equal(&s_controller_selection, &request->key)) return;
    if (!controller_matches(&request->key) || controller_mutation_busy()) {
        s_controller_notice = "List changed or busy. Reopen this device.";
        return;
    }
    app_control_access_request_t mutation = {.expected_revision = request->key.revision,
        .pairing_request = request->key.pairing_request, .permissions = request->permissions,
        .duration = (app_control_access_duration_t)request->duration};
    memcpy(mutation.client_id, request->key.id, sizeof(mutation.client_id));
    switch (request->operation) {
    case APP_UI_GRANT_APPROVE: mutation.action = APP_CONTROL_ACCESS_APPROVE; break;
    case APP_UI_GRANT_EDIT: mutation.action = APP_CONTROL_ACCESS_EDIT; break;
    case APP_UI_GRANT_REVOKE: mutation.action = APP_CONTROL_ACCESS_REVOKE; break;
    case APP_UI_GRANT_REJECT: mutation.action = APP_CONTROL_ACCESS_REJECT; break;
    default: return;
    }
    uint64_t ticket = 0;
    esp_err_t error = app_control_runtime_access_request(&mutation, &ticket);
    s_controller_receipt = error == ESP_OK ? ticket : 0;
    s_controller_operation = request->operation;
    s_controller_notice = error == ESP_OK && ticket ? NULL :
        error == ESP_ERR_NO_MEM ? "Request queue full. Try again." :
        "Request not accepted. Recheck the device and its status.";
}
