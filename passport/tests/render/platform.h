#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define BSP_LCD_W 240
#define BSP_BTN_SHORT_PRESS_MS 180
#define CONFIG_BUTTON_PERIOD_TIME_MS 5
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef void *QueueHandle_t;
typedef unsigned TickType_t;
typedef enum {BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK} bsp_btn_t;
typedef enum {BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG, BSP_BTN_RELEASE} bsp_btn_ev_t;
typedef void (*bsp_btn_cb_t)(bsp_btn_t,bsp_btn_ev_t,void *);
esp_err_t bsp_display_init(void);
lv_display_t *bsp_lvgl_init(void);
bool bsp_lvgl_lock(int); void bsp_lvgl_unlock(void);void bsp_display_backlight(uint8_t);
esp_err_t bsp_button_init(bsp_btn_cb_t,void *);
QueueHandle_t xQueueCreate(unsigned,unsigned);void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t,const void *,unsigned);int xQueueReceive(QueueHandle_t,void *,unsigned);int xQueueReset(QueueHandle_t);
void vTaskDelay(unsigned);int64_t esp_timer_get_time(void);
