// SPDX-License-Identifier: Apache-2.0
#include "app_presenter.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define PRESENTER_TASK_BSS EXT_RAM_BSS_ATTR
#else
#define PRESENTER_TASK_BSS
#endif
#include "board_display_transfer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    app_view_t view;
    char message[APP_PRESENTER_MESSAGE_BYTES];
    char target[APP_PRESENTER_TARGET_BYTES];
} presenter_snapshot_t;

/* Only the LVGL timer touches scratch. The slot/status lock is never held
 * across rendering, timer allocation, logging or another application call. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Task-only copies after PSRAM initialization; the ISR never enters presenter. */
static PRESENTER_TASK_BSS presenter_snapshot_t s_slot;
static PRESENTER_TASK_BSS presenter_snapshot_t s_scratch;
static lv_timer_t *s_timer;
static app_presenter_status_t s_status = {.init_error = ESP_ERR_INVALID_STATE};
static uint64_t s_publish_ms, s_apply_ms, s_pending_since_ms;
static lv_display_t *s_display;
static uint64_t s_display_generation, s_refresh_sequence, s_owner_heartbeat_ms;
static uint64_t s_observed_invalidation_revision;
static bool s_have_heartbeat, s_refresh_active, s_have_transfer, s_geometry_fault;
static app_ui_capture_indicator_t s_indicator;
static board_display_token_t s_refresh_token;
static board_display_receipt_t s_confirmed_transfer;
static esp_err_t s_display_error = ESP_ERR_INVALID_STATE;

static void wipe(void *memory, size_t bytes)
{
    volatile unsigned char *destination = memory;
    while (bytes--) *destination++ = 0;
}

static uint64_t now_ms(void)
{
    int64_t microseconds = esp_timer_get_time();
    return microseconds > 0 ? (uint64_t)microseconds / 1000U : 0;
}

static uint64_t age_ms(uint64_t now, uint64_t then)
{
    return now >= then ? now - then : 0;
}

static void increment(uint64_t *count)
{
    if (*count != UINT64_MAX) ++*count;
}

static bool same_indicator(const app_ui_capture_indicator_t *a, const app_ui_capture_indicator_t *b)
{
    return a->armed == b->armed && (!a->armed ||
        (a->capture_generation == b->capture_generation && a->indicator_revision == b->indicator_revision &&
         a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2));
}

static bool valid_indicator(const app_ui_capture_indicator_t *indicator)
{
    return indicator->armed && indicator->capture_generation && indicator->indicator_revision &&
        indicator->x1 >= 0 && indicator->y1 >= 0 && indicator->x2 < 480 && indicator->y2 < 480 &&
        indicator->x2 >= indicator->x1 && indicator->y2 >= indicator->y1;
}

/* Caller holds s_lock; the receipt was copied outside it. Owner observation
 * must retain a complete receipt before the next refresh replaces Board's
 * records, even when no status caller ran between those refreshes. */
static bool observe_transfer_locked(const board_display_receipt_t *observed)
{
    /* A status caller can copy before owner progress, then acquire this lock
     * afterwards. It must not restore an invalidated epoch or older proof. */
    if (observed->invalidation_revision < s_observed_invalidation_revision) return false;
    s_observed_invalidation_revision = observed->invalidation_revision;
    bool observer_current = observed->installed && observed->callback_attached &&
        !observed->association_lost && observed->error == ESP_OK &&
        observed->display_generation == s_display_generation;
    if (!s_display || !observer_current || s_geometry_fault || s_display_error != ESP_OK ||
        (s_have_transfer && observed->invalidation_revision != s_confirmed_transfer.invalidation_revision))
        s_have_transfer = false;
    bool token_matches = observed->token.display_generation == s_display_generation &&
        observed->token.capture_generation == s_indicator.capture_generation &&
        observed->token.indicator_revision == s_indicator.indicator_revision &&
        observed->token.refresh_sequence && observed->token.refresh_sequence <= s_refresh_sequence;
    bool area_matches = observed->required.x0 == s_indicator.x1 && observed->required.y0 == s_indicator.y1 &&
        observed->required.x1 == s_indicator.x2 + 1 && observed->required.y1 == s_indicator.y2 + 1;
    if (s_display && s_indicator.armed && observer_current && !s_geometry_fault &&
        s_display_error == ESP_OK && token_matches && area_matches && observed->refresh_sealed &&
        observed->coverage_complete && observed->indicator_transfer_confirmed &&
        observed->transfers && observed->transfers == observed->completed_transfers &&
        (!s_have_transfer || observed->token.refresh_sequence >= s_confirmed_transfer.token.refresh_sequence)) {
        s_confirmed_transfer = *observed;
        s_have_transfer = true;
    }
    return observer_current;
}

static void observe_owner(void)
{
    /* Only called by the actual LVGL owner, never by publish/status polling. */
    app_ui_capture_indicator_t indicator = app_ui_capture_indicator();
    if (!valid_indicator(&indicator)) indicator = (app_ui_capture_indicator_t){0};
    board_display_receipt_t receipt = board_display_transfer_receipt();
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    bool changed = !same_indicator(&indicator, &s_indicator);
    bool bound = s_display != NULL;
    bool generation_changed = bound && receipt.installed && receipt.callback_attached &&
        receipt.display_generation && receipt.display_generation != s_display_generation;
    lv_display_t *display = s_display;
    if (generation_changed) {
        s_display_generation = receipt.display_generation;
        s_display_error = ESP_OK;
    }
    s_owner_heartbeat_ms = now;
    s_have_heartbeat = true;
    s_indicator = indicator;
    if (changed || generation_changed || (s_have_transfer &&
        receipt.invalidation_revision != s_confirmed_transfer.invalidation_revision)) {
        s_have_transfer = false;
        s_refresh_active = false;
    }
    /* A changed identity is invalidated below and needs its own new transfer.
     * Otherwise preserve the already-read proof before begin clears records. */
    if (!changed && !generation_changed) (void)observe_transfer_locked(&receipt);
    portEXIT_CRITICAL(&s_lock);
    if ((changed || generation_changed) && bound) board_display_transfer_invalidate();
    /* Callback replacement invalidates the old hardware generation. Repaint
     * under the actual LVGL owner; a read-only status caller never does this. */
    if (generation_changed && indicator.armed) {
        lv_area_t area = {indicator.x1, indicator.y1, indicator.x2, indicator.y2};
        lv_obj_invalidate_area(lv_display_get_screen_active(display), &area);
    }
}

static bool supported_display(lv_display_t *display)
{
    return display && lv_display_get_horizontal_resolution(display) == 480 &&
        lv_display_get_vertical_resolution(display) == 480 &&
        lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_0 &&
        lv_display_get_offset_x(display) == 0 && lv_display_get_offset_y(display) == 0;
}

static void display_event(lv_event_t *event)
{
    lv_display_t *display = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);
    portENTER_CRITICAL(&s_lock);
    bool bound = display == s_display;
    portEXIT_CRITICAL(&s_lock);
    if (!bound) return;
    if (code == LV_EVENT_DELETE) {
        portENTER_CRITICAL(&s_lock);
        s_display = NULL;
        s_have_transfer = s_refresh_active = s_have_heartbeat = false;
        s_indicator = (app_ui_capture_indicator_t){0};
        s_display_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_lock);
        board_display_transfer_invalidate();
        return;
    }
    if (code != LV_EVENT_REFR_START && code != LV_EVENT_REFR_READY) return;
    if (!supported_display(display)) {
        portENTER_CRITICAL(&s_lock);
        s_geometry_fault = true;
        s_have_transfer = s_refresh_active = false;
        s_display_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_lock);
        board_display_transfer_invalidate();
        return;
    }
    observe_owner();
    if (code == LV_EVENT_REFR_START) {
        portENTER_CRITICAL(&s_lock);
        if (!s_indicator.armed || s_geometry_fault || s_refresh_sequence == UINT64_MAX) {
            s_refresh_active = false;
            if (s_refresh_sequence == UINT64_MAX) {
                s_have_transfer = false;
                s_display_error = ESP_ERR_INVALID_STATE;
            }
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        app_ui_capture_indicator_t indicator = s_indicator;
        board_display_token_t token = {.display_generation = s_display_generation,
            .capture_generation = indicator.capture_generation, .indicator_revision = indicator.indicator_revision,
            .refresh_sequence = ++s_refresh_sequence};
        s_refresh_token = token;
        s_refresh_active = true;
        portEXIT_CRITICAL(&s_lock);
        board_display_rect_t required = {.x0 = indicator.x1, .y0 = indicator.y1,
            .x1 = (int16_t)(indicator.x2 + 1), .y1 = (int16_t)(indicator.y2 + 1)};
        esp_err_t error = board_display_transfer_begin(&token, &required);
        portENTER_CRITICAL(&s_lock);
        s_display_error = error;
        if (error != ESP_OK) s_have_transfer = s_refresh_active = false;
        portEXIT_CRITICAL(&s_lock);
    } else {
        portENTER_CRITICAL(&s_lock);
        bool active = s_refresh_active;
        board_display_token_t token = s_refresh_token;
        s_refresh_active = false;
        portEXIT_CRITICAL(&s_lock);
        if (!active) return;
        esp_err_t error = board_display_transfer_end(&token);
        portENTER_CRITICAL(&s_lock);
        if (error != ESP_OK) { s_have_transfer = false; s_display_error = error; }
        portEXIT_CRITICAL(&s_lock);
    }
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = 0;
    /* Destination was wiped first; even a shorter replacement leaves no tail. */
    while (source && length + 1 < capacity && source[length]) {
        destination[length] = source[length];
        ++length;
    }
    destination[length] = '\0';
}

static void clear_tail(char *text, size_t capacity)
{
    size_t length = 0;
    while (length + 1 < capacity && text[length]) ++length;
    wipe(text + length, capacity - length);
}

static void bind_text(presenter_snapshot_t *snapshot)
{
    if (snapshot->view.message) snapshot->view.message = snapshot->message;
    if (snapshot->view.target_name) snapshot->view.target_name = snapshot->target;
}

static void store_view(const app_view_t *view)
{
    wipe(&s_slot, sizeof(s_slot));
    s_slot.view = *view;
    copy_text(s_slot.message, sizeof(s_slot.message), view->message);
    copy_text(s_slot.target, sizeof(s_slot.target), view->target_name);
    bind_text(&s_slot);
    app_view_t *owned = &s_slot.view;
    if (owned->overlay == APP_OVERLAY_WIFI_SETUP && owned->wifi_setup_active &&
        !owned->locked && !owned->screen_off) {
        clear_tail(owned->wifi_setup_ssid, sizeof(owned->wifi_setup_ssid));
        clear_tail(owned->wifi_setup_passcode, sizeof(owned->wifi_setup_passcode));
        clear_tail(owned->wifi_setup_url, sizeof(owned->wifi_setup_url));
    } else {
        wipe(owned->wifi_setup_ssid, sizeof(owned->wifi_setup_ssid));
        wipe(owned->wifi_setup_passcode, sizeof(owned->wifi_setup_passcode));
        wipe(owned->wifi_setup_url, sizeof(owned->wifi_setup_url));
    }
}

static void present(lv_timer_t *timer)
{
    (void)timer;
    observe_owner();
    uint64_t published_ms, epoch, revision;
    portENTER_CRITICAL(&s_lock);
    if (!s_status.pending || s_status.applying) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_scratch = s_slot;
    bind_text(&s_scratch);
    wipe(&s_slot, sizeof(s_slot));
    published_ms = s_publish_ms;
    epoch = s_scratch.view.epoch;
    revision = s_scratch.view.ui_revision;
    s_status.pending = false;
    s_status.applying = true;
    portEXIT_CRITICAL(&s_lock);

    /* LVGL invokes this callback under its existing owner/mutex. A publish
     * during render occupies the slot for the next tick; never loop/catch up. */
    app_ui_render(&s_scratch.view);
    observe_owner();
    wipe(&s_scratch, sizeof(s_scratch));
    uint64_t completed_ms = now_ms();

    portENTER_CRITICAL(&s_lock);
    increment(&s_status.applied_count);
    s_status.has_applied = true;
    s_status.applied_epoch = epoch;
    s_status.applied_revision = revision;
    s_status.last_apply_latency_ms = age_ms(completed_ms, published_ms);
    s_apply_ms = completed_ms;
    s_status.applying = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_presenter_init(void)
{
    /* Serialized by the startup LVGL lock; no runtime initialization calls. */
    if (s_timer) return ESP_ERR_INVALID_STATE;
    lv_timer_t *timer = lv_timer_create(present, APP_PRESENTER_PERIOD_MS, NULL);
    esp_err_t error = timer ? ESP_OK : ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    s_timer = timer;
    s_status.init_error = error;
    s_status.initialized = timer != NULL;
    portEXIT_CRITICAL(&s_lock);
    return error;
}

app_presenter_result_t app_presenter_publish(const app_view_t *view)
{
    if (!view) return APP_PRESENTER_INVALID;
    uint64_t published_ms = now_ms();
    portENTER_CRITICAL(&s_lock);
    if (!s_status.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return APP_PRESENTER_UNAVAILABLE;
    }
    if (s_status.has_published && (view->epoch < s_status.published_epoch ||
                                 view->ui_revision < s_status.published_revision)) {
        increment(&s_status.stale_count);
        portEXIT_CRITICAL(&s_lock);
        return APP_PRESENTER_STALE;
    }
    store_view(view);
    if (s_status.pending) increment(&s_status.coalesced_count);
    else s_pending_since_ms = published_ms;
    s_publish_ms = published_ms;
    s_status.pending = true;
    s_status.has_published = true;
    s_status.published_epoch = view->epoch;
    s_status.published_revision = view->ui_revision;
    increment(&s_status.published_count);
    portEXIT_CRITICAL(&s_lock);
    return APP_PRESENTER_ACCEPTED;
}

app_presenter_status_t app_presenter_status(void)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    app_presenter_status_t status = s_status;
    status.publish_age_ms = status.has_published ? age_ms(now, s_publish_ms) : 0;
    status.apply_age_ms = status.has_applied ? age_ms(now, s_apply_ms) : 0;
    status.pending_age_ms = status.pending ? age_ms(now, s_pending_since_ms) : 0;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

esp_err_t app_presenter_bind_display(lv_display_t *display)
{
    if (!supported_display(display)) return ESP_ERR_INVALID_ARG;
    board_display_receipt_t receipt = board_display_transfer_receipt();
    if (!receipt.installed || !receipt.callback_attached || !receipt.display_generation)
        return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (!s_status.initialized || s_display) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_display = display;
    s_display_generation = receipt.display_generation;
    /* Preserve monotonic sequence across display deletion/rebinding too. */
    s_geometry_fault = s_have_transfer = s_refresh_active = false;
    s_display_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    board_display_transfer_invalidate();
    lv_display_add_event_cb(display, display_event, LV_EVENT_ALL, NULL);
    observe_owner();
    return ESP_OK;
}

app_presenter_indicator_status_t app_presenter_indicator_status(void)
{
    /* Board receipt computes bounded exact coverage without LVGL or IO. This
     * caller never waits for a render/transfer and never manufactures one. */
    board_display_receipt_t observed = board_display_transfer_receipt();
    int64_t raw_us = esp_timer_get_time();
    uint64_t current_us = raw_us > 0 ? (uint64_t)raw_us : 0;
    uint64_t current_ms = current_us / 1000U;
    portENTER_CRITICAL(&s_lock);
    app_presenter_indicator_status_t status = {
        .bound = s_display != NULL, .indicator_armed = s_indicator.armed,
        .display_generation = s_display_generation, .capture_generation = s_indicator.capture_generation,
        .indicator_revision = s_indicator.indicator_revision, .owner_heartbeat_ms = s_owner_heartbeat_ms,
        .error = s_display_error
    };
    bool observer_current = observe_transfer_locked(&observed);
    status.clock_valid = raw_us >= 0 && s_have_heartbeat && current_ms >= s_owner_heartbeat_ms &&
        (!s_have_transfer || current_us >= s_confirmed_transfer.completed_us);
    status.heartbeat_age_ms = status.clock_valid ? current_ms - s_owner_heartbeat_ms : 0;
    status.owner_alive = status.clock_valid && status.heartbeat_age_ms <= APP_PRESENTER_OWNER_MAX_AGE_MS;
    status.indicator_transfer_confirmed = status.bound && s_indicator.armed && s_have_transfer &&
        s_confirmed_transfer.token.capture_generation == s_indicator.capture_generation &&
        s_confirmed_transfer.token.indicator_revision == s_indicator.indicator_revision;
    if (status.indicator_transfer_confirmed) {
        status.refresh_sequence = s_confirmed_transfer.token.refresh_sequence;
        status.completed_us = s_confirmed_transfer.completed_us;
        status.completion_age_ms = status.clock_valid ? (current_us - status.completed_us) / 1000U : 0;
    }
    if (!observer_current) status.error = observed.error != ESP_OK ? observed.error : ESP_ERR_INVALID_STATE;
    status.usable = status.indicator_transfer_confirmed && status.owner_alive && status.error == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    return status;
}
