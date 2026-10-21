// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef void *spi_device_handle_t;
typedef struct { uint32_t flags; size_t rxlength; uint64_t addr; uint16_t cmd; uint8_t rx_data[4]; } spi_transaction_t;
typedef struct { spi_transaction_t base; uint8_t command_bits, address_bits, dummy_bits; } spi_transaction_ext_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, spics_io_num, queue_size; uint32_t flags; } spi_device_interface_config_t;
#define SPI_TRANS_VARIABLE_ADDR 1U
#define SPI_TRANS_VARIABLE_CMD 2U
#define SPI_TRANS_VARIABLE_DUMMY 4U
#define SPI_TRANS_USE_RXDATA 8U
#define SPI_DEVICE_HALFDUPLEX 16U
#define SPI_DMA_CH_AUTO 3
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t spi_bus_free(int);
esp_err_t spi_bus_add_device(int, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t *, TickType_t);
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t **, TickType_t);
