#include "passport_board.h"
#include <stdatomic.h>
#include "passport_widgets.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_timer.h"
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

void passport_backlight(bool on) { bsp_display_backlight(on ? 60 : 0); }
