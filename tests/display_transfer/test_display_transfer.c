// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/main/board_display_transfer.c"

static esp_lcd_panel_t panel;
static esp_lcd_panel_io_t io;
static esp_lcd_panel_io_color_trans_done_cb_t registered_callback;
static void *registered_context;
static esp_lcd_panel_io_event_data_t event;
static unsigned critical_depth, callbacks, alternate_callbacks, draws, colors, registrations, changes, deletions;
static bool in_isr, yield_result, early_completion, skip_color, fail_registration, reject_color, reject_draw;
static bool wrong_color_size;
static size_t expected_io_bytes;
static uint64_t time_us;
static uint16_t pixels[480U*480U];
static int original_context, alternate_context;
static board_display_token_t token;

void display_test_enter(int isr) { assert((isr != 0) == in_isr && critical_depth++ == 0); }
void display_test_leave(int isr) { assert((isr != 0) == in_isr && critical_depth-- == 1); }
int64_t esp_timer_get_time(void) { assert(in_isr && critical_depth == 1); return (int64_t)++time_us; }
static bool adapter_callback(esp_lcd_panel_io_handle_t actual_io, esp_lcd_panel_io_event_data_t *actual_event, void *context)
{
    assert(in_isr && !critical_depth && actual_io == &io && actual_event == &event && context == &original_context);
    ++callbacks; return yield_result;
}
static bool alternate_callback(esp_lcd_panel_io_handle_t actual_io, esp_lcd_panel_io_event_data_t *actual_event, void *context)
{
    assert(in_isr && !critical_depth && actual_io == &io && actual_event == &event && context == &alternate_context);
    ++alternate_callbacks; return yield_result;
}
static void complete(void)
{
    assert(registered_callback && !critical_depth && !in_isr);
    in_isr = true;
    bool wake = registered_callback(&io, &event, registered_context);
    in_isr = false;
    assert(wake == yield_result);
}
static esp_err_t native_register(esp_lcd_panel_io_t *actual_io, const esp_lcd_panel_io_callbacks_t *cbs, void *context)
{
    assert(actual_io == &io && cbs && !critical_depth); ++registrations;
    if (fail_registration) return ESP_FAIL;
    registered_callback = cbs->on_color_trans_done;
    registered_context = context;
    return ESP_OK;
}
static esp_err_t native_color(esp_lcd_panel_io_t *actual_io, int command, const void *actual_pixels, size_t bytes)
{
    assert(actual_io == &io && command == 0x2c && actual_pixels == pixels && bytes == expected_io_bytes && !critical_depth);
    ++colors;
    if (early_completion) {
        complete();
        /* ISR alone cannot publish success before draw_bitmap returns. */
        assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    }
    return reject_color ? ESP_FAIL : ESP_OK;
}
static esp_err_t native_draw(esp_lcd_panel_t *actual_panel, int x0, int y0, int x1, int y1, const void *actual_pixels)
{
    assert(actual_panel == &panel && actual_pixels == pixels && !critical_depth);
    assert(x0 < x1 && y0 < y1); ++draws;
    if (!skip_color) {
        expected_io_bytes=(size_t)(x1-x0)*(size_t)(y1-y0)*2U;
        if(wrong_color_size) expected_io_bytes-=2;
        esp_err_t result = io.tx_color(&io, 0x2c, actual_pixels, expected_io_bytes);
        if (result != ESP_OK) return result;
    }
    return reject_draw ? ESP_FAIL : ESP_OK;
}
static esp_err_t native_change(esp_lcd_panel_t *actual_panel) { assert(actual_panel == &panel && !critical_depth); ++changes; return ESP_OK; }
static esp_err_t native_mirror(esp_lcd_panel_t *actual_panel, bool x, bool y) { (void)x; (void)y; return native_change(actual_panel); }
static esp_err_t native_bool(esp_lcd_panel_t *actual_panel, bool value) { (void)value; return native_change(actual_panel); }
static esp_err_t native_gap(esp_lcd_panel_t *actual_panel, int x, int y) { (void)x; (void)y; return native_change(actual_panel); }
static esp_err_t native_delete(esp_lcd_panel_t *actual_panel) { assert(actual_panel == &panel && !critical_depth); ++deletions; return ESP_OK; }
static esp_err_t native_io_delete(esp_lcd_panel_io_t *actual_io) { assert(actual_io == &io && !critical_depth); ++deletions; return ESP_OK; }

static void reset(void)
{
    memset(&d, 0, sizeof(d));
    critical_depth = callbacks = alternate_callbacks = draws = colors = registrations = changes = deletions = 0;
    in_isr = yield_result = early_completion = skip_color = fail_registration = reject_color = reject_draw = false;
    wrong_color_size=false;
    registered_callback = NULL; registered_context = NULL; time_us = 1000;
    panel = (esp_lcd_panel_t){.draw_bitmap = native_draw, .reset = native_change, .init = native_change,
        .mirror = native_mirror, .swap_xy = native_bool, .set_gap = native_gap,
        .disp_on_off = native_bool, .disp_sleep = native_bool, .del = native_delete, .user_data = &pixels};
    io = (esp_lcd_panel_io_t){.tx_color = native_color, .register_event_callbacks = native_register, .del = native_io_delete};
    assert(board_display_transfer_install(&panel, &io) == ESP_OK);
    assert(panel.user_data == &pixels);
    esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = adapter_callback};
    assert(io.register_event_callbacks(&io, &cbs, &original_context) == ESP_OK);
    token = (board_display_token_t){.display_generation = 1, .capture_generation = 3, .indicator_revision = 1, .refresh_sequence = 1};
}
static void begin(board_display_rect_t rect)
{
    assert(board_display_transfer_begin(&token, &rect) == ESP_OK);
}
static void draw(board_display_rect_t rect)
{
    assert(panel.draw_bitmap(&panel, rect.x0, rect.y0, rect.x1, rect.y1, pixels) == ESP_OK);
    if (!early_completion) complete();
}
static void finish(bool confirmed)
{
    assert(board_display_transfer_end(&token) == ESP_OK);
    board_display_receipt_t receipt = board_display_transfer_receipt();
    assert(receipt.indicator_transfer_confirmed == confirmed);
    assert(same_token(&receipt.token, &token));
}

static void ordering_and_preservation(void)
{
    reset(); board_display_rect_t required = {10,20,30,40}; begin(required);
    assert(panel.draw_bitmap(&panel, 10,20,30,40,pixels) == ESP_OK);
    assert(board_display_transfer_end(&token) == ESP_OK);
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    yield_result = true; complete();
    board_display_receipt_t receipt = board_display_transfer_receipt();
    assert(receipt.indicator_transfer_confirmed && receipt.completed_transfers == 1 && receipt.completed_us == 1001);
    assert(callbacks == 1 && draws == 1 && colors == 1);
    ++token.refresh_sequence; begin(required); early_completion = true; draw(required); finish(true);
    assert(callbacks == 2);
    ++token.refresh_sequence; begin(required); skip_color = true; reject_draw = true;
    assert(panel.draw_bitmap(&panel, 10,20,30,40,pixels) == ESP_FAIL); finish(false);
    assert(board_display_transfer_receipt().error == ESP_FAIL && !d.pending.active);
    skip_color = reject_draw = early_completion = false;
    ++token.refresh_sequence; begin(required); draw(required); finish(true);
    esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = alternate_callback};
    fail_registration = true;
    assert(io.register_event_callbacks(&io, &cbs, &alternate_context) == ESP_FAIL);
    assert(d.callback == adapter_callback && d.callback_context == &original_context);
    fail_registration = false;
    assert(io.register_event_callbacks(&io, &cbs, &alternate_context) == ESP_OK);
    token.display_generation=board_display_transfer_receipt().display_generation;
    ++token.refresh_sequence; begin(required); draw(required); finish(true);
    assert(alternate_callbacks == 1);
    /* Actual pinned adapter unregister retains callback but passes NULL impl. */
    assert(io.register_event_callbacks(&io, &cbs, NULL) == ESP_OK && registered_callback==alternate_callback && !registered_context);
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    cbs.on_color_trans_done=NULL;
    assert(io.register_event_callbacks(&io,&cbs,NULL)==ESP_OK && !registered_callback);
}

static void coverage_and_stale_completion(void)
{
    reset(); board_display_rect_t area = {0,0,20,20}; begin(area);
    draw((board_display_rect_t){0,0,10,20});
    draw((board_display_rect_t){9,0,19,20});
    finish(false); /* Bounding box/overlap cannot hide the rightmost gap. */
    ++token.refresh_sequence; begin(area);
    draw((board_display_rect_t){0,0,10,20}); draw((board_display_rect_t){10,0,20,20}); finish(true);
    ++token.refresh_sequence; begin(area); draw((board_display_rect_t){30,30,40,40}); finish(false);
    ++token.refresh_sequence; begin(area); finish(false); /* Empty refresh. */
    ++token.refresh_sequence; begin(area);
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels) == ESP_OK);
    board_display_transfer_invalidate();
    board_display_token_t old = token;
    ++token.refresh_sequence; ++token.capture_generation;
    assert(board_display_transfer_begin(&token,&area) == ESP_ERR_INVALID_STATE);
    complete(); /* Old ISR is forwarded but cannot confirm the replacement. */
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    begin(area); draw(area);
    assert(board_display_transfer_end(&old) == ESP_ERR_INVALID_STATE);
    finish(true);
    assert(board_display_transfer_begin(&token,&area) == ESP_ERR_INVALID_STATE);
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    ++token.refresh_sequence; begin(area); draw(area); finish(true);
    draw((board_display_rect_t){30,30,40,40});
    assert(board_display_transfer_receipt().indicator_transfer_confirmed);
    draw(area); /* Unbound overlap invalidates an existing warning receipt. */
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
}

static void failure_and_changes(void)
{
    reset(); board_display_rect_t area={0,0,20,20}; begin(area);
    reject_color = true;
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_FAIL); finish(false);
    ++token.refresh_sequence;
    assert(board_display_transfer_begin(&token,&area)==ESP_ERR_INVALID_STATE);
    complete(); reject_color=false; begin(area); draw(area); finish(true);
    ++token.refresh_sequence; begin(area); reject_draw=true; early_completion=true;
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_FAIL); finish(false);
    assert(board_display_transfer_receipt().completed_transfers==0);
    reject_draw=false; early_completion=false;
    ++token.refresh_sequence; begin(area);
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_OK);
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_OK); /* Unexpected overlap. */
    complete(); finish(false);
    assert(board_display_transfer_receipt().association_lost);
    reset(); begin(area); early_completion=true;
    for(unsigned i=0;i<BOARD_DISPLAY_TRANSFER_RECTS+1U;++i) draw(area);
    finish(false); assert(board_display_transfer_receipt().association_lost);
    reset(); begin(area); draw(area); finish(true);
    assert(panel.mirror(&panel,true,false)==ESP_OK);
    assert(changes==1 && board_display_transfer_receipt().display_generation==2 && !board_display_transfer_receipt().indicator_transfer_confirmed);
    ++token.refresh_sequence;
    assert(board_display_transfer_begin(&token,&area)==ESP_ERR_INVALID_STATE);
    token.display_generation=2; begin(area); draw(area); finish(true);
    assert(panel.set_gap(&panel,1,0)==ESP_OK && board_display_transfer_receipt().association_lost);
    assert(panel.del(&panel)==ESP_OK && io.del(&io)==ESP_OK && deletions==2);
    assert(!board_display_transfer_receipt().installed && !board_display_transfer_receipt().indicator_transfer_confirmed);
    reset(); begin(area);
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_OK);
    assert(panel.del(&panel)==ESP_OK);
    complete(); /* Late completion still forwards the exact adapter callback. */
    assert(callbacks==1 && !board_display_transfer_receipt().indicator_transfer_confirmed);
    reset(); begin(area);
    assert(panel.draw_bitmap(&panel,0,0,20,20,pixels)==ESP_OK);
    esp_lcd_panel_io_callbacks_t changed={.on_color_trans_done=alternate_callback};
    assert(io.register_event_callbacks(&io,&changed,&alternate_context)==ESP_OK);
    assert(registered_callback==alternate_callback && registered_context==&alternate_context);
    yield_result=true; complete();
    assert(alternate_callbacks==1 && !board_display_transfer_receipt().indicator_transfer_confirmed);
    assert(board_display_transfer_receipt().association_lost);
    reset(); begin(area); wrong_color_size=true; draw(area); finish(false);
    assert(board_display_transfer_receipt().error==ESP_ERR_INVALID_SIZE && !board_display_transfer_receipt().coverage_complete);
}

static uint32_t random_state=1;
static void invalidation_epochs(void)
{
    reset(); board_display_rect_t area={0,0,20,20}; begin(area); draw(area); finish(true);
    uint64_t revision=board_display_transfer_receipt().invalidation_revision;
    assert(revision>0);
    (void)board_display_transfer_receipt(); draw((board_display_rect_t){30,30,40,40});
    assert(board_display_transfer_receipt().invalidation_revision==revision);
    ++token.refresh_sequence; begin(area); finish(false); /* Empty same-identity refresh preserves old pixel evidence. */
    assert(board_display_transfer_receipt().invalidation_revision==revision);
    ++token.refresh_sequence; begin(area); draw(area); finish(true);
    assert(board_display_transfer_receipt().invalidation_revision==revision);
    draw(area);
    assert(board_display_transfer_receipt().invalidation_revision>revision && !board_display_transfer_receipt().indicator_transfer_confirmed);
    revision=board_display_transfer_receipt().invalidation_revision;
    ++token.refresh_sequence; ++token.capture_generation; begin(area); draw(area); finish(true);
    assert(board_display_transfer_receipt().invalidation_revision>revision);
    revision=board_display_transfer_receipt().invalidation_revision;
    board_display_transfer_invalidate();
    assert(board_display_transfer_receipt().invalidation_revision>revision);
    revision=board_display_transfer_receipt().invalidation_revision;
    complete(); /* Unknown/extra callback never preserves a prior cached receipt. */
    assert(board_display_transfer_receipt().invalidation_revision>revision && board_display_transfer_receipt().association_lost);
    reset(); d.invalidation_revision=UINT64_MAX-1U;
    board_display_transfer_invalidate();
    assert(board_display_transfer_receipt().invalidation_revision==UINT64_MAX && board_display_transfer_receipt().association_lost);
    board_display_transfer_invalidate();
    assert(board_display_transfer_receipt().invalidation_revision==UINT64_MAX);
    assert(board_display_transfer_begin(&token,&area)==ESP_ERR_INVALID_STATE);
}
static unsigned random8(void) { random_state=random_state*1664525U+1013904223U; return random_state>>29; }
static void coverage_matches_pixel_union(void)
{
    for(unsigned trial=0;trial<1000;++trial) {
        reset(); board_display_rect_t area={0,0,8,8}; begin(area); bool grid[8][8]={{false}};
        unsigned count=1+random8()*4;
        for(unsigned i=0;i<count;++i) {
            int x0=(int)random8(), y0=(int)random8();
            int x1=x0+1+(int)(random8()%(unsigned)(8-x0));
            int y1=y0+1+(int)(random8()%(unsigned)(8-y0));
            draw((board_display_rect_t){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1});
            for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) grid[y][x]=true;
        }
        bool all=true;
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) all=all&&grid[y][x];
        finish(all);
    }
}

int main(void)
{
    assert(board_display_transfer_install(NULL,NULL)==ESP_ERR_INVALID_ARG);
    ordering_and_preservation(); coverage_and_stale_completion(); failure_and_changes(); invalidation_epochs(); coverage_matches_pixel_union();
    assert(!critical_depth);
    puts("Display observer: exact draw/ISR ordering, callback/context/yield preservation, split union, stale/error/overlap and pixel oracle passed.");
    return 0;
}
