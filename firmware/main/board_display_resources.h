// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_lcd_io_spi.h"

/* One descriptor on the tested S31 alignment (4095 bytes per descriptor).
 * The SDK reports the actual descriptor-rounded limit to the LCD IO factory. */
enum { BOARD_DISPLAY_SPI_TRANSFER_REQUEST_BYTES = 4092 };

/* Board-only link boundaries: cap the Mosaico bus transfer request and queue.
 * Each queued PSRAM color chunk otherwise retains an internal DMA bounce
 * buffer until the SDK recycles it. This does not change the draw buffer,
 * viewport, transfer protocol, TE synchronization or completion callbacks. */
esp_err_t __wrap_spi_bus_initialize(spi_host_device_t,
    const spi_bus_config_t *, spi_dma_chan_t);
esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
