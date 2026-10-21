// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_presenter.c"

/* Production is included to inspect owned storage after cleanup. Tests never
 * reset or patch its state: each named case runs in a fresh process. */
struct lv_timer_t { lv_timer_cb_t callback; };
static struct lv_timer_t s_test_timer;
static bool s_fail_timer;
static unsigned s_timer_creations, s_renders;
static _Thread_local unsigned s_lock_depth;
static _Thread_local bool s_lvgl_owner;
static atomic_int_fast64_t s_clock_us;
static app_view_t s_rendered;
static char s_rendered_message[APP_PRESENTER_MESSAGE_BYTES];
static char s_rendered_target[APP_PRESENTER_TARGET_BYTES];
static void (*s_render_hook)(const app_view_t *view);
struct lv_display_t {
    int32_t width, height, x, y;
    lv_display_rotation_t rotation;
    lv_event_cb_t callback;
};
struct lv_event_t { lv_display_t *display; lv_event_code_t code; };
static lv_display_t s_test_display = {.width = 480, .height = 480};
static app_ui_capture_indicator_t s_test_indicator;
static board_display_receipt_t s_board;
static esp_err_t s_begin_error, s_end_error;
static unsigned s_begins, s_ends, s_invalidates;
static unsigned s_repaints;
struct lv_obj_t { unsigned unused; };
static lv_obj_t s_test_screen;
lv_obj_t *lv_display_get_screen_active(lv_display_t *display)
{ assert(s_lvgl_owner && s_lock_depth == 0 && display == &s_test_display); return &s_test_screen; }
void lv_obj_invalidate_area(const lv_obj_t *object, const lv_area_t *area)
{ assert(s_lvgl_owner && s_lock_depth == 0 && object == &s_test_screen && area); ++s_repaints; }

void *lv_event_get_target(lv_event_t *event) { return event->display; }
lv_event_code_t lv_event_get_code(lv_event_t *event) { return event->code; }
int32_t lv_display_get_horizontal_resolution(const lv_display_t *display) { return display->width; }
int32_t lv_display_get_vertical_resolution(const lv_display_t *display) { return display->height; }
int32_t lv_display_get_offset_x(const lv_display_t *display) { return display->x; }
int32_t lv_display_get_offset_y(const lv_display_t *display) { return display->y; }
lv_display_rotation_t lv_display_get_rotation(lv_display_t *display) { return display->rotation; }
void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback, lv_event_code_t filter, void *data)
{
    assert(s_lvgl_owner && s_lock_depth == 0 && filter == LV_EVENT_ALL && !data);
    display->callback = callback;
}
app_ui_capture_indicator_t app_ui_capture_indicator(void)
{
    assert(s_lvgl_owner && s_lock_depth == 0);
    return s_test_indicator;
}
esp_err_t board_display_transfer_begin(const board_display_token_t *token, const board_display_rect_t *required)
{
    assert(s_lvgl_owner && s_lock_depth == 0);
    ++s_begins;
    s_board.token = *token;
    s_board.required = *required;
    s_board.refresh_sealed = s_board.coverage_complete = s_board.indicator_transfer_confirmed = false;
    s_board.transfers = s_board.completed_transfers = 0;
    s_board.error = s_begin_error;
    return s_begin_error;
}
esp_err_t board_display_transfer_end(const board_display_token_t *token)
{
    assert(s_lvgl_owner && s_lock_depth == 0);
    assert(token->refresh_sequence == s_board.token.refresh_sequence);
    ++s_ends;
    s_board.refresh_sealed = true;
    s_board.error = s_end_error;
    return s_end_error;
}
void board_display_transfer_invalidate(void)
{
    assert(s_lvgl_owner && s_lock_depth == 0);
    ++s_invalidates;
    ++s_board.invalidation_revision;
    s_board.refresh_sealed = s_board.coverage_complete = s_board.indicator_transfer_confirmed = false;
    s_board.transfers = s_board.completed_transfers = 0;
}
board_display_receipt_t board_display_transfer_receipt(void)
{
    assert(s_lock_depth == 0);
    return s_board;
}

void presenter_test_enter(portMUX_TYPE *lock)
{
    assert(s_lock_depth == 0);
    assert(pthread_mutex_lock(lock) == 0);
    ++s_lock_depth;
}

void presenter_test_exit(portMUX_TYPE *lock)
{
    assert(s_lock_depth == 1);
    --s_lock_depth;
    assert(pthread_mutex_unlock(lock) == 0);
}

int64_t esp_timer_get_time(void)
{
    assert(s_lock_depth == 0);
    return (int64_t)atomic_load(&s_clock_us);
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *data)
{
    assert(s_lvgl_owner && s_lock_depth == 0);
    assert(callback && period == 100 && !data);
    ++s_timer_creations;
    if (s_fail_timer) return NULL;
    s_test_timer.callback = callback;
    return &s_test_timer;
}

void app_ui_render(const app_view_t *view)
{
    assert(s_lvgl_owner && s_lock_depth == 0 && view);
    ++s_renders;
    s_rendered = *view;
    snprintf(s_rendered_message, sizeof(s_rendered_message), "%s", view->message ? view->message : "");
    snprintf(s_rendered_target, sizeof(s_rendered_target), "%s", view->target_name ? view->target_name : "");
    if (s_render_hook) s_render_hook(view);
}

static void initialize(void)
{
    s_lvgl_owner = true;
    assert(app_presenter_init() == ESP_OK);
    s_lvgl_owner = false;
    assert(app_presenter_status().initialized);
}

static void tick(void)
{
    assert(s_test_timer.callback);
    s_lvgl_owner = true;
    s_test_timer.callback(&s_test_timer);
    s_lvgl_owner = false;
}

static bool all_zero(const void *memory, size_t size)
{
    const unsigned char *bytes = memory;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

static app_view_t setup_view(void)
{
    app_view_t view = {.epoch = 4, .ui_revision = 8, .overlay = APP_OVERLAY_WIFI_SETUP,
                      .wifi_setup_active = true};
    strcpy(view.wifi_setup_ssid, "SYNTHETIC-SETUP");
    strcpy(view.wifi_setup_passcode, "TESTONLY-LONG-SECRET");
    strcpy(view.wifi_setup_url, "http://192.0.2.1/");
    return view;
}

static void init_failure_and_retry(void)
{
    app_view_t view = {0};
    app_presenter_status_t status = app_presenter_status();
    assert(!status.initialized && !status.has_published && !status.has_applied);
    assert(status.init_error == ESP_ERR_INVALID_STATE);
    assert(app_presenter_publish(NULL) == APP_PRESENTER_INVALID);
    assert(app_presenter_publish(&view) == APP_PRESENTER_UNAVAILABLE);
    s_fail_timer = s_lvgl_owner = true;
    assert(app_presenter_init() == ESP_ERR_NO_MEM);
    s_lvgl_owner = false;
    status = app_presenter_status();
    assert(!status.initialized && status.init_error == ESP_ERR_NO_MEM);
    assert(app_presenter_publish(&view) == APP_PRESENTER_UNAVAILABLE && s_renders == 0);
    assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
    s_fail_timer = false;
    initialize();
    s_lvgl_owner = true;
    assert(app_presenter_init() == ESP_ERR_INVALID_STATE);
    s_lvgl_owner = false;
    assert(s_timer_creations == 2 && app_presenter_status().init_error == ESP_OK);
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    assert(s_renders == 1);
}

static void first_snapshot_and_coalescing(void)
{
    initialize();
    for (unsigned i = 0; i < 100; ++i) tick();
    assert(s_renders == 0);
    app_view_t view = {0}; /* Zero is a valid first identity, not a sentinel. */
    for (size_t i = 0; i < 10000; ++i) {
        view.queued = i;
        assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    }
    app_presenter_status_t status = app_presenter_status();
    assert(status.has_published && !status.has_applied && status.pending);
    assert(status.published_count == 10000 && status.coalesced_count == 9999);
    assert(s_renders == 0);
    tick();
    assert(s_renders == 1 && s_rendered.queued == 9999);
    assert(s_rendered.epoch == 0 && s_rendered.ui_revision == 0);
    for (unsigned i = 0; i < 100; ++i) tick();
    status = app_presenter_status();
    assert(status.applied_count == 1 && !status.pending && !status.applying);
    assert(s_renders == 1 && s_timer_creations == 1);
    assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
}

static void publish_temporary_strings(void)
{
    char message[] = "Temporary caller message", target[] = "Temporary caller target";
    app_view_t view = {.message = message, .target_name = target};
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    memset(message, 'X', sizeof(message));
    memset(target, 'Y', sizeof(target));
}

static void pointer_lifetime_and_bounds(void)
{
    initialize();
    publish_temporary_strings();
    tick();
    assert(strcmp(s_rendered_message, "Temporary caller message") == 0);
    assert(strcmp(s_rendered_target, "Temporary caller target") == 0);
    char message[400], target[400];
    memset(message, 'M', sizeof(message));
    memset(target, 'T', sizeof(target)); /* Bounded copies need no far terminator. */
    app_view_t view = {.message = message, .target_name = target};
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    assert(strlen(s_rendered_message) == 159 && strlen(s_rendered_target) == 95);
    view.message = "short";
    view.target_name = "";
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    assert(all_zero(s_slot.message + 6, sizeof(s_slot.message) - 6));
    assert(all_zero(s_slot.target, sizeof(s_slot.target)));
    tick();
    assert(s_rendered.target_name && !s_rendered_target[0]);
    view.message = view.target_name = NULL;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    assert(!s_rendered.message && !s_rendered.target_name);
}

static void stale_identity_rejection(void)
{
    initialize();
    app_view_t view = {.epoch = 10, .ui_revision = 20};
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    view.epoch = 9; view.ui_revision = 21;
    assert(app_presenter_publish(&view) == APP_PRESENTER_STALE);
    view.epoch = 11; view.ui_revision = 19;
    assert(app_presenter_publish(&view) == APP_PRESENTER_STALE);
    view.ui_revision = 20;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    view.queued = 9;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    view.epoch = 10; view.ui_revision = 100;
    assert(app_presenter_publish(&view) == APP_PRESENTER_STALE);
    tick();
    assert(s_rendered.epoch == 11 && s_rendered.ui_revision == 20 && s_rendered.queued == 9);
    view.epoch = view.ui_revision = UINT64_MAX;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    view.epoch = view.ui_revision = 0;
    assert(app_presenter_publish(&view) == APP_PRESENTER_STALE);
    app_presenter_status_t status = app_presenter_status();
    assert(status.published_epoch == UINT64_MAX && status.applied_revision == UINT64_MAX);
    assert(status.published_count == 4 && status.applied_count == 3 && status.stale_count == 4);
}

static void secret_overwrite_consume_and_hidden_state(void)
{
    initialize();
    app_view_t view = setup_view();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    assert(strcmp(s_slot.view.wifi_setup_passcode, view.wifi_setup_passcode) == 0);
    /* Shortening the caller string intentionally leaves its old secret tail. */
    view.wifi_setup_passcode[0] = 'S'; view.wifi_setup_passcode[1] = '\0';
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    assert(s_slot.view.wifi_setup_passcode[0] == 'S');
    assert(all_zero(s_slot.view.wifi_setup_passcode + 1, sizeof(view.wifi_setup_passcode) - 1));
    tick();
    assert(strcmp(s_rendered.wifi_setup_passcode, "S") == 0);
    assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
    for (unsigned reason = 0; reason < 4; ++reason) {
        view = setup_view();
        if (reason == 0) view.overlay = APP_OVERLAY_NONE;
        if (reason == 1) view.locked = true;
        if (reason == 2) view.screen_off = true;
        if (reason == 3) view.wifi_setup_active = false;
        assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
        assert(all_zero(s_slot.view.wifi_setup_ssid, sizeof(view.wifi_setup_ssid)));
        assert(all_zero(s_slot.view.wifi_setup_passcode, sizeof(view.wifi_setup_passcode)));
        assert(all_zero(s_slot.view.wifi_setup_url, sizeof(view.wifi_setup_url)));
        assert(strcmp(view.wifi_setup_passcode, "TESTONLY-LONG-SECRET") == 0);
        tick();
        assert(!s_rendered.wifi_setup_passcode[0]);
        assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
    }
    view = setup_view();
    memset(view.wifi_setup_passcode, 'A', sizeof(view.wifi_setup_passcode));
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    tick();
    assert(strlen(s_rendered.wifi_setup_passcode) == sizeof(view.wifi_setup_passcode) - 1);
}

static void publish_inside_render(const app_view_t *view)
{
    assert(view->epoch == 4 && view->ui_revision == 8);
    assert(strcmp(view->wifi_setup_passcode, "TESTONLY-LONG-SECRET") == 0);
    assert(all_zero(&s_slot, sizeof(s_slot)));
    app_view_t next = {.epoch = 5, .ui_revision = 9, .locked = true, .message = "Newer lock"};
    assert(app_presenter_publish(&next) == APP_PRESENTER_ACCEPTED);
    app_presenter_status_t status = app_presenter_status();
    assert(status.pending && status.applying && !status.has_applied);
    assert(status.published_epoch == 5 && status.published_count == 2);
    /* A nested timer invocation must not overwrite the in-flight snapshot. */
    s_test_timer.callback(&s_test_timer);
    assert(s_renders == 1 && view->epoch == 4);
    assert(strcmp(view->wifi_setup_passcode, "TESTONLY-LONG-SECRET") == 0);
}

static void publish_during_render(void)
{
    initialize();
    app_view_t view = setup_view();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    s_render_hook = publish_inside_render;
    tick();
    s_render_hook = NULL;
    assert(all_zero(&s_scratch, sizeof(s_scratch)));
    app_presenter_status_t status = app_presenter_status();
    assert(status.pending && !status.applying && status.applied_epoch == 4);
    assert(status.applied_count == 1 && status.coalesced_count == 0);
    tick();
    assert(s_renders == 2 && s_rendered.epoch == 5 && s_rendered.ui_revision == 9);
    assert(s_rendered.locked && !s_rendered.wifi_setup_passcode[0]);
    assert(strcmp(s_rendered_message, "Newer lock") == 0);
    assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
}

static void delayed_consumer_status(void)
{
    initialize();
    app_view_t view = {0};
    atomic_store(&s_clock_us, 1000000);
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    atomic_store(&s_clock_us, 5000000);
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    atomic_store(&s_clock_us, 10000000);
    app_presenter_status_t status = app_presenter_status();
    assert(status.pending_age_ms == 9000 && status.publish_age_ms == 5000);
    assert(!status.has_applied && status.applied_count == 0);
    tick();
    status = app_presenter_status();
    assert(status.has_applied && status.last_apply_latency_ms == 5000);
    assert(status.pending_age_ms == 0 && status.apply_age_ms == 0);
    atomic_store(&s_clock_us, 12000000);
    status = app_presenter_status();
    assert(status.apply_age_ms == 2000 && status.publish_age_ms == 7000);
    atomic_store(&s_clock_us, -1);
    status = app_presenter_status();
    assert(status.apply_age_ms == 0 && status.publish_age_ms == 0);
    assert(s_renders == 1); /* Observation never consumes or renders. */
}

static pthread_mutex_t s_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_changed = PTHREAD_COND_INITIALIZER;
static bool s_render_entered, s_release_render;

static void paused_render(const app_view_t *view)
{
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_render_entered = true;
    assert(pthread_cond_broadcast(&s_gate_changed) == 0);
    while (!s_release_render) assert(pthread_cond_wait(&s_gate_changed, &s_gate) == 0);
    assert(view->epoch == 1 && view->ui_revision == 1);
    assert(pthread_mutex_unlock(&s_gate) == 0);
}

static void *consumer_thread(void *unused)
{
    (void)unused;
    tick();
    return NULL;
}

static void caller_progress_while_renderer_delayed(void)
{
    initialize();
    app_view_t view = {.epoch = 1, .ui_revision = 1};
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    s_render_hook = paused_render;
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, consumer_thread, NULL) == 0);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_render_entered) assert(pthread_cond_wait(&s_gate_changed, &s_gate) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    unsigned caller_progress = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        view.ui_revision = (uint64_t)i + 2;
        assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
        ++caller_progress;
    }
    app_presenter_status_t status = app_presenter_status();
    assert(caller_progress == 10000 && status.applying && status.pending);
    assert(status.published_count == 10001 && status.applied_count == 0);
    assert(status.coalesced_count == 9999 && status.published_revision == 10001);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_render = true;
    assert(pthread_cond_broadcast(&s_gate_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    s_render_hook = NULL;
    tick();
    assert(s_renders == 2 && s_rendered.ui_revision == 10001);
    assert(app_presenter_status().applied_count == 2);
    assert(all_zero(&s_slot, sizeof(s_slot)) && all_zero(&s_scratch, sizeof(s_scratch)));
}

typedef struct { const char *name; void (*run)(void); } test_case_t;
#include "display_cases.inc"
static const test_case_t s_cases[] = {
    {"init_failure_and_retry", init_failure_and_retry},
    {"first_snapshot_and_coalescing", first_snapshot_and_coalescing},
    {"pointer_lifetime_and_bounds", pointer_lifetime_and_bounds},
    {"stale_identity_rejection", stale_identity_rejection},
    {"secret_overwrite_consume_and_hidden_state", secret_overwrite_consume_and_hidden_state},
    {"publish_during_render", publish_during_render},
    {"delayed_consumer_status", delayed_consumer_status},
    {"caller_progress_while_renderer_delayed", caller_progress_while_renderer_delayed},
    {"display_binding_failures", display_binding_failures},
    {"display_requires_real_complete_receipt", display_requires_real_complete_receipt},
    {"display_late_completion_and_failure", display_late_completion_and_failure},
    {"display_idle_proof_and_owner_liveness", display_idle_proof_and_owner_liveness},
    {"display_superseded_hidden_and_recreated", display_superseded_hidden_and_recreated},
    {"display_revocation_and_live_generation", display_revocation_and_live_generation},
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) puts(s_cases[i].name);
        return 0;
    }
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) {
        if (strcmp(argv[1], s_cases[i].name) != 0) continue;
        s_cases[i].run();
        printf("PASS: presenter %s\n", s_cases[i].name);
        return 0;
    }
    fprintf(stderr, "Unknown presenter case: %s\n", argv[1]);
    return 2;
}
