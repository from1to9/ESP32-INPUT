// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/main/board_recording_nand.c"

static unsigned lock_depth, calls, bus_claims, bus_frees, device_removes, reads;
static bool existing_owner, bus_busy, timeout_completion, remove_failure, free_failure, empty_id, task_failure;
static TaskFunction_t scheduled;
static spi_transaction_t *pending;
static int device_token;

void audio_test_enter(portMUX_TYPE *lock) { (void)lock; assert(lock_depth++ == 0); }
void audio_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth-- == 1); }
static void sdk(void) { assert(lock_depth == 0); ++calls; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *task)
{
    sdk(); assert(function && name && stack == 4096 && !argument && priority == 2 && !task);
    if (task_failure) return 0;
    scheduled = function; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { sdk(); assert(!task); }
void *bsp_nand_flash_get_handle(void) { sdk(); return existing_owner ? &device_token : NULL; }
esp_err_t gpio_set_level(int pin, int level) { sdk(); assert((pin == 24 || pin == 25) && level == 1); return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config)
{
    sdk(); assert(config->pin_bit_mask == (UINT64_C(1) << 24) || config->pin_bit_mask == (UINT64_C(1) << 25));
    assert(config->mode == GPIO_MODE_OUTPUT && config->pull_up_en == GPIO_PULLUP_ENABLE); return ESP_OK;
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    sdk(); ++bus_claims; assert(host == 3 && dma == SPI_DMA_CH_AUTO);
    assert(config->mosi_io_num == 21 && config->miso_io_num == 22 && config->sclk_io_num == 20);
    assert(config->quadwp_io_num == -1 && config->quadhd_io_num == -1);
    return bus_busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t spi_bus_free(int host) { sdk(); assert(host == 3 && !pending); ++bus_frees; return free_failure ? ESP_FAIL : ESP_OK; }
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device)
{
    sdk(); assert(host == 3 && config->spics_io_num == 23 && config->clock_speed_hz == 1000000);
    assert(config->queue_size == 1 && config->flags == SPI_DEVICE_HALFDUPLEX);
    *device = &device_token; return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t device)
{
    sdk(); assert(device == &device_token && !pending); ++device_removes; return remove_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t spi_device_queue_trans(spi_device_handle_t device, spi_transaction_t *transaction, TickType_t timeout)
{
    sdk(); assert(device == &device_token && timeout == 10 && !pending);
    assert(transaction->cmd == 0x9f || transaction->cmd == 0x0f);
    assert(transaction->flags == (SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA));
    const spi_transaction_ext_t *extended = (const spi_transaction_ext_t *)transaction;
    assert(extended->command_bits == 8 && extended->address_bits == 8 && extended->dummy_bits == 0);
    if (transaction->cmd == 0x9f) {
        assert(transaction->rxlength == 24 && transaction->addr == 0);
        transaction->rx_data[0] = empty_id ? 255 : 0xc8;
        transaction->rx_data[1] = empty_id ? 255 : 0x91;
        transaction->rx_data[2] = empty_id ? 255 : 0;
    } else {
        assert(transaction->rxlength == 8 && (transaction->addr == 0xb0 || transaction->addr == 0xc0));
        transaction->rx_data[0] = transaction->addr == 0xb0 ? 0x10 : 0x20;
    }
    ++reads; pending = transaction; return ESP_OK;
}
esp_err_t spi_device_get_trans_result(spi_device_handle_t device, spi_transaction_t **transaction, TickType_t timeout)
{
    sdk(); assert(device == &device_token && pending && timeout == 10);
    if (timeout_completion) return ESP_ERR_TIMEOUT;
    *transaction = pending; pending = NULL; return ESP_OK;
}

static void reset(void)
{
    memset(&n, 0, sizeof(n));
    calls = bus_claims = bus_frees = device_removes = reads = 0;
    existing_owner = bus_busy = timeout_completion = remove_failure = free_failure = empty_id = task_failure = false;
    scheduled = NULL; pending = NULL;
}
static void run_probe(void)
{
    assert(board_recording_nand_probe_start() == AE_AGAIN && scheduled);
    unsigned before = calls;
    assert(board_recording_nand_probe_start() == AE_AGAIN && calls == before);
    assert(board_recording_nand_status().state == BOARD_NAND_PROBING);
    scheduled(NULL);
}

int main(void)
{
    reset();
    assert(board_recording_nand_capabilities().reason == AE_UNAVAILABLE);
    run_probe();
    board_nand_status_t status = board_recording_nand_status();
    assert(status.state == BOARD_NAND_NEEDS_BACKUP_AND_MIGRATION && status.result == AE_OK);
    assert(status.identification_observed && status.cleanup_complete && status.jedec_id[0] == 0xc8);
    assert(status.configuration_register == 0x10 && status.status_register == 0x20);
    assert(reads == 3 && device_removes == 1 && bus_frees == 1);
    ae_store_capabilities_t store = board_recording_nand_capabilities();
    assert(!store.implemented && !store.available && !store.qualified && !store.geometry_verified);
    assert(!store.owned_and_provisioned && !store.recovery_complete && store.reason == AE_PRIVACY);
    unsigned before = calls;
    assert(board_recording_nand_probe_start() == AE_OK && calls == before);
    reset(); existing_owner = true; run_probe();
    assert(board_recording_nand_status().result == AE_BUSY && !bus_claims && !bus_frees && !reads);
    reset(); bus_busy = true; run_probe();
    assert(board_recording_nand_status().result == AE_BUSY && bus_claims == 1 && !bus_frees && !reads);
    reset(); empty_id = true; run_probe();
    assert(board_recording_nand_status().result == AE_IO_ERROR && !board_recording_nand_status().identification_observed);
    assert(bus_frees == 1 && device_removes == 1 && reads == 1);
    reset(); timeout_completion = true; run_probe();
    status = board_recording_nand_status();
    assert(status.state == BOARD_NAND_CLEANUP_UNCERTAIN && status.result == AE_UNCERTAIN && status.sdk_error == ESP_ERR_TIMEOUT);
    assert(!status.cleanup_complete && !bus_frees && !device_removes && pending == &n.transaction.base);
    reset(); remove_failure = true; run_probe();
    assert(board_recording_nand_status().result == AE_UNCERTAIN && !bus_frees && n.device);
    reset(); free_failure = true; run_probe();
    assert(board_recording_nand_status().result == AE_UNCERTAIN && !n.device && n.bus_owned);
    reset(); task_failure = true;
    assert(board_recording_nand_probe_start() == AE_EXHAUSTED && !scheduled && !bus_claims);
    assert(board_recording_nand_status().cleanup_complete);
    assert(lock_depth == 0);
    puts("NAND inventory: raw read-only command allowlist, owner preservation, timeout quarantine and unavailable store passed.");
    return 0;
}
