// SPDX-License-Identifier: Apache-2.0
#include "board_display_resources.h"

#include "bsp/esp_mosaico.h"
#include "sdkconfig.h"

#if CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH < 1 || CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH > 2
#error "The Mosaico display SPI queue ceiling must be one or two transactions"
#endif

esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);

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
