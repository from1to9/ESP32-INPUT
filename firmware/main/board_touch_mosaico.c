// SPDX-License-Identifier: Apache-2.0
#include "board_touch_port.h"
#include "bsp/esp_mosaico.h"
#include "esp_lv_adapter_input.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io_interface.h"
#include <limits.h>

_Static_assert(CONFIG_ESP_LCD_TOUCH_MAX_POINTS >= BOARD_TOUCH_CONTACTS,
               "The driver must retain both real contacts");
static board_touch_snapshot_fn s_observer;
static uint64_t s_sequence;
static esp_lcd_panel_io_t *s_io;
static esp_err_t (*s_original_read)(esp_lcd_panel_io_t *, int, void *, size_t);
static bool s_read_active, s_raw_seen, s_raw_valid, s_raw_overflow;

/* Observe the SAME report read by the pinned CST9220 decoder, before it can
 * normalize a malformed header to zero contacts or clamp an excessive count.
 * Forward the original return unchanged so the decoder still performs its
 * protocol-specific report ACK. No second read or transport ownership change.
 * The pinned read_data performs exactly one 15-byte report read with cmd=-1. */
static esp_err_t observe_report(esp_lcd_panel_io_t *io, int command, void *buffer, size_t bytes)
{
    esp_err_t result = s_original_read(io, command, buffer, bytes);
    if (s_read_active) {
        bool shape = !s_raw_seen && io == s_io && command == -1 && buffer && bytes == 15;
        s_raw_seen = true;
        if (shape && result == ESP_OK) {
            const uint8_t *report = buffer;
            s_raw_overflow = (report[5] & 0x7fU) > BOARD_TOUCH_CONTACTS;
            s_raw_valid = !s_raw_overflow && (report[0] == 0 ||
                (report[0] != 0xab && report[6] == 0xab));
        } else s_raw_valid = false;
    }
    return result;
}

static esp_err_t reject_scan(void)
{
    if (s_sequence != UINT64_MAX) ++s_sequence;
    board_touch_snapshot_t invalid = {.sequence = s_sequence,
        .timestamp_ms = (uint64_t)esp_timer_get_time() / 1000U};
    if (s_observer) (void)s_observer(&invalid);
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t read_contacts(esp_lcd_touch_handle_t touch,
    esp_lcd_touch_point_data_t *primary, uint8_t *count, uint8_t capacity, void *context)
{
    (void)context;
    if (!primary || !count || !capacity || !s_observer) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (!touch || !touch->io || !touch->io->rx_param) return reject_scan();
    if (!s_io) {
        s_io = touch->io; s_original_read = s_io->rx_param;
        s_io->rx_param = observe_report;
    }
    if (touch->io != s_io || s_io->rx_param != observe_report || s_read_active) return reject_scan();
    s_raw_seen = s_raw_valid = s_raw_overflow = false;
    s_read_active = true;
    esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t found = 0;
    esp_err_t result = esp_lcd_touch_read_data(touch);
    s_read_active = false;
    if (result == ESP_OK)
        result = esp_lcd_touch_get_data(touch, points, &found, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    board_touch_snapshot_t sample = {.timestamp_ms = (uint64_t)esp_timer_get_time() / 1000U,
        .valid = result == ESP_OK && s_raw_seen && s_raw_valid,
        .overflow = s_raw_overflow || found > BOARD_TOUCH_CONTACTS};
    if (s_sequence != UINT64_MAX) ++s_sequence;
    else sample.valid = false;
    sample.sequence = s_sequence;
    sample.count = found > BOARD_TOUCH_CONTACTS ? BOARD_TOUCH_CONTACTS : found;
    for (uint8_t i = 0; i < sample.count; ++i)
        sample.contacts[i] = (board_touch_contact_t){.id = points[i].track_id,
            .x = points[i].x, .y = points[i].y};
    /* The pinned BSP configures scale 1:1 and panel rotation in its driver;
     * esp_lcd_touch_get_data already applies the corresponding touch transform.
     * A new board with a different logical scale must supply its own port. */
    if (!s_observer(&sample)) return ESP_ERR_INVALID_STATE;
    if (result != ESP_OK || sample.overflow || !sample.valid) return ESP_ERR_INVALID_STATE;
    uint8_t returned = found < capacity ? found : capacity;
    for (uint8_t i = 0; i < returned; ++i) primary[i] = points[i];
    *count = returned;
    return ESP_OK;
}

esp_err_t board_touch_observe(board_touch_snapshot_fn observer)
{
    if (!observer || s_observer) return ESP_ERR_INVALID_STATE;
    lv_indev_t *input = bsp_display_get_input_dev();
    if (!input) return ESP_ERR_NOT_SUPPORTED;
    s_observer = observer;
    esp_lv_adapter_touch_callbacks_t callbacks = {.custom_touch_read = read_contacts};
    esp_err_t result = esp_lv_adapter_set_touch_callbacks(input, &callbacks);
    if (result != ESP_OK) s_observer = NULL;
    return result;
}
