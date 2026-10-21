// SPDX-License-Identifier: Apache-2.0
/* One immutable remote proposal rendered and confirmed by the native owner. */
static bool s_built_remote_review, s_remote_clock_fault, s_remote_pressed_admitted;
static uint32_t s_remote_clock_tick;
static uint64_t s_remote_clock_ms;
static lv_obj_t *s_remote_id, *s_remote_digest, *s_remote_binding, *s_remote_pressed_target;
static UI_TASK_BSS app_remote_review_view_t s_remote_pressed;

static bool remote_identity_equal(const app_remote_review_request_t *a, const app_remote_review_request_t *b)
{
    return a->management_generation == b->management_generation && a->resource_revision == b->resource_revision &&
        a->expires_ms == b->expires_ms && !memcmp(a->operation, b->operation, sizeof(a->operation)) &&
        !memcmp(a->approval_id, b->approval_id, sizeof(a->approval_id)) &&
        !memcmp(a->action_digest, b->action_digest, sizeof(a->action_digest));
}
static bool remote_hex(const char *value, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
              (value[i] >= 'A' && value[i] <= 'F'))) return false;
    return value[length] == 0;
}
static void observe_remote_review_clock(const app_remote_review_view_t *view)
{
    bool replaced = !remote_identity_equal(&view->request, &s_view.remote_review.request);
    if (replaced) s_remote_clock_fault = false;
    else if (view->clock_known && view->now_ms < s_remote_clock_ms) s_remote_clock_fault = true;
    if (replaced || view->now_ms != s_remote_clock_ms) {
        s_remote_clock_ms = view->now_ms;
        s_remote_clock_tick = lv_tick_get();
    }
}
static bool remote_text_fits(const char *value, int height)
{
    lv_point_t size;
    lv_text_get_size(&size, value, META_FONT, 0, 0, 456, LV_TEXT_FLAG_NONE);
    return size.y <= height;
}
static void remote_scope_text(char output[512], const app_remote_review_view_t *view)
{
    if (!strcmp(view->request.operation, "preferences.reset"))
        snprintf(output, 512, "Reset group: %s\n%s", view->reset_group, view->details);
    else if (!strcmp(view->request.operation, "device.factory_reset.request"))
        snprintf(output, 512, "Retained manifest:\n%s\n%s", view->retained_manifest_digest, view->details);
    else snprintf(output, 512, "%s", view->details);
}
static bool remote_review_complete(void)
{
    const app_remote_review_view_t *view = &s_view.remote_review;
    const app_remote_review_request_t *request = &view->request;
    bool restart = !strcmp(request->operation, "system.restart");
    bool preferences = !strcmp(request->operation, "preferences.reset");
    bool reset = !strcmp(request->operation, "device.factory_reset.request");
    bool bluetooth = !strcmp(request->operation, "bluetooth.pairing.open") ||
        !strcmp(request->operation, "bluetooth.bonds.forget_all") || !strcmp(request->operation, "bluetooth.bond.forget");
    if (!view->pending || !view->complete || (!restart && !preferences && !reset && !bluetooth) ||
        !request->management_generation || !request->resource_revision || !request->expires_ms ||
        !remote_hex(request->approval_id, 32) || !remote_hex(request->action_digest, 64) || !*view->details ||
        (preferences && !*view->reset_group) || (reset && !remote_hex(view->retained_manifest_digest, 64))) return false;
    char scope[512]; remote_scope_text(scope, view);
    return remote_text_fits(scope, 124);
}
static bool remote_review_live(void)
{
    const app_remote_review_view_t *view = &s_view.remote_review;
    uint64_t elapsed = lv_tick_elaps(s_remote_clock_tick);
    return view->clock_known && !s_remote_clock_fault && view->now_ms <= UINT64_MAX - elapsed &&
        view->now_ms + elapsed < view->request.expires_ms;
}
static void remote_review_press(lv_obj_t *target)
{
    s_remote_pressed_target = NULL;
    memset(&s_remote_pressed, 0, sizeof(s_remote_pressed));
    if (s_view.overlay != APP_OVERLAY_CONFIRM || s_view.confirmation != APP_CONFIRM_REMOTE || !s_built_remote_review) return;
    s_remote_pressed_target = target;
    s_remote_pressed = s_view.remote_review;
    s_remote_pressed_admitted = remote_review_complete() && remote_review_live() && s_view.remote_review.can_confirm;
}
static void remote_review_action(lv_obj_t *target, bool confirm)
{
    if (!target || target != s_remote_pressed_target || !s_built_remote_review ||
        s_built_overlay != APP_OVERLAY_CONFIRM || s_view.overlay != APP_OVERLAY_CONFIRM ||
        s_view.confirmation != APP_CONFIRM_REMOTE || s_view.locked || s_view.screen_off || s_wake_touch ||
        s_touch_local_blocked || s_touch_epoch != s_view.epoch || s_touch_revision != s_view.ui_revision ||
        !remote_identity_equal(&s_remote_pressed.request, &s_view.remote_review.request) ||
        memcmp(s_remote_pressed.reset_group, s_view.remote_review.reset_group, sizeof(s_remote_pressed.reset_group)) ||
        memcmp(s_remote_pressed.retained_manifest_digest, s_view.remote_review.retained_manifest_digest, sizeof(s_remote_pressed.retained_manifest_digest)) ||
        memcmp(s_remote_pressed.details, s_view.remote_review.details, sizeof(s_remote_pressed.details))) return;
    if (confirm && (!s_remote_pressed_admitted || !remote_review_complete() || !remote_review_live() ||
        !s_view.remote_review.can_confirm)) return;
    app_intent_t intent = {.action = APP_CONFIRM, .x = confirm ? 1 : 0, .epoch = s_touch_epoch,
        .ui_revision = s_touch_revision, .gesture = s_gesture, .surface = APP_SURFACE_CONTROL,
        .remote_review = s_remote_pressed.request};
    if ((!s_submit || !s_submit(&intent)) && s_status) label_text(s_status, "Input busy. Please try again.");
}
static void clear_remote_review_panel(void)
{
    s_remote_id = s_remote_digest = s_remote_binding = s_remote_pressed_target = NULL;
    memset(&s_remote_pressed, 0, sizeof(s_remote_pressed));
    s_remote_pressed_admitted = false;
}
static lv_obj_t *remote_review_label(lv_obj_t *body, int y, int height)
{
    lv_obj_t *object = label(body, "", 0, y, 456, MUTED);
    lv_obj_set_style_text_font(object, META_FONT, 0);
    lv_obj_set_height(object, height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}
static void create_remote_review(lv_obj_t *body, lv_obj_t *title)
{
    label_text(title, "Remote action review");
    s_confirm_title = label(body, "", 0, 0, 456, INK);
    lv_obj_set_style_text_font(s_confirm_title, BODY_FONT, 0);
    lv_obj_set_height(s_confirm_title, 32);
    s_remote_id = remote_review_label(body, 40, 48);
    s_remote_digest = remote_review_label(body, 92, 64);
    s_remote_binding = remote_review_label(body, 164, 56);
    s_confirm_detail = remote_review_label(body, 228, 124);
    button(body, "Cancel", 0, 360, 224, 48, APP_CONFIRM, 0);
    s_confirm_accept = button(body, "Confirm locally", 232, 360, 224, 48, APP_CONFIRM, 1);
    selected(s_confirm_accept, true);
}
static void render_remote_review(void)
{
    if (!s_built_remote_review) return;
    const app_remote_review_view_t *view = &s_view.remote_review;
    bool complete = remote_review_complete();
    label_text(s_confirm_title, complete ? view->request.operation : "Remote review incomplete");
    if (complete) {
        label_format(s_remote_id, "Approval ID:\n%s", view->request.approval_id);
        label_format(s_remote_digest, "Action digest:\n%s", view->request.action_digest);
        label_format(s_remote_binding, "Original revision: %llu\nExpires at: %llu ms / %s",
            (unsigned long long)view->request.resource_revision, (unsigned long long)view->request.expires_ms,
            remote_review_live() ? "valid" : "expired / clock unknown");
        char scope[512]; remote_scope_text(scope, view); label_text(s_confirm_detail, scope);
    } else {
        label_text(s_remote_id, "Informational only; confirmation is disabled.");
        label_text(s_remote_digest, "A complete immutable request is required.");
        label_text(s_remote_binding, "No action has been approved.");
        label_text(s_confirm_detail, "Cancel this review and request complete details again.");
    }
    enabled(s_confirm_accept, complete && remote_review_live() && view->can_confirm);
}
static void sanitize_remote_review(void)
{
    app_remote_review_view_t *view = &s_view.remote_review;
    char *strings[] = {view->request.operation, view->request.approval_id, view->request.action_digest,
        view->reset_group, view->retained_manifest_digest, view->details};
    const size_t sizes[] = {49, 33, 65, 33, 65, 257};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (bounded_length(strings[i], sizes[i]) == sizes[i]) view->complete = false;
        strings[i][sizes[i] - 1] = 0;
    }
}
