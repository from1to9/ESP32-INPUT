#include "passport_board.h"
#include <stdatomic.h>
#include "passport_widgets.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static bool display_ready;
static QueueHandle_t button_queue;
static atomic_bool overflow;
static atomic_uint observed_buttons;

esp_err_t passport_display_init(void)
{
    esp_err_t err = bsp_display_init();
    if (err != ESP_OK) {
        return err;
    }
    lv_display_t *display = bsp_lvgl_init();
    if (!display) {
        return ESP_FAIL;
    }
    if (!bsp_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    display_ready = passport_widgets_init(lv_display_get_screen_active(display));
    if (!display_ready) {
        bsp_lvgl_unlock();
        return ESP_ERR_NO_MEM;
    }
    static const passport_view_t starting = {
        .layout = VIEW_MESSAGE, .tone = TONE_WAIT, .hero_icon = ICON_LOCK,
        .title = "Starting", .heading = "Preparing input", .bluetooth = "BT ?", .wifi = "WiFi ?",
        .body = "Starting services...", .message = "Input unavailable", .ok = "Wait",
    };
    passport_widgets_update(&starting);
    bsp_lvgl_unlock();
    bsp_display_backlight(60);
    return ESP_OK;
}

static void button_pressed(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    static const unsigned bits[] = {PASSPORT_BUTTON_UP, PASSPORT_BUTTON_DOWN, PASSPORT_BUTTON_OK};
    static const passport_button_kind_t kinds[] = {
        [BSP_BTN_PRESS] = PASSPORT_PRESS, [BSP_BTN_CLICK] = PASSPORT_CLICKED,
        [BSP_BTN_DOUBLE] = PASSPORT_DOUBLE, [BSP_BTN_LONG] = PASSPORT_LONG,
        [BSP_BTN_RELEASE] = PASSPORT_RELEASE,
    };
    if (event == BSP_BTN_PRESS) atomic_fetch_or(&observed_buttons, bits[button]);
    const passport_button_event_t input = {
        .button = (passport_button_t)button, .kind = kinds[event],
        .time_ms = esp_timer_get_time() / 1000,
    };
    if (xQueueSend((QueueHandle_t)user, &input, 0) != pdTRUE) atomic_store(&overflow, true);
}

esp_err_t passport_buttons_init(void)
{
    button_queue = xQueueCreate(16, sizeof(passport_button_event_t));
    if (!button_queue) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = bsp_button_init(button_pressed, button_queue);
    if (err != ESP_OK) {
        vQueueDelete(button_queue);
        button_queue = NULL;
    }
    return err;
}

unsigned passport_buttons_observed(void)
{
    return atomic_load(&observed_buttons);
}

unsigned passport_button_click_guard_ms(void)
{
    // The pinned button driver completes a click after ticks > short_press_ticks.
    return BSP_BTN_SHORT_PRESS_MS + CONFIG_BUTTON_PERIOD_TIME_MS;
}

bool passport_render(const passport_view_t *view)
{
    if (!display_ready || !bsp_lvgl_lock(50)) return false;
    passport_widgets_update(view);
    bsp_lvgl_unlock();
    return true;
}

bool passport_next_button(passport_button_event_t *event)
{
    if (!button_queue) { vTaskDelay(pdMS_TO_TICKS(20)); return false; }
    return xQueueReceive(button_queue, event, pdMS_TO_TICKS(20)) == pdTRUE;
}

bool passport_button_overflow(void)
{
    bool lost = atomic_exchange(&overflow, false);
    if (lost && button_queue) xQueueReset(button_queue);
    return lost;
}

void passport_backlight(bool on)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    const uint64_t now = esp_timer_get_time() / 1000;
#endif
    bsp_display_backlight(on ? 60 : 0);
#if CONFIG_PASSPORT_DIAGNOSTICS
    if (passport_board_diagnostics()) {
        ESP_LOGI("passport_diag", "backlight=%u t=%llu", on, (unsigned long long)now);
    }
#endif
}

bool passport_board_diagnostics(void)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    static uint64_t started, next_sample, next_log;
    static int minimum, maximum, latest = -1;
    static unsigned samples, invalid;
    const uint64_t now = esp_timer_get_time() / 1000;
    if (!started) started = now;
    if (now - started >= 180000) return false;
    if (now >= next_sample) {
        next_sample = now + 10;
        // Normal UI waits are 20 ms. Aggregate any samples obtained between
        // logs, so a short low pulse need not coincide with a log timestamp.
        latest = bsp_button_read_mv();
        if (latest < 0) ++invalid;
        else {
            if (!samples || latest < minimum) minimum = latest;
            if (!samples || latest > maximum) maximum = latest;
            ++samples;
        }
    }
    if (now >= next_log) {
        next_log = now + 250;
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        // Independent calibrated reading from the BSP's shared ADC handle,
        // not the decoder's cached sample. Never feed it into event handling.
        ESP_LOGI("passport_diag", "t=%llu adc_mv=%d min=%d max=%d samples=%u invalid=%u glitches=%lu observed=%u heap=%zu largest=%zu",
                 (unsigned long long)now, latest, samples ? minimum : -1,
                 samples ? maximum : -1, samples, invalid,
                 (unsigned long)bsp_button_glitch_count(), passport_buttons_observed(),
                 heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps));
        samples = invalid = 0;
    }
    return true;
#else
    return false;
#endif
}
