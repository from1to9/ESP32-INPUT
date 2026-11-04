// SPDX-License-Identifier: Apache-2.0
/* Included by app_ui.c. The LVGL owner holds only the displayed selection and
 * unsaved permission choices. The coordinator submits mutations to the owner. */
enum {
    CONTROLLER_ROW_FIRST = 0,
    CONTROLLER_PERMISSION_FIRST = 10,
    CONTROLLER_DURATION_FIRST = 20,
    CONTROLLER_SAVE = 30, CONTROLLER_REMOVE
};
static lv_obj_t *s_controller_rows[APP_UI_CONTROLLER_ROWS], *s_controller_summary;
static lv_obj_t *s_controller_name, *s_controller_state, *s_controller_fingerprint;
static lv_obj_t *s_controller_permissions[3], *s_controller_durations[3];
static lv_obj_t *s_controller_feedback, *s_controller_save, *s_controller_remove;

typedef enum { CONTROLLER_EDITING, CONTROLLER_REVIEW_SAVE, CONTROLLER_REVIEW_REMOVE } controller_stage_t;
static controller_stage_t s_controller_stage;
static app_controller_key_t s_controller_draft_key, s_controller_pressed;
static uint8_t s_controller_bits;
static app_ui_grant_duration_t s_controller_duration;
static const char *s_controller_local_error;
static lv_obj_t *s_controller_permission_title, *s_controller_duration_title, *s_controller_review;

static void create_controller_panel(lv_obj_t *body, lv_obj_t *title, app_overlay_t kind)
{
    if (kind == APP_OVERLAY_CONTROLLERS) {
        label_text(title, "Browser controllers");
        s_controller_summary = connection_label(body, 0, 44, MUTED);
        for (unsigned i = 0; i < sizeof(s_controller_rows) / sizeof(s_controller_rows[0]); ++i) {
            lv_obj_t *row = button(body, "", 0, 48 + 60 * (int)i, 456, 54,
                APP_CONTROLLER_SELECT, CONTROLLER_ROW_FIRST + (int)i);
            lv_obj_t *caption = lv_obj_get_child(row, 0);
            lv_obj_set_height(caption, 42);
            lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_width(caption, 428);
            lv_obj_center(caption);
            s_controller_rows[i] = row;
        }
        button(body, "Back to Web/API", 0, 360, 456, 48, APP_MENU, APP_OVERLAY_API_CONNECTION);
        return;
    }
    s_controller_draft_key = (app_controller_key_t){0};
    s_controller_local_error = NULL;
    s_controller_stage = CONTROLLER_EDITING;
    label_text(title, "Controller access");
    s_controller_name = heading(body, "", 0, 0, 456);
    single_line(s_controller_name);
    s_controller_state = metadata(body, "", 0, 32, 456);
    s_controller_fingerprint = connection_label(body, 56, 56, MUTED);
    lv_obj_set_style_text_font(s_controller_fingerprint, META_FONT, 0);
    s_controller_permission_title = metadata(body, "Permissions", 0, 118, 456);
    const char *permissions[] = {"Keyboard\n& mouse", "Management", "Updates"};
    const char *durations[] = {"6 hours", "24 hours", "Until revoked"};
    for (int i = 0; i < 3; ++i) {
        s_controller_permissions[i] = button(body, permissions[i], 156 * i, 140, 144, 48,
            APP_CONTROLLER_EDIT, CONTROLLER_PERMISSION_FIRST + i);
        lv_obj_t *caption = lv_obj_get_child(s_controller_permissions[i], 0);
        lv_obj_set_style_text_font(caption, META_FONT, 0);
        if (!i) { lv_obj_set_height(caption, 36); lv_obj_center(caption); }
        s_controller_durations[i] = button(body, durations[i], 156 * i, 222, 144, 44,
            APP_CONTROLLER_EDIT, CONTROLLER_DURATION_FIRST + i);
    }
    s_controller_duration_title = metadata(body, "Grant duration", 0, 198, 456);
    s_controller_review = connection_label(body, 112, 194, INK);
    lv_obj_set_style_text_font(s_controller_review, META_FONT, 0);
    s_controller_feedback = connection_label(body, 271, 39, MUTED);
    lv_obj_set_style_text_font(s_controller_feedback, META_FONT, 0);
    s_controller_save = button(body, "Approve", 0, 310, 224, 44, APP_CONTROLLER_EDIT, CONTROLLER_SAVE);
    s_controller_remove = button(body, "Reject", 232, 310, 224, 44, APP_CONTROLLER_EDIT, CONTROLLER_REMOVE);
    button(body, "Back to controllers", 0, 364, 456, 44, APP_MENU, APP_OVERLAY_CONTROLLERS);
}

static const app_controller_row_t *controller_choice(int value)
{
    const app_controllers_view_t *view = &s_view.controllers;
    if (!view->known || view->uncertain || view->count > APP_UI_CONTROLLER_ROWS) return NULL;
    if (s_view.overlay == APP_OVERLAY_CONTROLLERS)
        return value >= 0 && (unsigned)value < view->count ? &view->rows[value] : NULL;
    if (s_view.overlay == APP_OVERLAY_CONTROLLER_ACCESS)
        for (unsigned i = 0; i < view->count; ++i)
            if (app_controller_key_equal(&view->selection, &view->rows[i].key)) return &view->rows[i];
    return NULL;
}

static const char *controller_status(const app_controller_row_t *row)
{
    switch (row->state) {
    case APP_CONTROLLER_PAIRING: return "Pairing request";
    case APP_CONTROLLER_REVOKED: return "Revoked";
    case APP_CONTROLLER_EXPIRED: return "Expired";
    case APP_CONTROLLER_TIME_UNKNOWN: return "Time unknown / access paused";
    case APP_CONTROLLER_AUTHORIZED: return "Authorized";
    default: return "Access disabled";
    }
}

static const char *controller_session(const app_controller_row_t *row)
{
    return row->controlling ? " / Controlling input" : row->connected ? " / Signed in" : "";
}

static const char *controller_duration_label(app_ui_grant_duration_t value)
{
    switch (value) {
    case APP_UI_GRANT_6H: return "6 hours";
    case APP_UI_GRANT_24H: return "24 hours";
    case APP_UI_GRANT_PERMANENT: return "Until revoked";
    }
    return "Unknown term";
}

static void controller_press(lv_obj_t *target)
{
    if (s_view.overlay != APP_OVERLAY_CONTROLLERS && s_view.overlay != APP_OVERLAY_CONTROLLER_ACCESS) return;
    s_controller_pressed = (app_controller_key_t){0};
    for (size_t i = s_core_bindings; i < s_binding_count; ++i) {
        const binding_t *b = &s_bindings[i];
        if (b->object != target || (b->action != APP_CONTROLLER_SELECT && b->action != APP_CONTROLLER_EDIT)) continue;
        const app_controller_row_t *row = controller_choice(b->value);
        if (row) s_controller_pressed = row->key;
        return;
    }
}

static void render_controllers(void)
{
    const app_controllers_view_t *view = &s_view.controllers;
    if (s_view.overlay == APP_OVERLAY_CONTROLLERS) {
        const char *summary = !view->known || view->uncertain ? "Controller list unavailable" :
            view->busy ? "A controller request is in progress." :
            view->count ? "Choose a device to review its identity and permissions." : "No controllers. Request pairing in the browser.";
        label_text(s_controller_summary, *view->feedback ? view->feedback : summary);
        for (unsigned i = 0; i < APP_UI_CONTROLLER_ROWS; ++i) {
            const app_controller_row_t *row = controller_choice((int)i);
            visible(s_controller_rows[i], row != NULL);
            if (!row) continue;
            char caption[125];
            snprintf(caption, sizeof(caption), "%.32s\n%s%s  /  ID %02x%02x%02x%02x",
                *row->label ? row->label : "Unnamed controller", controller_status(row), controller_session(row),
                row->key.id[0], row->key.id[1], row->key.id[2], row->key.id[3]);
            text(s_controller_rows[i], caption);
            enabled(s_controller_rows[i], row->key.revision != 0);
        }
        return;
    }
    if (s_view.overlay != APP_OVERLAY_CONTROLLER_ACCESS) return;
    const app_controller_row_t *row = controller_choice(0);
    bool pairing = row && row->key.pairing_request;
    if (row && !app_controller_key_equal(&s_controller_draft_key, &row->key)) {
        s_controller_draft_key = row->key;
        s_controller_bits = pairing ? APP_UI_GRANT_INPUT : row->permissions;
        s_controller_duration = pairing ? APP_UI_GRANT_6H : row->duration;
        s_controller_stage = CONTROLLER_EDITING;
        s_controller_local_error = NULL;
    }
    bool editable = row && !view->busy && s_view.local_access == APP_LOCAL_NORMAL;
    bool fingerprint = row && api_digest_valid(row->fingerprint);
    label_text(s_controller_name, row ? (*row->label ? row->label : "Unnamed controller") :
        view->applied ? "Request complete" : !view->known || view->uncertain ? "Controller unavailable" : "Selection changed");
    if (row) label_format(s_controller_state, "%s%s%s%s%s", controller_status(row), controller_session(row), row->legacy ? " / USB import" : "",
        pairing ? "" : " / ", pairing ? "" : controller_duration_label(row->duration));
    else label_text(s_controller_state, "Return to the list for current information.");
    if (fingerprint) label_format(s_controller_fingerprint, "Compare the browser fingerprint\n%.32s\n%.32s",
        row->fingerprint, row->fingerprint + 32);
    else if (row && !pairing) {
        char identity[33];
        for (unsigned i = 0; i < sizeof(row->key.id); ++i) snprintf(identity + i * 2U, 3, "%02x", row->key.id[i]);
        label_format(s_controller_fingerprint, "Saved controller ID\n%s", identity);
    } else label_text(s_controller_fingerprint, pairing ? "Fingerprint unavailable. Approval is blocked." : "");
    bool review = row && s_controller_stage != CONTROLLER_EDITING;
    visible(s_controller_permission_title, !review && row);
    visible(s_controller_duration_title, !review && row);
    visible(s_controller_review, review);
    visible(s_controller_feedback, !review);
    for (unsigned i = 0; i < 3; ++i) {
        visible(s_controller_permissions[i], !review && row);
        visible(s_controller_durations[i], !review && row);
        selected(s_controller_permissions[i], (s_controller_bits & (1U << i)) != 0);
        selected(s_controller_durations[i], s_controller_duration == (app_ui_grant_duration_t)i);
        enabled(s_controller_permissions[i], editable);
        enabled(s_controller_durations[i], editable);
    }
    bool review_complete = true;
    bool removing = s_controller_stage == CONTROLLER_REVIEW_REMOVE;
    if (review) {
        if (removing) label_text(s_controller_review, pairing ?
            "Reject this pairing request? No permissions will be granted." :
            "Revoke all access for this controller? It must be authorized again before it can control the device.");
        else {
            static UI_TASK_BSS char review_text[512];
            int length = snprintf(review_text, sizeof(review_text), "Duration: %s\n%s%s%s%s%s",
                controller_duration_label(s_controller_duration),
                s_controller_bits & APP_UI_GRANT_INPUT ? "Keyboard & mouse after taking control.\n" : "",
                s_controller_bits & APP_UI_GRANT_MANAGE ? "Management: edit settings and connections, configure Wi-Fi and read diagnostics. No mic or recording.\n" : "",
                s_controller_bits & APP_UI_GRANT_UPDATE ?
                    "Updates: check, stage and activate signed firmware from enrolled sources and restart the device. Up to 10 minutes per job. Maintenance and health checks still apply.\n" : "",
                row->legacy ? "Replaces legacy scopes, including any capture grants.\n" : "",
                "Saving does not resume control or jobs.");
            review_complete = length > 0 && (size_t)length < sizeof(review_text) && review_text_fits(review_text, 194);
            label_text(s_controller_review, review_complete ? review_text : "Cannot show the complete permissions. Approval is unavailable.");
        }
    }
    label_text(s_controller_feedback, s_controller_local_error ? s_controller_local_error : *view->feedback ? view->feedback :
        view->busy ? "Waiting for the access service." : !row ? "Open the controller list to refresh." :
        !s_controller_bits ? "Choose at least one permission." :
        !view->clock_known && s_controller_duration != APP_UI_GRANT_PERMANENT && !removing ?
        "Timed grants need trusted time. Access has not changed." : review ? "Confirm only if the device and choices match." :
        "Highlighted permissions will be granted.");
    visible(s_controller_save, row != NULL);
    visible(s_controller_remove, row != NULL);
    text(s_controller_save, !review ? "Review permissions" : removing ? pairing ? "Reject request" : "Revoke access" :
        pairing ? "Approve controller" : "Save permissions");
    text(s_controller_remove, review ? "Edit choices" : pairing ? "Reject..." : "Revoke...");
    bool valid_term = s_controller_duration >= APP_UI_GRANT_6H && s_controller_duration <= APP_UI_GRANT_PERMANENT;
    enabled(s_controller_save, editable && review_complete && (removing || (s_controller_bits && !(s_controller_bits & ~7U) &&
        valid_term && (view->clock_known || s_controller_duration == APP_UI_GRANT_PERMANENT) && (!pairing || fingerprint))));
    enabled(s_controller_remove, editable && (review || row->state != APP_CONTROLLER_REVOKED));
}

static void controller_action(const binding_t *binding)
{
    const app_controller_row_t *row = controller_choice(binding->value);
    if (!row || !app_controller_key_equal(&row->key, &s_controller_pressed) ||
        s_built_overlay != s_view.overlay || s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
        s_touch_local_blocked || s_wake_touch || s_view.screen_off || s_view.locked ||
        lv_obj_has_state(binding->object, LV_STATE_DISABLED)) return;
    app_intent_t intent = {.action = binding->action, .epoch = s_touch_epoch, .ui_revision = s_touch_revision,
        .gesture = s_gesture, .surface = APP_SURFACE_CONTROL, .controller.key = row->key};
    if (binding->action == APP_CONTROLLER_EDIT) {
        if (s_view.local_access != APP_LOCAL_NORMAL || s_view.controllers.busy) return;
        int value = binding->value;
        if (value >= CONTROLLER_PERMISSION_FIRST && value < CONTROLLER_PERMISSION_FIRST + 3)
            s_controller_bits ^= (uint8_t)(1U << (value - CONTROLLER_PERMISSION_FIRST));
        else if (value >= CONTROLLER_DURATION_FIRST && value < CONTROLLER_DURATION_FIRST + 3)
            s_controller_duration = (app_ui_grant_duration_t)(value - CONTROLLER_DURATION_FIRST);
        else if (value == CONTROLLER_REMOVE)
            s_controller_stage = s_controller_stage == CONTROLLER_EDITING ? CONTROLLER_REVIEW_REMOVE : CONTROLLER_EDITING;
        else if (value == CONTROLLER_SAVE && s_controller_stage == CONTROLLER_EDITING)
            s_controller_stage = CONTROLLER_REVIEW_SAVE;
        else if (value == CONTROLLER_SAVE) {
            bool remove = s_controller_stage == CONTROLLER_REVIEW_REMOVE;
            intent.controller.operation = remove ? (row->key.pairing_request ? APP_UI_GRANT_REJECT : APP_UI_GRANT_REVOKE) :
                (row->key.pairing_request ? APP_UI_GRANT_APPROVE : APP_UI_GRANT_EDIT);
            intent.controller.permissions = remove ? 0 : s_controller_bits;
            intent.controller.duration = s_controller_duration;
            if (s_submit && s_submit(&intent)) s_controller_local_error = NULL;
            else s_controller_local_error = "Could not submit. Try again.";
            s_controller_stage = CONTROLLER_EDITING;
        }
        render_controllers();
        return;
    }
    if (!s_submit || !s_submit(&intent)) label_text(s_controller_summary, "Could not open this device. Try again.");
}
