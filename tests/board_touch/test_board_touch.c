// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/board_touch_mosaico.c"

static lv_indev_t device;
static uint8_t raw_report[15] = {6, 0, 0, 0, 0, 2, 0xab};
static esp_err_t read_raw(esp_lcd_panel_io_t *io, int command, void *buffer, size_t bytes);
static esp_lcd_panel_io_t panel_io = {.rx_param = read_raw};
static test_touch_t touch_device = {.io = &panel_io};
static esp_err_t read_raw(esp_lcd_panel_io_t *io, int command, void *buffer, size_t bytes)
{ assert(io == &panel_io && command == -1 && bytes == sizeof raw_report); memcpy(buffer, raw_report, bytes); return ESP_OK; }
static esp_lv_adapter_touch_callbacks_t installed;
static esp_lcd_touch_point_data_t source[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
static uint8_t source_count;
static unsigned reads, get_calls, snapshots;
static bool has_device, reject;
static esp_err_t install_error, read_error, get_error;
static board_touch_snapshot_t observed;
lv_indev_t *bsp_display_get_input_dev(void) { return has_device ? &device : NULL; }
int64_t esp_timer_get_time(void) { return 1234000; }
esp_err_t esp_lv_adapter_set_touch_callbacks(lv_indev_t *input, const esp_lv_adapter_touch_callbacks_t *callbacks)
{ assert(input == &device); installed = *callbacks; return install_error; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch)
{ assert(touch == &touch_device); ++reads;
    if (read_error) return read_error;
    uint8_t report[15]; return touch->io->rx_param(touch->io, -1, report, sizeof report); }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch, esp_lcd_touch_point_data_t *points,
    uint8_t *count, uint8_t capacity)
{
    assert(touch == &touch_device && capacity == CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    ++get_calls; if (get_error) return get_error;
    *count = source_count; memcpy(points, source, source_count * sizeof(*points)); return ESP_OK;
}
static bool observe(const board_touch_snapshot_t *sample)
{ observed = *sample; ++snapshots; return !reject; }
static esp_err_t scan(esp_lcd_touch_point_data_t *point, uint8_t *count)
{ return installed.custom_touch_read(&touch_device, point, count, 1, installed.user_ctx); }
int main(void)
{
    assert(board_touch_observe(NULL) == ESP_ERR_INVALID_STATE);
    assert(board_touch_observe(observe) == ESP_ERR_NOT_SUPPORTED);
    has_device = true; install_error = ESP_FAIL;
    assert(board_touch_observe(observe) == ESP_FAIL && !s_observer);
    install_error = ESP_OK;
    assert(board_touch_observe(observe) == ESP_OK);
    assert(board_touch_observe(observe) == ESP_ERR_INVALID_STATE);
    source[0] = (esp_lcd_touch_point_data_t){.track_id=7, .x=12, .y=34};
    source[1] = (esp_lcd_touch_point_data_t){.track_id=3, .x=56, .y=78};
    source_count = 2;
    esp_lcd_touch_point_data_t primary; uint8_t count = 9;
    assert(scan(&primary, &count) == ESP_OK && count == 1);
    assert(reads == 1 && get_calls == 1 && snapshots == 1);
    assert(primary.track_id == 7 && primary.x == 12 && primary.y == 34);
    assert(observed.count == 2 && observed.contacts[1].id == 3 && observed.contacts[1].y == 78);
    assert(observed.valid && !observed.overflow && observed.sequence == 1 && observed.timestamp_ms == 1234);
    read_error = ESP_FAIL;
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && !count);
    assert(reads == 2 && get_calls == 1 && !observed.valid && !observed.count);
    read_error = ESP_OK; get_error = ESP_FAIL;
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && !observed.valid);
    get_error = ESP_OK; source_count = 3;
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && observed.overflow && !count);
    source_count = 0;
    assert(scan(&primary, &count) == ESP_OK && observed.valid && !observed.count && !count);
    source_count = 2; reject = true;
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && !count);
    reject = false;
    source_count = 0; raw_report[6] = 0; /* Decoder would normalize this to release. */
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && !observed.valid && !count);
    raw_report[6] = 0xab; raw_report[5] = 3; source_count = 2; /* Decoder would clamp to two. */
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && observed.overflow && !observed.valid);
    raw_report[5] = 2;
    s_sequence = UINT64_MAX;
    assert(scan(&primary, &count) == ESP_ERR_INVALID_STATE && !observed.valid && observed.sequence == UINT64_MAX);
    puts("PASS: single-reader touch snapshots, complete contacts, error/overflow and primary ordering");
    return 0;
}
