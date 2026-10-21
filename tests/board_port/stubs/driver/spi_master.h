// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host_id, size_t *max_bytes);
