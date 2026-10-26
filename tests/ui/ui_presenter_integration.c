// SPDX-License-Identifier: Apache-2.0
/* Real production UI + presenter + Board observer and native LVGL. Only the
 * physical panel/IO, monotonic clock and task/ISR critical sections are seams. */
#include "app_ui.h"
#include "app_presenter.h"
#include "board_display_transfer.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "ui_memory.h"
#include <assert.h>
#include <stdio.h>

static esp_lcd_panel_t panel;
static esp_lcd_panel_io_t io;
static esp_lcd_panel_io_color_trans_done_cb_t native_callback;
static void *native_context;
static unsigned critical_depth, draws;
static bool in_isr;
static uint16_t pixels[480U * 480U];
void display_test_enter(int isr) { assert((isr != 0) == in_isr && critical_depth++ == 0); }
void display_test_leave(int isr) { assert((isr != 0) == in_isr && critical_depth-- == 1); }
int64_t esp_timer_get_time(void) { return (int64_t)lv_tick_get() * 1000; }
static esp_err_t physical_register(esp_lcd_panel_io_t *actual, const esp_lcd_panel_io_callbacks_t *callbacks, void *context)
{ assert(actual == &io && !critical_depth); native_callback = callbacks->on_color_trans_done; native_context = context; return ESP_OK; }
static esp_err_t physical_color(esp_lcd_panel_io_t *actual, int command, const void *data, size_t bytes)
{
    assert(actual == &io && command == 0x2c && data && bytes && !critical_depth && native_callback);
    esp_lcd_panel_io_event_data_t event = {0}; in_isr = true;
    assert(!native_callback(&io, &event, native_context)); in_isr = false;
    return ESP_OK;
}
static esp_err_t physical_draw(esp_lcd_panel_t *actual, int x0, int y0, int x1, int y1, const void *data)
{
    assert(actual == &panel && !critical_depth); ++draws;
    return io.tx_color(&io, 0x2c, data, (size_t)(x1 - x0) * (size_t)(y1 - y0) * 2U);
}
static bool adapter_done(esp_lcd_panel_io_handle_t actual, esp_lcd_panel_io_event_data_t *event, void *context)
{ assert(actual == &io && event && in_isr && !critical_depth); lv_display_flush_ready(context); return false; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)display; assert(panel.draw_bitmap(&panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, data) == ESP_OK); }
static bool no_intent(app_intent_t *intent) { (void)intent; assert(!"Rendering must not submit product actions"); return false; }
static void advance(unsigned milliseconds)
{ for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) { lv_tick_inc(10); lv_timer_handler(); } }
int main(void)
{
    lv_init(); lv_tick_inc(100);
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    panel = (esp_lcd_panel_t){.draw_bitmap = physical_draw};
    io = (esp_lcd_panel_io_t){.tx_color = physical_color, .register_event_callbacks = physical_register};
    assert(board_display_transfer_install(&panel, &io) == ESP_OK);
    esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = adapter_done};
    assert(io.register_event_callbacks(&io, &callbacks, display) == ESP_OK);
    app_ui_create(display, no_intent);
    assert(app_presenter_init() == ESP_OK && app_presenter_bind_display(display) == ESP_OK);
    app_view_t view = {.epoch = 1, .ui_revision = 1, .dark_theme = true,
        .capture_indicator_required = true, .capture_indicator_generation = 17, .capture_state = APP_CAPTURE_ACTIVE};
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); advance(200);
    board_display_receipt_t first_transfer = board_display_transfer_receipt();
    assert(first_transfer.indicator_transfer_confirmed && first_transfer.completed_us);
    /* The first status read may follow another refresh with no pixels to send. */
    unsigned completed_draws = draws;
    lv_refr_now(display);
    assert(draws == completed_draws && !board_display_transfer_receipt().transfers);
    app_presenter_indicator_status_t proof = app_presenter_indicator_status();
    assert(proof.usable && proof.capture_generation == 17 && proof.completed_us);
    assert(proof.completed_us == first_transfer.completed_us &&
        proof.refresh_sequence == first_transfer.token.refresh_sequence);
    unsigned before = draws; advance(1000);
    assert(draws == before && app_presenter_indicator_status().usable);
    assert(app_presenter_indicator_status().completed_us == proof.completed_us);
    /* Unknown drawing through the actual observed panel API covers the marker.
     * The presenter's old cached receipt must not survive this Board revocation. */
    assert(panel.draw_bitmap(&panel, 280, 8, 396, 56, pixels) == ESP_OK);
    assert(!board_display_transfer_receipt().indicator_transfer_confirmed);
    assert(!app_presenter_indicator_status().usable);
    /* An empty new refresh must not erase the fact of a missed overwrite. */
    lv_refr_now(display);
    assert(!app_presenter_indicator_status().usable);
    lv_obj_invalidate(lv_display_get_screen_active(display)); advance(30);
    assert(app_presenter_indicator_status().usable);
    /* The revocation must survive even if nobody polls status before begin. */
    assert(panel.draw_bitmap(&panel, 280, 8, 396, 56, pixels) == ESP_OK);
    lv_refr_now(display);
    assert(!app_presenter_indicator_status().usable);
    lv_obj_invalidate(lv_display_get_screen_active(display)); advance(30);
    assert(app_presenter_indicator_status().usable);
    uint64_t generation = app_presenter_indicator_status().display_generation;
    assert(io.register_event_callbacks(&io, &callbacks, display) == ESP_OK);
    assert(!app_presenter_indicator_status().usable);
    advance(200);
    assert(app_presenter_indicator_status().usable && app_presenter_indicator_status().display_generation > generation);
    view.screen_off = true; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); advance(120);
    assert(!app_presenter_indicator_status().usable);
    lv_display_delete(display); lv_deinit();
    ui_memory_stats_t memory = ui_memory_stats();
    assert(memory.live_bytes == 0 && memory.live_blocks == 0);
    puts("PASS: native LVGL + production UI/presenter/Board receipt, idle heartbeat, actual overwrite revocation, empty refresh and hidden screen");
    return 0;
}
