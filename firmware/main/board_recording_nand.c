// SPDX-License-Identifier: Apache-2.0
#include "board_recording_nand.h"

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PROBE_WAIT_MS 10U

static struct {
    portMUX_TYPE lock;
    board_nand_status_t status;
    bool bus_owned;
    bool transaction_pending;
    spi_device_handle_t device;
    /* Retained if an SDK completion times out; never free DMA-owned storage. */
    spi_transaction_ext_t transaction;
} n = { .lock = portMUX_INITIALIZER_UNLOCKED, .status = {.result = AE_UNAVAILABLE} };

static esp_err_t read_only_command(uint8_t command, uint8_t address, size_t bytes, uint8_t *output)
{
    /* Only raw identification and feature reads. In particular, do not invoke
     * spi_nand_flash_read_page: its ECC refresh path may program NAND. */
    if ((command != 0x9f && command != 0x0f) || !bytes || bytes > 3 || n.transaction_pending)
        return ESP_ERR_INVALID_ARG;
    memset(&n.transaction, 0, sizeof(n.transaction));
    n.transaction = (spi_transaction_ext_t) {
        .base = { .flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
            SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA,
            .rxlength = bytes * 8U, .addr = address, .cmd = command },
        .address_bits = 8, .command_bits = 8,
    };
    esp_err_t error = spi_device_queue_trans(n.device, &n.transaction.base, pdMS_TO_TICKS(PROBE_WAIT_MS));
    if (error != ESP_OK) return error;
    n.transaction_pending = true;
    spi_transaction_t *completed = NULL;
    error = spi_device_get_trans_result(n.device, &completed, pdMS_TO_TICKS(PROBE_WAIT_MS));
    if (error != ESP_OK) return error;
    n.transaction_pending = false;
    if (completed != &n.transaction.base) return ESP_FAIL;
    memcpy(output, completed->rx_data, bytes);
    return ESP_OK;
}

static esp_err_t control_pin(int pin)
{
    esp_err_t error = gpio_set_level(pin, 1);
    if (error != ESP_OK) return error;
    gpio_config_t config = { .pin_bit_mask = UINT64_C(1) << pin,
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    error = gpio_config(&config);
    return error == ESP_OK ? gpio_set_level(pin, 1) : error;
}

static void probe(void *unused)
{
    (void)unused;
    board_nand_status_t status = {.state = BOARD_NAND_PROBE_FAILED, .result = AE_IO_ERROR};
    esp_err_t error = ESP_OK;
    /* A BSP owner may have mounted unknown factory assets. Do not reuse, sync,
     * detach, change bus configuration or take ownership of its handle. */
    if (bsp_nand_flash_get_handle()) { status.result = AE_BUSY; goto done; }
    spi_bus_config_t bus = { .mosi_io_num = BSP_NAND_D, .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 64 };
    error = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        status.result = error == ESP_ERR_INVALID_STATE ? AE_BUSY : AE_IO_ERROR;
        goto done;
    }
    n.bus_owned = true;
    error = control_pin(BSP_NAND_HOLD);
    if (error != ESP_OK) goto cleanup;
    error = control_pin(BSP_NAND_WP);
    if (error != ESP_OK) goto cleanup;
    spi_device_interface_config_t device = { .clock_speed_hz = 1000000,
        .spics_io_num = BSP_NAND_CS, .queue_size = 1, .flags = SPI_DEVICE_HALFDUPLEX };
    error = spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &n.device);
    if (error != ESP_OK) goto cleanup;
    error = read_only_command(0x9f, 0, 3, status.jedec_id);
    if (error != ESP_OK) goto cleanup;
    if ((status.jedec_id[0] == 0 && status.jedec_id[1] == 0 && status.jedec_id[2] == 0) ||
        (status.jedec_id[0] == 255 && status.jedec_id[1] == 255 && status.jedec_id[2] == 255)) {
        error = ESP_FAIL;
        goto cleanup;
    }
    status.identification_observed = true;
    error = read_only_command(0x0f, 0xb0, 1, &status.configuration_register);
    if (error != ESP_OK) goto cleanup;
    error = read_only_command(0x0f, 0xc0, 1, &status.status_register);
    if (error != ESP_OK) goto cleanup;
    status.state = BOARD_NAND_NEEDS_BACKUP_AND_MIGRATION;
    status.result = AE_OK; /* Probe observed hardware; store remains unavailable. */
cleanup:
    if (!n.transaction_pending && n.device) {
        esp_err_t cleanup_error = spi_bus_remove_device(n.device);
        if (cleanup_error == ESP_OK) n.device = NULL;
        else { status.state = BOARD_NAND_CLEANUP_UNCERTAIN; status.result = AE_UNCERTAIN; if (error == ESP_OK) error = cleanup_error; }
    }
    if (!n.transaction_pending && !n.device && n.bus_owned) {
        esp_err_t cleanup_error = spi_bus_free(BSP_NAND_SPI_HOST);
        if (cleanup_error == ESP_OK) n.bus_owned = false;
        else { status.state = BOARD_NAND_CLEANUP_UNCERTAIN; status.result = AE_UNCERTAIN; if (error == ESP_OK) error = cleanup_error; }
    }
    if (n.transaction_pending) { status.state = BOARD_NAND_CLEANUP_UNCERTAIN; status.result = AE_UNCERTAIN; }
done:
    status.sdk_error = error;
    status.cleanup_complete = !n.bus_owned && !n.device && !n.transaction_pending;
    if (error != ESP_OK && status.result != AE_UNCERTAIN) {
        status.state = BOARD_NAND_PROBE_FAILED;
        if (error == ESP_ERR_TIMEOUT) status.result = AE_TIMEOUT;
    }
    portENTER_CRITICAL(&n.lock);
    n.status = status;
    portEXIT_CRITICAL(&n.lock);
    vTaskDelete(NULL);
}

ae_result_t board_recording_nand_probe_start(void)
{
    portENTER_CRITICAL(&n.lock);
    if (n.status.state != BOARD_NAND_NOT_PROBED) {
        ae_result_t result = n.status.state == BOARD_NAND_PROBING ? AE_AGAIN : n.status.result;
        portEXIT_CRITICAL(&n.lock);
        return result;
    }
    n.status.state = BOARD_NAND_PROBING;
    n.status.result = AE_AGAIN;
    portEXIT_CRITICAL(&n.lock);
    if (xTaskCreate(probe, "nand_inventory", 4096, NULL, 2, NULL) == pdPASS) return AE_AGAIN;
    portENTER_CRITICAL(&n.lock);
    n.status.state = BOARD_NAND_PROBE_FAILED;
    n.status.result = AE_EXHAUSTED;
    n.status.cleanup_complete = true;
    portEXIT_CRITICAL(&n.lock);
    return AE_EXHAUSTED;
}

board_nand_status_t board_recording_nand_status(void)
{
    portENTER_CRITICAL(&n.lock);
    board_nand_status_t status = n.status;
    portEXIT_CRITICAL(&n.lock);
    return status;
}

ae_store_capabilities_t board_recording_nand_capabilities(void)
{
    board_nand_status_t status = board_recording_nand_status();
    ae_result_t reason = status.state == BOARD_NAND_NEEDS_BACKUP_AND_MIGRATION ? AE_PRIVACY :
        status.state == BOARD_NAND_NOT_PROBED ? AE_UNAVAILABLE : status.result;
    return (ae_store_capabilities_t) {.reason = reason};
}
