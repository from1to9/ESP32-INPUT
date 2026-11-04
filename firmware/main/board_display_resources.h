// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_lcd_io_spi.h"

/* Bound controller descriptor storage. SDK rounding can raise the actual
 * capacity above this request; color chunks have a separate ceiling below. */
enum { BOARD_DISPLAY_SPI_TRANSFER_REQUEST_BYTES = 4092 };
/* Cap the value consumed by the LCD color chunker after descriptor rounding. */
enum { BOARD_DISPLAY_SPI_COLOR_CHUNK_BYTES = 2048 };

/* Board-only link boundaries: cap the dedicated panel bus and color queue.
 * Each queued PSRAM color chunk otherwise retains an internal DMA bounce
 * buffer until the SDK recycles it. This does not change the draw buffer,
 * viewport, transfer protocol, TE synchronization or completion callbacks. */
esp_err_t __wrap_spi_bus_initialize(spi_host_device_t,
    const spi_bus_config_t *, spi_dma_chan_t);
esp_err_t __wrap_spi_bus_get_max_transaction_len(spi_host_device_t, size_t *);
esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
