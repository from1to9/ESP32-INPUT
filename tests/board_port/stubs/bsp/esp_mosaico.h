// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_lcd_types.h"
#define CONFIG_BSP_DISPLAY_ENABLE 1
#define CONFIG_BSP_DISPLAY_LVGL_ENABLE 1
#define CONFIG_BSP_MOTOR_ENABLE_PWM 1
#define BSP_BATTERY_I2C_ADDR 0x55
#define BSP_BATTERY_I2C_SPEED_HZ 400000
#define BSP_LCD_SPI_HOST SPI2_HOST
typedef enum { BSP_BOARD_VARIANT_V1_0, BSP_BOARD_VARIANT_V1_2 } bsp_board_variant_t;
typedef void *button_handle_t;
typedef void (*button_cb_t)(void *button, void *context);
typedef enum { BSP_BUTTON_AI, BSP_BUTTON_NUM } bsp_button_t;
typedef enum { BUTTON_PRESS_DOWN, BUTTON_PRESS_UP, BUTTON_LONG_PRESS_START, BUTTON_EVENT_MAX } button_event_t;
typedef union { struct { uint16_t press_time; } long_press; } button_event_args_t;
typedef struct { bool enable_touch; } bsp_display_config_t;
#define BSP_DISPLAY_DEFAULT_CONFIG() {.enable_touch = true}
typedef struct { uint8_t accel_odr, accel_range, gyro_odr, gyro_range; } bsp_imu_config_t;
#define BMI2_ACC_ODR_100HZ 8
#define BMI2_GYR_ODR_100HZ 8
#define BMI2_DRDY_GYR 0x40
#define BMI2_DRDY_ACC 0x80
#define BMI2_OK 0
#define BSP_IMU_CONFIG_DEFAULT() {.accel_odr = 8, .accel_range = 1, .gyro_odr = 8, .gyro_range = 0}
struct bmi2_dev { int identifier; };
esp_err_t bsp_board_variant_get(bsp_board_variant_t *variant);
esp_err_t bsp_power_init(void);
lv_display_t *bsp_display_start_with_config(const bsp_display_config_t *config);
esp_err_t bsp_display_new(const bsp_display_config_t *, esp_lcd_panel_handle_t *);
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(int32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int percent);
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_iot_button_create(button_handle_t buttons[], int *count, int capacity);
esp_err_t iot_button_register_cb(button_handle_t button, button_event_t event,
    button_event_args_t *arguments, button_cb_t callback, void *context);
esp_err_t iot_button_delete(button_handle_t button);
esp_err_t bsp_motor_init(void);
esp_err_t bsp_motor_set_strength(uint8_t strength);
esp_err_t bsp_motor_set(bool on);
esp_err_t bsp_imu_init(void);
esp_err_t bsp_imu_start(const bsp_imu_config_t *config);
esp_err_t bsp_imu_stop(void);
esp_err_t bsp_imu_deinit(void);
struct bmi2_dev *bsp_imu_get_handle(void);
int8_t bmi2_get_status(uint8_t *status, struct bmi2_dev *device);
esp_err_t bsp_imu_get_gyro(float *x, float *y, float *z);
esp_err_t bsp_imu_get_accel(float *x, float *y, float *z);
