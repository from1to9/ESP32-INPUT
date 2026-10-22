// SPDX-License-Identifier: Apache-2.0
/* Native connection management uses copied owner identities, never labels as keys. */
#define CONNECTION_BINDINGS 20U
typedef union {
    app_api_request_t api;
    app_saved_wifi_request_t wifi;
    app_bluetooth_target_request_t bluetooth;
    struct { uint64_t generation; bool allowed; } remote;
} connection_request_t;
typedef struct {
    lv_obj_t *object;
    app_action_t action;
    connection_request_t request;
    bool admitted;
} connection_binding_t;
static UI_TASK_BSS connection_binding_t s_connection_bindings[CONNECTION_BINDINGS];
static connection_binding_t s_connection_pressed;
static size_t s_connection_count;
static lv_obj_t *s_connection_state, *s_connection_detail, *s_connection_reason;
static lv_obj_t *s_connection_allow_note;
static lv_obj_t *s_connection_actions[4], *s_connection_pages[2], *s_connection_page;
static lv_obj_t *s_saved_names[4], *s_saved_actions[4][3];

static bool api_digest_valid(const char digest[65])
{
    for (size_t i = 0; i < 64; ++i)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f') ||
              (digest[i] >= 'A' && digest[i] <= 'F'))) return false;
    return digest[64] == 0;
}
static bool target_equal(const app_bluetooth_target_key_t *a, const app_bluetooth_target_key_t *b)
{
    return a->address_type == b->address_type && a->bond_id_known == b->bond_id_known &&
        !memcmp(a->address, b->address, sizeof(a->address)) && !memcmp(a->bond_id, b->bond_id, sizeof(a->bond_id));
}
static void connection_bind(lv_obj_t *object, app_action_t action, connection_request_t request, bool admitted)
{
    if (!object) return;
    size_t index = 0;
    while (index < s_connection_count && s_connection_bindings[index].object != object) ++index;
    LV_ASSERT(index < CONNECTION_BINDINGS);
    if (index >= CONNECTION_BINDINGS) { enabled(object, false); return; }
    if (index == s_connection_count) ++s_connection_count;
    s_connection_bindings[index] = (connection_binding_t){object, action, request, admitted};
    enabled(object, admitted);
}
static void connection_press(lv_obj_t *target)
{
    memset(&s_connection_pressed, 0, sizeof(s_connection_pressed));
    for (size_t i = 0; i < s_connection_count; ++i)
        if (s_connection_bindings[i].object == target) { s_connection_pressed = s_connection_bindings[i]; return; }
}
static bool connection_equal(const connection_binding_t *a, const connection_binding_t *b)
{
    if (a->action != b->action) return false;
    if (a->action == APP_API_INPUT_ALLOW_SET)
        return a->request.remote.generation == b->request.remote.generation && a->request.remote.allowed == b->request.remote.allowed;
    if (a->action == APP_API_COMMISSIONING)
        return a->request.api.operation == b->request.api.operation && a->request.api.revision == b->request.api.revision &&
            a->request.api.page == b->request.api.page && !memcmp(a->request.api.review_digest, b->request.api.review_digest, 65);
    if (a->action == APP_WIFI_PROFILE_CONTROL)
        return a->request.wifi.operation == b->request.wifi.operation && a->request.wifi.revision == b->request.wifi.revision &&
            a->request.wifi.page == b->request.wifi.page && !memcmp(a->request.wifi.id, b->request.wifi.id, 33);
    return a->request.bluetooth.operation == b->request.bluetooth.operation &&
        a->request.bluetooth.bond_revision == b->request.bluetooth.bond_revision &&
        a->request.bluetooth.preferences_revision == b->request.bluetooth.preferences_revision &&
        target_equal(&a->request.bluetooth.target, &b->request.bluetooth.target);
}
static void connection_action(lv_obj_t *target)
{
    if (!target || target != s_connection_pressed.object || !s_connection_pressed.admitted ||
        s_view.locked || s_view.screen_off || s_wake_touch || s_touch_local_blocked ||
        s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision || s_built_overlay != s_view.overlay) return;
    const connection_binding_t *current = NULL;
    for (size_t i = 0; i < s_connection_count; ++i)
        if (s_connection_bindings[i].object == target) current = &s_connection_bindings[i];
    if (!current || !current->admitted || !connection_equal(current, &s_connection_pressed)) return;
    app_intent_t intent = {.action = current->action, .epoch = s_touch_epoch, .ui_revision = s_touch_revision,
        .gesture = s_gesture, .surface = APP_SURFACE_CONTROL};
    if (intent.action == APP_API_COMMISSIONING) intent.api = s_connection_pressed.request.api;
    else if (intent.action == APP_API_INPUT_ALLOW_SET) {
        intent.x = s_connection_pressed.request.remote.allowed ? 1 : 0;
        intent.api.revision = s_connection_pressed.request.remote.generation;
    }
    else if (intent.action == APP_WIFI_PROFILE_CONTROL) intent.wifi_profile = s_connection_pressed.request.wifi;
    else intent.bluetooth_target = s_connection_pressed.request.bluetooth;
    if ((!s_submit || !s_submit(&intent)) && s_status) label_text(s_status, "Input busy. Please try again.");
}
static void clear_connection_panel(void)
{
    s_connection_count = 0;
    memset(s_connection_bindings, 0, sizeof(s_connection_bindings));
    memset(s_connection_actions, 0, sizeof(s_connection_actions));
    memset(s_connection_pages, 0, sizeof(s_connection_pages));
    memset(s_saved_names, 0, sizeof(s_saved_names));
    memset(s_saved_actions, 0, sizeof(s_saved_actions));
    s_connection_state = s_connection_detail = s_connection_reason = s_connection_page = NULL;
    s_connection_allow_note = NULL;
}
static lv_obj_t *connection_label(lv_obj_t *body, int y, int height, uint32_t color)
{
    lv_obj_t *object = label(body, "", 0, y, 456, color);
    lv_obj_set_height(object, height); lv_label_set_long_mode(object, LV_LABEL_LONG_DOT); return object;
}
static void create_connection_panel(lv_obj_t *body, lv_obj_t *title, app_overlay_t kind)
{
    if (kind == APP_OVERLAY_API_CONNECTION) {
        label_text(title, "Web / API connection");
        s_connection_state = connection_label(body, 0, 32, INK);
        s_connection_detail = connection_label(body, 40, 64, MUTED);
        s_connection_reason = connection_label(body, 112, 88, MUTED);
        lv_obj_set_style_text_font(s_connection_reason, META_FONT, 0);
        s_connection_actions[0] = button(body, "Open USB window", 0, 208, 224, 48, APP_API_COMMISSIONING, 0);
        s_connection_actions[1] = button(body, "Close window", 232, 208, 224, 48, APP_API_COMMISSIONING, 0);
        s_connection_actions[2] = button(body, "Review imported identity", 0, 264, 456, 44, APP_MENU, APP_OVERLAY_API_REVIEW);
        s_connection_actions[3] = button(body, "Remote input / unknown", 0, 316, 456, 44, APP_API_INPUT_ALLOW_SET, 0);
        lv_obj_set_y(lv_obj_get_child(s_connection_actions[3], 0), -8);
        s_connection_allow_note = metadata(s_connection_actions[3], "", 4, 26, 448);
        lv_obj_set_style_text_align(s_connection_allow_note, LV_TEXT_ALIGN_CENTER, 0);
        button(body, "Wi-Fi settings", 0, 364, 456, 44, APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    } else if (kind == APP_OVERLAY_API_REVIEW) {
        label_text(title, "Review API identity");
        s_connection_state = connection_label(body, 0, 88, INK);
        lv_obj_set_style_text_font(s_connection_state, META_FONT, 0);
        s_connection_detail = connection_label(body, 96, 64, MUTED);
        lv_obj_set_style_text_font(s_connection_detail, META_FONT, 0);
        s_connection_reason = connection_label(body, 168, 128, INK);
        lv_obj_set_style_text_font(s_connection_reason, META_FONT, 0);
        s_connection_pages[0] = button(body, "Previous", 0, 304, 136, 48, APP_API_COMMISSIONING, 0);
        s_connection_page = label(body, "", 144, 316, 168, MUTED);
        lv_obj_set_style_text_align(s_connection_page, LV_TEXT_ALIGN_CENTER, 0);
        s_connection_pages[1] = button(body, "Next", 320, 304, 136, 48, APP_API_COMMISSIONING, 0);
        s_connection_actions[0] = button(body, "Approve identity", 0, 360, 456, 48, APP_API_COMMISSIONING, 0);
    } else if (kind == APP_OVERLAY_WIFI_PROFILES) {
        label_text(title, "Saved Wi-Fi networks");
        s_connection_state = connection_label(body, 0, 32, MUTED);
        for (int i = 0; i < 4; ++i) {
            s_saved_names[i] = label(body, "", 0, 40 + i * 64, 224, INK);
            lv_obj_set_height(s_saved_names[i], 52); lv_label_set_long_mode(s_saved_names[i], LV_LABEL_LONG_DOT);
            s_saved_actions[i][0] = button(body, "Join", 232, 40 + i * 64, 60, 52, APP_WIFI_PROFILE_CONTROL, 0);
            s_saved_actions[i][1] = button(body, "First", 300, 40 + i * 64, 68, 52, APP_WIFI_PROFILE_CONTROL, 0);
            s_saved_actions[i][2] = button(body, "Forget", 376, 40 + i * 64, 80, 52, APP_WIFI_PROFILE_CONTROL, 0);
        }
        s_connection_reason = connection_label(body, 304, 40, MUTED);
        lv_obj_set_style_text_font(s_connection_reason, META_FONT, 0);
        s_connection_pages[0] = button(body, "Previous", 0, 360, 224, 48, APP_WIFI_PROFILE_CONTROL, 0);
        s_connection_pages[1] = button(body, "Next", 232, 360, 224, 48, APP_WIFI_PROFILE_CONTROL, 0);
    } else {
        label_text(title, "Bluetooth target");
        s_connection_state = connection_label(body, 0, 56, INK);
        s_connection_detail = connection_label(body, 64, 64, MUTED);
        s_connection_reason = connection_label(body, 144, 144, MUTED);
        s_connection_actions[0] = button(body, "Select target", 0, 304, 224, 48, APP_BLUETOOTH_TARGET_CONTROL, 0);
        s_connection_actions[1] = button(body, "Move to first", 232, 304, 224, 48, APP_BLUETOOTH_TARGET_CONTROL, 0);
        button(body, "Saved Bluetooth hosts", 0, 360, 456, 48, APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    }
}
static void bind_bluetooth_row(lv_obj_t *row, const app_bond_view_t *host)
{
    connection_request_t request = {.bluetooth = {.operation = APP_BLUETOOTH_TARGET_DETAILS,
        .bond_revision = host->bond_revision, .preferences_revision = host->preferences_revision, .target = host->target}};
    connection_bind(row, APP_BLUETOOTH_TARGET_CONTROL, request, host->target_known);
}
static bool review_text_fits(const char *text_value, int height)
{
    lv_point_t measured;
    lv_text_get_size(&measured, text_value, META_FONT, 0, 0, 456, LV_TEXT_FLAG_NONE);
    return measured.y <= height;
}
static void render_connection_panel(void)
{
    if (s_view.overlay == APP_OVERLAY_API_CONNECTION || s_view.overlay == APP_OVERLAY_API_REVIEW) {
        const app_api_view_t *api = &s_view.api;
        bool review = api->known && api->review_pending && api->review_complete && api_digest_valid(api->review_digest) &&
            api->review_page_count && api->review_page_count <= APP_API_REVIEW_PAGES && api->review_page < api->review_page_count &&
            review_text_fits(api->review_host, 88) && review_text_fits(api->review_digest, 64);
        for (unsigned i = 0; review && i < api->review_page_count; ++i)
            review = review_text_fits(api->review_pages[i], 128);
        if (s_view.overlay == APP_OVERLAY_API_CONNECTION) {
            label_text(s_connection_state, !api->known ? "Web/API status unavailable" : api->ready && api->tls ? "HTTPS service ready" :
                api->commissioning_open ? "Local USB enrollment open" : "HTTPS service not ready");
            label_text(s_connection_detail, api->known && api->ready && api->tls && *api->url ? api->url :
                "No confirmed HTTPS address.\nConnect this device to your existing Wi-Fi.");
            if (api->remaining_known && api->commissioning_open)
                label_format(s_connection_reason, "USB window: %us left\nImport the identity package over local USB.\nReview permissions on this device before approval.\n%.96s", api->remaining_seconds, api->reason);
            else label_format(s_connection_reason, "Open a local USB enrollment window, then import the identity package.\nWi-Fi hotspot setup does not authorize API access.\n%.96s", api->reason);
            connection_request_t request = {.api = {.operation = APP_API_WINDOW_OPEN, .revision = api->revision}};
            connection_bind(s_connection_actions[0], APP_API_COMMISSIONING, request, api->known && api->can_open);
            request.api.operation = APP_API_WINDOW_CLOSE;
            connection_bind(s_connection_actions[1], APP_API_COMMISSIONING, request, api->known && api->can_close);
            enabled(s_connection_actions[2], review);
            text(s_connection_actions[3], !api->remote_input_generation ? "Remote input / unknown" :
                api->remote_input_allowed ? "Block remote input" : "Allow remote input");
            label_text(s_connection_allow_note, !api->remote_input_generation ? "Status unavailable" :
                api->remote_input_allowed ? "Remote input allowed" : "Blocked; enabling returns to main screen");
            foreground(s_connection_allow_note, api->remote_input_generation && api->remote_input_allowed ? ON_ACCENT : MUTED, 0);
            connection_request_t allowance = {.remote = {.generation = api->remote_input_generation, .allowed = !api->remote_input_allowed}};
            connection_bind(s_connection_actions[3], APP_API_INPUT_ALLOW_SET, allowance, api->remote_input_generation != 0);
            selected(s_connection_actions[3], api->remote_input_generation && api->remote_input_allowed);
        } else {
            label_text(s_connection_state, review ? api->review_host : "Review unavailable or incomplete");
            label_text(s_connection_detail, review ? api->review_digest : "A complete local proposal is required.");
            label_text(s_connection_reason, review ? api->review_pages[api->review_page] : "Close this page and import a fresh identity package.");
            if (review) label_format(s_connection_page, "%u / %u", api->review_page + 1, api->review_page_count);
            else label_text(s_connection_page, "--");
            for (unsigned i = 0; i < 2; ++i) {
                connection_request_t request = {.api = {.operation = APP_API_REVIEW_PAGE, .revision = api->revision,
                    .page = i ? api->review_page + 1 : api->review_page ? api->review_page - 1 : 0}};
                memcpy(request.api.review_digest, api->review_digest, 65);
                connection_bind(s_connection_pages[i], APP_API_COMMISSIONING, request, review &&
                    (i ? api->review_page + 1 < api->review_page_count : api->review_page > 0));
            }
            connection_request_t request = {.api = {.operation = APP_API_REVIEW_APPROVE, .revision = api->revision, .page = api->review_page}};
            memcpy(request.api.review_digest, api->review_digest, 65);
            connection_bind(s_connection_actions[0], APP_API_COMMISSIONING, request, review && api->can_approve && api->review_page + 1 == api->review_page_count);
        }
    } else if (s_view.overlay == APP_OVERLAY_WIFI_PROFILES) {
        const app_saved_wifi_view_t *list = &s_view.saved_wifi;
        bool known = list->known && !list->uncertain && list->count <= APP_SAVED_WIFI_ROWS && list->page < 2;
        label_text(s_connection_state, list->pending ? "Updating saved networks..." : list->uncertain ? "Saved network state uncertain" :
            !known ? "Saved networks unavailable" : !list->count ? "No saved networks" : "Join, prioritize or forget a saved network");
        label_text(s_connection_reason, list->reason);
        for (size_t i = 0; i < 4; ++i) {
            size_t index = (size_t)list->page * 4 + i;
            bool present = known && index < list->count;
            visible(s_saved_names[i], present);
            for (unsigned j = 0; j < 3; ++j) visible(s_saved_actions[i][j], present);
            if (!present) {
                for (unsigned j = 0; j < 3; ++j)
                    connection_bind(s_saved_actions[i][j], APP_WIFI_PROFILE_CONTROL, (connection_request_t){0}, false);
                continue;
            }
            const app_saved_wifi_row_t *row = &list->rows[index];
            label_format(s_saved_names[i], "%.128s\n%s / priority %u", row->label, row->current ? "Current" : row->enabled ? "Saved" : "Disabled", row->priority);
            connection_request_t request = {.wifi = {.operation = APP_SAVED_WIFI_JOIN, .revision = list->revision, .page = list->page}};
            memcpy(request.wifi.id, row->id, sizeof(request.wifi.id));
            connection_bind(s_saved_actions[i][0], APP_WIFI_PROFILE_CONTROL, request, !list->pending && *row->id && row->can_join);
            request.wifi.operation = APP_SAVED_WIFI_PREFER;
            connection_bind(s_saved_actions[i][1], APP_WIFI_PROFILE_CONTROL, request, !list->pending && *row->id && row->can_prefer);
            request.wifi.operation = APP_SAVED_WIFI_FORGET_REQUEST;
            connection_bind(s_saved_actions[i][2], APP_WIFI_PROFILE_CONTROL, request, !list->pending && *row->id && row->can_forget);
        }
        for (unsigned i = 0; i < 2; ++i) {
            connection_request_t request = {.wifi = {.operation = APP_SAVED_WIFI_PAGE, .revision = list->revision,
                .page = i ? list->page + 1 : list->page ? list->page - 1 : 0}};
            connection_bind(s_connection_pages[i], APP_WIFI_PROFILE_CONTROL, request, known && !list->pending &&
                (i ? (list->page + 1) * 4 < list->count : list->page > 0));
        }
    } else if (s_view.overlay == APP_OVERLAY_BLUETOOTH_TARGET) {
        const app_bluetooth_target_view_t *target = &s_view.bluetooth_target;
        label_text(s_connection_state, target->known ? target->name : "Saved target unavailable");
        label_format(s_connection_detail, "%.48s\n%s", target->identity, target->uncertain ? "Preference outcome uncertain" :
            target->pending ? "Saving preference..." : target->selected ? "Selected target" : "Saved host");
        label_format(s_connection_reason, "Connect from this host's Bluetooth settings.\nThis device cannot dial or detect nearby hosts.\n%.96s", target->reason);
        connection_request_t request = {.bluetooth = {.operation = APP_BLUETOOTH_TARGET_SELECT,
            .bond_revision = target->bond_revision, .preferences_revision = target->preferences_revision, .target = target->target}};
        connection_bind(s_connection_actions[0], APP_BLUETOOTH_TARGET_CONTROL, request,
            target->known && !target->pending && !target->uncertain && target->can_select);
        request.bluetooth.operation = APP_BLUETOOTH_TARGET_PREFER;
        connection_bind(s_connection_actions[1], APP_BLUETOOTH_TARGET_CONTROL, request,
            target->known && !target->pending && !target->uncertain && target->can_prefer);
    }
}
static void sanitize_connections(void)
{
    app_api_view_t *api = &s_view.api;
    if (bounded_length(api->review_digest, sizeof(api->review_digest)) == sizeof(api->review_digest) ||
        bounded_length(api->review_host, sizeof(api->review_host)) == sizeof(api->review_host)) api->review_complete = false;
    api->url[255] = api->reason[96] = api->review_digest[64] = api->review_host[128] = 0;
    for (size_t i = 0; i < APP_API_REVIEW_PAGES; ++i) {
        if (bounded_length(api->review_pages[i], sizeof(api->review_pages[i])) == sizeof(api->review_pages[i])) api->review_complete = false;
        api->review_pages[i][256] = 0;
    }
    s_view.saved_wifi.reason[96] = 0;
    for (size_t i = 0; i < APP_SAVED_WIFI_ROWS; ++i) {
        app_saved_wifi_row_t *row = &s_view.saved_wifi.rows[i];
        if (bounded_length(row->id, sizeof(row->id)) == sizeof(row->id)) row->can_join = row->can_prefer = row->can_forget = false;
        row->id[32] = row->label[128] = 0;
    }
    s_view.bluetooth_target.name[48] = s_view.bluetooth_target.identity[48] = s_view.bluetooth_target.reason[96] = 0;
}
