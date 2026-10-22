// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef struct spi_transaction_t {
    unsigned int flags;
    size_t length, rxlength;
    const void *tx_buffer;
    void *rx_buffer, *user;
} spi_transaction_t;
typedef struct test_spi_device *spi_device_handle_t;
typedef struct {
    unsigned int flags, clock_speed_hz;
    int mode, spics_io_num;
    size_t queue_size;
    void (*pre_cb)(spi_transaction_t *), (*post_cb)(spi_transaction_t *);
    uint8_t cs_ena_pretrans, cs_ena_posttrans;
} spi_device_interface_config_t;
#define SPI_DEVICE_HALFDUPLEX (1U << 0)
#define SPI_DEVICE_TXBIT_LSBFIRST (1U << 1)
#define SPI_DEVICE_3WIRE (1U << 2)
#define SPI_DEVICE_POSITIVE_CS (1U << 3)
#define SPI_TRANS_CS_KEEP_ACTIVE (1U << 0)
#define SPI_TRANS_MULTILINE_CMD (1U << 1)
#define SPI_TRANS_MULTILINE_ADDR (1U << 2)
#define SPI_TRANS_MODE_OCT (1U << 3)
#define SPI_TRANS_MODE_QIO (1U << 4)
#define SPI_TRANS_DMA_USE_PSRAM (1U << 5)
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t *);
esp_err_t spi_device_acquire_bus(spi_device_handle_t, unsigned int);
void spi_device_release_bus(spi_device_handle_t);
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t **, unsigned int);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t *, unsigned int);
