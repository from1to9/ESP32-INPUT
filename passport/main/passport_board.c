#include "passport_board.h"
#include <stdatomic.h>
#include <stdio.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static lv_obj_t *status_label;
static QueueHandle_t button_queue;
static const char *last_button = "Waiting for a press";
static atomic_uint observed_buttons;
// Only change the persistent LVGL text while holding its lock.
static char status_text[512];

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
    status_label = lv_label_create(lv_display_get_screen_active(display));
    if (!status_label) {
        bsp_lvgl_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(status_label, BSP_LCD_W - 40);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_label_set_text_static(status_label, "Passport C3 health\nStarting...");
    bsp_lvgl_unlock();
    bsp_display_backlight(60);
    return ESP_OK;
}

static void button_pressed(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    if (event == BSP_BTN_PRESS) {
        static const unsigned bits[] = {
            [BSP_BTN_UP] = PASSPORT_BUTTON_UP,
            [BSP_BTN_DOWN] = PASSPORT_BUTTON_DOWN,
            [BSP_BTN_OK] = PASSPORT_BUTTON_OK,
        };
        atomic_fetch_or(&observed_buttons, bits[button]);
        // A single slot keeps the newest observation during startup or rendering.
        xQueueOverwrite((QueueHandle_t)user, &button);
    }
}

esp_err_t passport_buttons_init(void)
{
    button_queue = xQueueCreate(1, sizeof(bsp_btn_t));
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

bool passport_show_status(const char *network_text, const char *http_status,
                          const char *nimble_status, esp_err_t button_status)
{
    if (status_label && bsp_lvgl_lock(1000)) {
        snprintf(status_text, sizeof(status_text),
                 "Passport C3 health\n%s\nHTTP: %s\nBLE: %s\nKey: %s",
                 network_text, http_status, nimble_status,
                 button_status == ESP_OK ? last_button : esp_err_to_name(button_status));
        lv_label_set_text_static(status_label, status_text);
        bsp_lvgl_unlock();
        return true;
    }
    return false;
}

bool passport_wait_for_button(void)
{
    const TickType_t display_poll = pdMS_TO_TICKS(1000);
    if (!button_queue) {
        vTaskDelay(display_poll);
        return false;
    }
    bsp_btn_t button;
    if (xQueueReceive(button_queue, &button, display_poll) == pdTRUE) {
        static const char *const names[] = {
            [BSP_BTN_UP] = "UP",
            [BSP_BTN_DOWN] = "DOWN",
            [BSP_BTN_OK] = "OK",
        };
        last_button = names[button];
        ESP_LOGI("passport_buttons", "Pressed %s", last_button);
        return true;
    }
    return false;
}
