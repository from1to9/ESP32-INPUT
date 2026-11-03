// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_display_resources.h"
#include "bsp/esp_mosaico.h"
#include "esp_lcd_panel_io_interface.h"
#include "sdkconfig.h"

/* Compile the pinned SDK LCD IO implementation unchanged except its factory
 * symbol name. Substitute only the SPI/DMA boundary: queued external color
 * data consumes bounded bounce storage until get_trans_result recycles it.
 * This proves the SDK's real chunk/recycle/callback path, not DMA hardware. */
enum { FRAME_BYTES = 480 * 480 * 2, CHUNK_BYTES = 4095, MAX_QUEUE = 10, LARGEST_BLOCK_BYTES = 7 * 1024 };
struct test_spi_device {
    spi_device_interface_config_t config;
    struct {
        spi_transaction_t *transaction;
        unsigned char *bounce;
        size_t bytes;
        bool completed;
    } queue[MAX_QUEUE];
    size_t count;
    bool acquired, present;
};
static struct test_spi_device spi;
static unsigned char *frame;
static size_t dma_limit, dma_used, dma_peak, queued_peak, transmitted, expected_bytes;
static unsigned int done_count, submitted_count, polling_count;
static int callback_context;
static esp_err_t add_error, queue_error;
static size_t bus_chunk_bytes;
static size_t largest_block_limit, fail_after;

esp_err_t __real_spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config, spi_dma_chan_t dma)
{
    assert(host == BSP_LCD_SPI_HOST && config && dma == SPI_DMA_CH_AUTO);
    /* Pinned S31 spicommon_dma_desc_alloc rounds requests to 4095-byte nodes.
     * This is the bus boundary model; the real LCD chunker below is compiled. */
    bus_chunk_bytes = ((size_t)config->max_transfer_sz + CHUNK_BYTES - 1U) / CHUNK_BYTES * CHUNK_BYTES;
    return ESP_OK;
}

bool esp_ptr_external_ram(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer, base = (uintptr_t)frame;
    return address >= base && address < base + FRAME_BYTES;
}
esp_err_t spi_bus_add_device(spi_host_device_t bus, const spi_device_interface_config_t *config,
    spi_device_handle_t *out)
{
    assert(bus == BSP_LCD_SPI_HOST && !spi.present);
    if (add_error != ESP_OK) return add_error;
    if (!config->queue_size || config->queue_size > MAX_QUEUE) return ESP_ERR_INVALID_ARG;
    spi.config = *config; spi.present = true; *out = &spi;
    assert(config->clock_speed_hz == 40000000 && config->spics_io_num == BSP_LCD_CS);
    return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    assert(handle == &spi && spi.present && !spi.count && !spi.acquired && !dma_used);
    spi.present = false; return ESP_OK;
}
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t bus, size_t *out)
{
    assert(bus == BSP_LCD_SPI_HOST && bus_chunk_bytes); *out = bus_chunk_bytes; return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t handle, unsigned int wait)
{
    (void)wait; assert(handle == &spi && spi.present && !spi.acquired);
    spi.acquired = true; return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t handle)
{
    assert(handle == &spi && spi.acquired); spi.acquired = false;
}
static void complete_first(void)
{
    assert(spi.count);
    if (spi.queue[0].completed) return;
    size_t bytes = spi.queue[0].bytes;
    assert(transmitted + bytes <= expected_bytes);
    assert(!memcmp(spi.queue[0].bounce, frame + transmitted, bytes));
    transmitted += bytes;
    spi.queue[0].completed = true;
    spi.config.post_cb(spi.queue[0].transaction);
}
esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **out, unsigned int wait)
{
    (void)wait; assert(handle == &spi && spi.count);
    complete_first();
    *out = spi.queue[0].transaction;
    dma_used -= spi.queue[0].bytes; free(spi.queue[0].bounce);
    --spi.count; memmove(&spi.queue[0], &spi.queue[1], spi.count * sizeof(spi.queue[0]));
    return ESP_OK;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *transaction)
{
    assert(handle == &spi && spi.acquired && !spi.count);
    assert(transaction->length == 32 && transaction->tx_buffer);
    ++polling_count; spi.config.pre_cb(transaction); spi.config.post_cb(transaction); return ESP_OK;
}
esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *transaction, unsigned int wait)
{
    (void)wait; assert(handle == &spi && spi.acquired && spi.count < spi.config.queue_size);
    if (queue_error != ESP_OK) return queue_error;
    if (submitted_count == fail_after) return ESP_ERR_NO_MEM;
    size_t bytes = transaction->length / 8U;
    assert(bytes && bytes <= bus_chunk_bytes && esp_ptr_external_ram(transaction->tx_buffer));
    assert((transaction->flags & SPI_TRANS_MODE_QIO) && !(transaction->flags & SPI_TRANS_DMA_USE_PSRAM));
    assert(esp_ptr_external_ram((const unsigned char *)transaction->tx_buffer + bytes - 1U));
    /* Individual allocation geometry is independent of total available bytes. */
    if (bytes > largest_block_limit) return ESP_ERR_NO_MEM;
    if (bytes > dma_limit - dma_used) return ESP_ERR_NO_MEM;
    unsigned char *bounce = malloc(bytes); assert(bounce); memcpy(bounce, transaction->tx_buffer, bytes);
    size_t queued_bytes = 0;
    for (size_t i = 0; i < spi.count; ++i) if (!spi.queue[i].completed) queued_bytes += spi.queue[i].bytes;
    bool last = transmitted + queued_bytes + bytes == expected_bytes;
    assert(((transaction->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0) == !last);
    spi.queue[spi.count].transaction = transaction;
    spi.queue[spi.count].bounce = bounce; spi.queue[spi.count].bytes = bytes;
    spi.queue[spi.count].completed = false; ++spi.count;
    dma_used += bytes; if (dma_used > dma_peak) dma_peak = dma_used;
    if (spi.count > queued_peak) queued_peak = spi.count;
    ++submitted_count; spi.config.pre_cb(transaction);
    return ESP_OK;
}
static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io; assert(!event && context == &callback_context);
    /* A TE/adapter waiter must not see completion before all viewport bytes. */
    assert(transmitted == expected_bytes); ++done_count; return true;
}
static esp_lcd_panel_io_spi_config_t config(void)
{
    return (esp_lcd_panel_io_spi_config_t){.cs_gpio_num = BSP_LCD_CS, .dc_gpio_num = -1,
        .pclk_hz = 40000000, .trans_queue_depth = 10, .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags = {.quad_mode = true}};
}
static void reset(size_t limit)
{
    assert(!spi.present && !dma_used); memset(&spi, 0, sizeof(spi));
    dma_limit = limit; dma_peak = queued_peak = transmitted = 0;
    done_count = submitted_count = polling_count = 0; add_error = queue_error = ESP_OK;
    expected_bytes = FRAME_BYTES;
    largest_block_limit = LARGEST_BLOCK_BYTES; fail_after = SIZE_MAX;
}
static esp_lcd_panel_io_handle_t create(bool bounded, bool bounded_chunks)
{
    spi_bus_config_t bus_config = {.mosi_io_num = BSP_LCD_DATA0, .miso_io_num = BSP_LCD_DATA1,
        .sclk_io_num = BSP_LCD_SCL_V1_2, .quadwp_io_num = BSP_LCD_DATA2, .quadhd_io_num = BSP_LCD_DATA3,
        .max_transfer_sz = 7680};
    if (bounded_chunks) assert(__wrap_spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) == ESP_OK);
    else assert(__real_spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) == ESP_OK);
    assert(bus_config.max_transfer_sz == 7680 && bus_chunk_bytes == (bounded_chunks ? CHUNK_BYTES : 8190U));
    esp_lcd_panel_io_spi_config_t request = config(); esp_lcd_panel_io_handle_t io = NULL;
    if (bounded) assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &request, &io) == ESP_OK);
    else {
        extern esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
            const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
        assert(__real_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &request, &io) == ESP_OK);
    }
    assert(request.trans_queue_depth == 10);
    esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = color_done};
    assert(io->register_event_callbacks(io, &callbacks, &callback_context) == ESP_OK);
    return io;
}
static void complete_all(void)
{
    /* Hardware completion may precede SDK result recycling. Leave the last
     * completed descriptor allocated, as the actual driver does. */
    for (size_t i = 0; i < spi.count; ++i) {
        if (spi.queue[i].completed) continue;
        size_t bytes = spi.queue[i].bytes;
        assert(!memcmp(spi.queue[i].bounce, frame + transmitted, bytes));
        transmitted += bytes; spi.queue[i].completed = true;
        spi.config.post_cb(spi.queue[i].transaction);
    }
}
int main(void)
{
    frame = malloc(FRAME_BYTES); assert(frame);
    for (size_t i = 0; i < FRAME_BYTES; ++i) frame[i] = (unsigned char)((i * 7U + i / 251U) & 255U);
    const size_t bound = CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH * CHUNK_BYTES;
    reset(bound); esp_lcd_panel_io_handle_t io = create(true, true);
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_OK);
    assert(!done_count && spi.count == CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH);
    assert(submitted_count == (FRAME_BYTES + CHUNK_BYTES - 1U) / CHUNK_BYTES && polling_count == 1);
    assert(dma_peak <= bound && queued_peak == CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH);
    complete_all(); assert(done_count == 1 && transmitted == FRAME_BYTES && dma_used > 0);
    assert(io->del(io) == ESP_OK && !dma_used);

    reset(bound); io = create(true, true); largest_block_limit = CHUNK_BYTES - 1U;
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_NO_MEM);
    assert(!submitted_count && !done_count); assert(io->del(io) == ESP_OK && !dma_used);
    reset(bound); io = create(true, true); fail_after = 3;
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_NO_MEM);
    assert(submitted_count == 3 && !done_count);
    assert(io->del(io) == ESP_OK && !done_count && !dma_used);

    /* Reproduce the single-allocation failure even with the queue already
     * bounded: the original eight-row bus needs an unavailable 8190-byte block. */
    reset(bound); io = create(true, false);
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_NO_MEM);
    assert(!submitted_count && !done_count && !dma_used);
    assert(io->del(io) == ESP_OK);

    /* The unmodified pinned queue needs more DMA storage for the same frame.
     * Failed partial transmission never produces a successful final callback. */
    reset(bound); io = create(false, true);
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_NO_MEM);
    assert(!done_count && submitted_count == CONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH);
    assert(io->del(io) == ESP_OK && !done_count && !dma_used);

    reset(bound - 1U); io = create(true, true);
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_NO_MEM);
    assert(!done_count); assert(io->del(io) == ESP_OK && !dma_used);
    reset(bound); io = create(true, true); queue_error = ESP_ERR_INVALID_STATE;
    assert(io->tx_color(io, 0x32002c00, frame, FRAME_BYTES) == ESP_ERR_INVALID_STATE && !done_count);
    assert(io->del(io) == ESP_OK);
    reset(bound); add_error = ESP_ERR_NO_MEM;
    esp_lcd_panel_io_spi_config_t request = config(); io = NULL;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &request, &io) == ESP_ERR_NO_MEM && !io);
    add_error = ESP_OK; request.trans_queue_depth = 0;
    assert(__wrap_esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &request, &io) == ESP_ERR_INVALID_ARG && !io);
    assert(!spi.present && !dma_used);
    free(frame);
    puts("PASS: pinned SDK full-frame chunks recycle within DMA ceiling; final callback and partial failure remain truthful");
    return 0;
}
