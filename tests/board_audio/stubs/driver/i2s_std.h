// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct test_i2s_channel *i2s_chan_handle_t;
typedef struct { uint32_t dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct { uint32_t rate; int mclk_multiple; } test_clock_t;
typedef struct { int bits, mode; } test_slot_t;
typedef struct { int mclk, bclk, ws, dout, din; } test_gpio_t;
typedef struct { test_clock_t clk_cfg; test_slot_t slot_cfg; test_gpio_t gpio_cfg; } i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_MCLK_MULTIPLE_256 256
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate_) { .rate = (rate_) }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_, mode_) { .bits = (bits_), .mode = (mode_) }
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t, const void *, size_t, size_t *);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, uint32_t);
