// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_lcd_io_spi.h"

/* Board-only link boundary: cap the Mosaico panel's queued SPI transactions.
 * Each queued PSRAM color chunk otherwise retains an internal DMA bounce
 * buffer until the SDK recycles it. This does not change the draw buffer,
 * viewport, transfer protocol, TE synchronization or completion callbacks. */
esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
