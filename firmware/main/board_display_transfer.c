// SPDX-License-Identifier: Apache-2.0
#include "board_display_transfer.h"

#include <stddef.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define DISPLAY_EDGE 480
#define NO_RECORD UINT8_MAX

typedef struct {
    board_display_rect_t rect;
    bool returned;
    bool color_submitted;
    bool completed;
    esp_err_t error;
    uint64_t completed_us;
} transfer_record_t;

static DRAM_ATTR struct {
    portMUX_TYPE lock;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_t native_panel;
    esp_lcd_panel_io_t native_io;
    esp_lcd_panel_io_color_trans_done_cb_t callback;
    void *callback_context;
    bool installed;
    bool attached;
    bool association_lost;
    uint64_t generation;
    uint64_t invalidation_revision;
    uint64_t last_refresh;
    board_display_token_t token;
    board_display_rect_t required;
    bool active;
    bool sealed;
    bool token_valid;
    esp_err_t error;
    uint8_t count;
    transfer_record_t records[BOARD_DISPLAY_TRANSFER_RECTS];
    struct {
        bool active;
        bool returned;
        bool completed;
        bool color_attempted;
        bool color_returned;
        uint8_t index;
        size_t expected_bytes;
        const void *pixels;
    } pending;
} d = {.lock = portMUX_INITIALIZER_UNLOCKED};

static bool same_token(const board_display_token_t *a, const board_display_token_t *b)
{
    return a->display_generation == b->display_generation &&
        a->capture_generation == b->capture_generation && a->indicator_revision == b->indicator_revision &&
        a->refresh_sequence == b->refresh_sequence;
}

static bool valid_rect(const board_display_rect_t *r)
{
    return r && r->x0 >= 0 && r->y0 >= 0 && r->x1 <= DISPLAY_EDGE && r->y1 <= DISPLAY_EDGE &&
        r->x0 < r->x1 && r->y0 < r->y1;
}

static bool intersects(const board_display_rect_t *a, const board_display_rect_t *b)
{
    return a->x0 < b->x1 && a->x1 > b->x0 && a->y0 < b->y1 && a->y1 > b->y0;
}

static void IRAM_ATTR invalidate_locked(void)
{
    d.token_valid = false;
    if (d.invalidation_revision != UINT64_MAX) ++d.invalidation_revision;
    if (d.invalidation_revision == UINT64_MAX) {
        d.association_lost = true;
        d.error = ESP_ERR_INVALID_STATE;
    }
}

static void lose_association_locked(void)
{
    d.association_lost = true;
    invalidate_locked();
    d.error = ESP_ERR_INVALID_STATE;
}

static bool IRAM_ATTR color_done(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)context;
    portENTER_CRITICAL_ISR(&d.lock);
    if (io != d.io || !d.pending.active || !d.pending.color_attempted || d.pending.completed) {
        d.association_lost = true;
        invalidate_locked();
        d.error = ESP_ERR_INVALID_STATE;
    } else {
        d.pending.completed = true;
        if (d.pending.index != NO_RECORD && d.pending.index < d.count) {
            transfer_record_t *record = &d.records[d.pending.index];
            record->completed = true;
            record->completed_us = (uint64_t)esp_timer_get_time();
        }
        if (d.pending.returned) d.pending.active = false;
    }
    esp_lcd_panel_io_color_trans_done_cb_t callback = d.callback;
    void *callback_context = d.callback_context;
    portEXIT_CRITICAL_ISR(&d.lock);
    /* The pinned adapter's callback supplies the TE waiter notification. */
    return callback ? callback(io, event, callback_context) : false;
}

static esp_err_t observe_register(esp_lcd_panel_io_t *io,
    const esp_lcd_panel_io_callbacks_t *callbacks, void *context)
{
    if (!callbacks) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&d.lock);
    /* On this pin the adapter unregisters by retaining its function pointer
     * and replacing the impl context with NULL. Preserve that native path. */
    bool wrapping = callbacks->on_color_trans_done != NULL && context != NULL &&
        !d.pending.active && !d.association_lost;
    esp_lcd_panel_io_color_trans_done_cb_t previous = d.callback;
    void *previous_context = d.callback_context;
    bool previously_attached = d.attached;
    invalidate_locked();
    if (wrapping) {
        d.callback = callbacks->on_color_trans_done;
        d.callback_context = context;
    } else {
        /* Runtime replacement during IO cannot be atomically associated. Keep
         * the requested native callback semantics and fail observation closed. */
        d.token_valid = false;
        d.attached = false;
        if (d.pending.active) lose_association_locked();
    }
    portEXIT_CRITICAL(&d.lock);
    esp_lcd_panel_io_callbacks_t wrapped = *callbacks;
    if (wrapping) wrapped.on_color_trans_done = color_done;
    esp_err_t result = d.native_io.register_event_callbacks(io, wrapping ? &wrapped : callbacks,
        wrapping ? &d : context);
    portENTER_CRITICAL(&d.lock);
    if (result == ESP_OK) {
        d.attached = wrapping;
        if (previous || previous_context) {
            if (d.generation != UINT64_MAX) ++d.generation;
            else lose_association_locked();
            d.last_refresh = 0;
            d.active = false;
        }
        if (!wrapping) { d.callback = callbacks->on_color_trans_done; d.callback_context = context; }
    } else {
        d.callback = previous;
        d.callback_context = previous_context;
        d.attached = previously_attached;
        d.token_valid = false;
        d.error = result;
    }
    portEXIT_CRITICAL(&d.lock);
    return result;
}

static esp_err_t observe_color(esp_lcd_panel_io_t *io, int command, const void *pixels, size_t bytes)
{
    portENTER_CRITICAL(&d.lock);
    bool associated = io == d.io && d.pending.active && !d.pending.returned && !d.pending.color_attempted;
    bool exact_payload = associated && pixels == d.pending.pixels && bytes == d.pending.expected_bytes;
    if (associated) d.pending.color_attempted = true;
    else lose_association_locked();
    if (associated && !exact_payload) { invalidate_locked(); d.error = ESP_ERR_INVALID_SIZE; }
    portEXIT_CRITICAL(&d.lock);
    esp_err_t result = d.native_io.tx_color(io, command, pixels, bytes);
    portENTER_CRITICAL(&d.lock);
    if (associated) {
        d.pending.color_returned = true;
        if (d.pending.index != NO_RECORD && d.pending.index < d.count)
            d.records[d.pending.index].color_submitted = result == ESP_OK && exact_payload;
        if (result != ESP_OK) { invalidate_locked(); d.error = result; }
    }
    portEXIT_CRITICAL(&d.lock);
    return result;
}

static esp_err_t observe_draw(esp_lcd_panel_t *panel, int x0, int y0, int x1, int y1, const void *pixels)
{
    bool coordinates_valid = x0 >= 0 && y0 >= 0 && x0 < x1 && y0 < y1 && x1 <= DISPLAY_EDGE && y1 <= DISPLAY_EDGE;
    board_display_rect_t rect = {0};
    if (coordinates_valid) rect = (board_display_rect_t){(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1};
    portENTER_CRITICAL(&d.lock);
    bool tracking = !d.pending.active;
    if (!tracking) lose_association_locked();
    uint8_t index = NO_RECORD;
    if (!coordinates_valid) { invalidate_locked(); d.error = ESP_ERR_INVALID_ARG; }
    if (d.active && !d.sealed && tracking && coordinates_valid && !d.association_lost) {
        if (d.count == BOARD_DISPLAY_TRANSFER_RECTS) lose_association_locked();
        else {
            index = d.count++;
            d.records[index] = (transfer_record_t){.rect = rect};
        }
    } else if (coordinates_valid && d.token_valid && intersects(&rect, &d.required)) {
        /* An unbound draw may overwrite the old warning, even if its token
         * and text were not changed by the presenter. */
        invalidate_locked();
    }
    if (tracking) {
        memset(&d.pending, 0, sizeof(d.pending));
        d.pending.active = true;
        d.pending.index = index;
        d.pending.pixels = pixels;
        d.pending.expected_bytes = coordinates_valid ? (size_t)(x1-x0)*(size_t)(y1-y0)*2U : 0;
    }
    portEXIT_CRITICAL(&d.lock);
    esp_err_t result = d.native_panel.draw_bitmap(panel, x0, y0, x1, y1, pixels);
    portENTER_CRITICAL(&d.lock);
    if (tracking) {
        d.pending.returned = true;
        if (index != NO_RECORD) {
            d.records[index].returned = true;
            d.records[index].error = result;
        }
        if (result != ESP_OK) { invalidate_locked(); d.error = result; }
        if (!d.pending.color_attempted) {
            d.pending.active = false; /* Error before tx_color cannot leave a color ISR. */
            if (result == ESP_OK) { invalidate_locked(); d.error = ESP_ERR_INVALID_STATE; }
        } else if (d.pending.completed) d.pending.active = false;
    }
    portEXIT_CRITICAL(&d.lock);
    return result;
}

static void display_changed(void)
{
    portENTER_CRITICAL(&d.lock);
    invalidate_locked();
    d.active = false;
    if (d.generation != UINT64_MAX) ++d.generation;
    else lose_association_locked();
    d.last_refresh = 0;
    portEXIT_CRITICAL(&d.lock);
}

static esp_err_t observe_reset(esp_lcd_panel_t *panel)
{
    display_changed();
    return d.native_panel.reset(panel);
}
static esp_err_t observe_init(esp_lcd_panel_t *panel)
{
    display_changed();
    return d.native_panel.init(panel);
}
static esp_err_t observe_mirror(esp_lcd_panel_t *panel, bool x, bool y)
{
    display_changed();
    return d.native_panel.mirror(panel, x, y);
}
static esp_err_t observe_swap(esp_lcd_panel_t *panel, bool swap)
{
    display_changed();
    return d.native_panel.swap_xy(panel, swap);
}
static esp_err_t observe_gap(esp_lcd_panel_t *panel, int x, int y)
{
    display_changed();
    if (x || y) {
        portENTER_CRITICAL(&d.lock); lose_association_locked(); portEXIT_CRITICAL(&d.lock);
    }
    return d.native_panel.set_gap(panel, x, y);
}
static esp_err_t observe_on(esp_lcd_panel_t *panel, bool on)
{
    display_changed();
    return d.native_panel.disp_on_off(panel, on);
}
static esp_err_t observe_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    display_changed();
    return d.native_panel.disp_sleep(panel, sleep);
}
static esp_err_t observe_panel_delete(esp_lcd_panel_t *panel)
{
    display_changed();
    portENTER_CRITICAL(&d.lock); d.installed = false; portEXIT_CRITICAL(&d.lock);
    return d.native_panel.del(panel);
}
static esp_err_t observe_io_delete(esp_lcd_panel_io_t *io)
{
    display_changed();
    portENTER_CRITICAL(&d.lock); d.installed = false; portEXIT_CRITICAL(&d.lock);
    return d.native_io.del(io);
}

esp_err_t board_display_transfer_install(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io)
{
    if (!panel || !io || !panel->draw_bitmap || !io->register_event_callbacks || !io->tx_color)
        return ESP_ERR_INVALID_ARG;
#if !CONFIG_BSP_CO5300_ENABLE_TE
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (d.panel || d.io) return ESP_ERR_INVALID_STATE; /* Boot-only singleton. */
    d.panel = panel;
    d.io = io;
    d.native_panel = *panel;
    d.native_io = *io;
    d.generation = 1;
    d.invalidation_revision = 1;
    panel->draw_bitmap = observe_draw;
    if (panel->reset) panel->reset = observe_reset;
    if (panel->init) panel->init = observe_init;
    if (panel->mirror) panel->mirror = observe_mirror;
    if (panel->swap_xy) panel->swap_xy = observe_swap;
    if (panel->set_gap) panel->set_gap = observe_gap;
    if (panel->disp_on_off) panel->disp_on_off = observe_on;
    if (panel->disp_sleep) panel->disp_sleep = observe_sleep;
    if (panel->del) panel->del = observe_panel_delete;
    io->register_event_callbacks = observe_register;
    io->tx_color = observe_color;
    if (io->del) io->del = observe_io_delete;
    d.installed = true;
    return ESP_OK;
}

esp_err_t board_display_transfer_begin(const board_display_token_t *token, const board_display_rect_t *required)
{
    if (!token || !valid_rect(required) || !token->capture_generation || !token->indicator_revision || !token->refresh_sequence)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&d.lock);
    d.token_valid = false;
    if (!d.installed || !d.attached || d.association_lost || d.pending.active ||
        token->display_generation != d.generation || token->refresh_sequence <= d.last_refresh) {
        portEXIT_CRITICAL(&d.lock); return ESP_ERR_INVALID_STATE;
    }
    if (d.token.capture_generation != token->capture_generation || d.token.indicator_revision != token->indicator_revision ||
        d.token.display_generation != token->display_generation || d.required.x0 != required->x0 ||
        d.required.y0 != required->y0 || d.required.x1 != required->x1 || d.required.y1 != required->y1) {
        invalidate_locked();
        if (d.association_lost) { portEXIT_CRITICAL(&d.lock); return ESP_ERR_INVALID_STATE; }
    }
    d.token = *token;
    d.required = *required;
    d.last_refresh = token->refresh_sequence;
    d.active = true;
    d.sealed = false;
    d.token_valid = true;
    d.error = ESP_OK;
    d.count = 0;
    memset(d.records, 0, sizeof(d.records));
    portEXIT_CRITICAL(&d.lock);
    return ESP_OK;
}

esp_err_t board_display_transfer_end(const board_display_token_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&d.lock);
    if (!d.active || !same_token(token, &d.token)) { portEXIT_CRITICAL(&d.lock); return ESP_ERR_INVALID_STATE; }
    d.sealed = true;
    portEXIT_CRITICAL(&d.lock);
    return ESP_OK;
}

void board_display_transfer_invalidate(void)
{
    portENTER_CRITICAL(&d.lock);
    invalidate_locked();
    d.active = false;
    portEXIT_CRITICAL(&d.lock);
}

static bool covered(const board_display_rect_t *required, const transfer_record_t *records, uint8_t count)
{
    int16_t edges[2U + 2U * BOARD_DISPLAY_TRANSFER_RECTS];
    size_t edge_count = 2;
    edges[0] = required->y0; edges[1] = required->y1;
    for (uint8_t i = 0; i < count; ++i) {
        if (!intersects(required, &records[i].rect)) continue;
        int16_t low = records[i].rect.y0 > required->y0 ? records[i].rect.y0 : required->y0;
        int16_t high = records[i].rect.y1 < required->y1 ? records[i].rect.y1 : required->y1;
        edges[edge_count++] = low; edges[edge_count++] = high;
    }
    for (size_t i = 1; i < edge_count; ++i) {
        int16_t value = edges[i]; size_t j = i;
        while (j && edges[j - 1] > value) { edges[j] = edges[j - 1]; --j; }
        edges[j] = value;
    }
    for (size_t band = 1; band < edge_count; ++band) {
        if (edges[band] == edges[band - 1]) continue;
        board_display_rect_t intervals[BOARD_DISPLAY_TRANSFER_RECTS]; size_t length = 0;
        for (uint8_t i = 0; i < count; ++i) {
            board_display_rect_t r = records[i].rect;
            if (r.y0 > edges[band - 1] || r.y1 < edges[band] || !intersects(required, &r)) continue;
            if (r.x0 < required->x0) r.x0 = required->x0;
            if (r.x1 > required->x1) r.x1 = required->x1;
            size_t j = length++;
            while (j && intervals[j - 1].x0 > r.x0) { intervals[j] = intervals[j - 1]; --j; }
            intervals[j] = r;
        }
        int16_t right = required->x0;
        for (size_t i = 0; i < length; ++i) {
            if (intervals[i].x0 > right) return false;
            if (intervals[i].x1 > right) right = intervals[i].x1;
        }
        if (right != required->x1) return false;
    }
    return true;
}

board_display_receipt_t board_display_transfer_receipt(void)
{
    transfer_record_t records[BOARD_DISPLAY_TRANSFER_RECTS];
    portENTER_CRITICAL(&d.lock);
    board_display_receipt_t receipt = {.installed = d.installed, .callback_attached = d.attached,
        .association_lost = d.association_lost, .refresh_sealed = d.sealed,
        .display_generation = d.generation, .token = d.token, .required = d.required,
        .invalidation_revision = d.invalidation_revision,
        .transfers = d.count, .error = d.error};
    bool valid = d.token_valid && d.active;
    memcpy(records, d.records, sizeof(records));
    portEXIT_CRITICAL(&d.lock);
    bool all_complete = receipt.transfers != 0;
    for (uint8_t i = 0; i < receipt.transfers; ++i) {
        if (records[i].returned && records[i].color_submitted && records[i].completed && records[i].error == ESP_OK) {
            ++receipt.completed_transfers;
            if (records[i].completed_us > receipt.completed_us) receipt.completed_us = records[i].completed_us;
        } else all_complete = false;
    }
    receipt.coverage_complete = all_complete && covered(&receipt.required, records, receipt.transfers);
    receipt.indicator_transfer_confirmed = valid && receipt.installed && receipt.callback_attached &&
        !receipt.association_lost && receipt.refresh_sealed && receipt.coverage_complete && receipt.error == ESP_OK;
    return receipt;
}
