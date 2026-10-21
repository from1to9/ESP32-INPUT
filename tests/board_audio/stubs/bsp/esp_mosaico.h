// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"
#define BSP_I2C_PORT 0
#define BSP_AUDIO_CODEC_I2C_ADDR (0x19U << 1)
#define BSP_AUDIO_PA_CTRL 45
#define BSP_AUDIO_I2S_MCLK 54
#define BSP_AUDIO_I2S_SCLK 37
#define BSP_AUDIO_I2S_LRCLK 49
#define BSP_AUDIO_I2S_SDOUT 52
#define BSP_AUDIO_I2S_DSIN 40
#define BSP_NAND_SPI_HOST 3
#define BSP_NAND_CLK 20
#define BSP_NAND_D 21
#define BSP_NAND_Q 22
#define BSP_NAND_CS 23
#define BSP_NAND_HOLD 24
#define BSP_NAND_WP 25
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_power_set_vcc_3v3(bool);
esp_err_t bsp_power_set_codec_3v3(bool);
void *bsp_nand_flash_get_handle(void);
