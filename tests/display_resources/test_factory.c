// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_display_resources.h"
#include "bsp/esp_mosaico.h"
#include "sdkconfig.h"

static const esp_lcd_panel_io_spi_config_t *received_pointer;
static esp_lcd_panel_io_spi_config_t received;
static esp_lcd_spi_bus_handle_t received_bus;
static esp_err_t factory_error;
static unsigned int callbacks;
static int io_storage, context;
static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *user)
{
    (void)event; assert(io == (esp_lcd_panel_io_handle_t)&io_storage && user == &context);
    ++callbacks; return true;
}
esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t *config, esp_lcd_panel_io_handle_t *out)
{
    received_bus = bus; received_pointer = config;
    if (!config || !out) return ESP_ERR_INVALID_ARG;
    received = *config;
    if (factory_error != ESP_OK) return factory_error;
    *out = (esp_lcd_panel_io_handle_t)&io_storage;
    return ESP_OK;
}
int main(void)
{
    esp_lcd_panel_io_spi_config_t original = {.cs_gpio_num = BSP_LCD_CS, .dc_gpio_num = -1,
        .spi_mode = 3, .pclk_hz = 40000000, .trans_queue_depth = 10,
        .on_color_trans_done = color_done, .user_ctx = &context, .lcd_cmd_bits = 32,
        .lcd_param_bits = 8, .cs_ena_pretrans = 2, .cs_ena_posttrans = 4,
        .flags = {.quad_mode = true, .dc_high_on_cmd = true, .dc_low_on_data = true,
            .dc_low_on_param = true, .lsb_first = true, .cs_high_active = true}};
    esp_lcd_panel_io_spi_config_t before = original, expected = original;
    expected.trans_queue_depth = CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH;
    esp_lcd_panel_io_handle_t io = NULL;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, &io) == ESP_OK);
    assert(received_bus == BSP_LCD_SPI_HOST && received_pointer != &original);
    assert(!memcmp(&received, &expected, sizeof(expected)) && !memcmp(&original, &before, sizeof(before)));
    assert(!received.flags.psram_dma_direct && received.on_color_trans_done(io, NULL, received.user_ctx) && callbacks == 1);

    original.cs_gpio_num = BSP_LCD_CS - 1;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, &io) == ESP_OK);
    assert(received_pointer == &original && received.trans_queue_depth == 10);
    original.cs_gpio_num = BSP_LCD_CS;
    assert(__wrap_esp_lcd_new_panel_io_spi(SPI3_HOST, &original, &io) == ESP_OK);
    assert(received_pointer == &original && received_bus == SPI3_HOST && received.trans_queue_depth == 10);
    for (size_t depth = 0; depth <= CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH; ++depth) {
        original.trans_queue_depth = depth;
        assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, &io) == ESP_OK);
        assert(received_pointer == &original && received.trans_queue_depth == depth);
    }
    original.trans_queue_depth = 10; factory_error = ESP_ERR_NO_MEM;
    io = NULL;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, &io) == ESP_ERR_NO_MEM && !io);
    factory_error = ESP_ERR_INVALID_STATE;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, &io) == ESP_ERR_INVALID_STATE);
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, NULL, &io) == ESP_ERR_INVALID_ARG && !received_pointer);
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &original, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS: display factory targets only board host/CS, preserves borrowed config/callbacks and lower-layer errors");
    return 0;
}
