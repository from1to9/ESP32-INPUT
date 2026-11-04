// SPDX-License-Identifier: Apache-2.0
#include "board_display_resources.h"

#include "bsp/esp_mosaico.h"
#include "sdkconfig.h"

#if CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH < 1 || CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH > 2
#error "The Mosaico display SPI queue ceiling must be one or two transactions"
#endif

esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
esp_err_t __real_spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *, spi_dma_chan_t);
esp_err_t __real_spi_bus_get_max_transaction_len(spi_host_device_t, size_t *);

esp_err_t __wrap_spi_bus_get_max_transaction_len(spi_host_device_t host, size_t *bytes)
{
    esp_err_t result = __real_spi_bus_get_max_transaction_len(host, bytes);
    /* SPI2 is the dedicated panel bus. The LCD IO factory caches this query,
     * not the BSP's unrounded request, as its actual per-color chunk size.
     * Bound that size without reserving heap or changing the SPI DMA geometry.
     * Smaller hardware limits and errors retain the SDK's result. */
    if (result == ESP_OK && host == BSP_LCD_SPI_HOST && *bytes > BOARD_DISPLAY_SPI_COLOR_CHUNK_BYTES)
        *bytes = BOARD_DISPLAY_SPI_COLOR_CHUNK_BYTES;
    return result;
}

esp_err_t __wrap_spi_bus_initialize(spi_host_device_t host,
    const spi_bus_config_t *config, spi_dma_chan_t dma)
{
    if (host != BSP_LCD_SPI_HOST || !config || dma != SPI_DMA_CH_AUTO ||
        (config->sclk_io_num != BSP_LCD_SCL_V1_0 && config->sclk_io_num != BSP_LCD_SCL_V1_2) ||
        config->mosi_io_num != BSP_LCD_DATA0 || config->miso_io_num != BSP_LCD_DATA1 ||
        config->quadwp_io_num != BSP_LCD_DATA2 || config->quadhd_io_num != BSP_LCD_DATA3 ||
        config->max_transfer_sz <= BOARD_DISPLAY_SPI_TRANSFER_REQUEST_BYTES) {
        return __real_spi_bus_initialize(host, config, dma);
    }
    /* The BSP's eight-row request becomes two DMA descriptors (8190 bytes on
     * S31). One queued chunk still needs that much contiguous internal memory.
     * Bound the BSP request; let the unchanged LCD driver split
     * the full payload and issue its one final completion callback. */
    spi_bus_config_t bounded = *config;
    bounded.max_transfer_sz = BOARD_DISPLAY_SPI_TRANSFER_REQUEST_BYTES;
    return __real_spi_bus_initialize(host, &bounded, dma);
}

esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t *config, esp_lcd_panel_io_handle_t *out)
{
    if (bus != (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST || !config ||
        config->cs_gpio_num != BSP_LCD_CS ||
        config->trans_queue_depth <= CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH) {
        return __real_esp_lcd_new_panel_io_spi(bus, config, out);
    }
    /* Keep the pinned BSP's stable internal-bounce path and exact callbacks.
     * The SDK still splits and transmits the complete color payload, recycling
     * one completed chunk before admitting the next at the configured ceiling.
     * Invalid zero-depth requests pass unchanged to the SDK's validation. */
    esp_lcd_panel_io_spi_config_t bounded = *config;
    bounded.trans_queue_depth = CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH;
    return __real_esp_lcd_new_panel_io_spi(bus, &bounded, out);
}
