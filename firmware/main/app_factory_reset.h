// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>

/* Only the locally confirmed restart worker arms reset. This writes a durable
 * marker, not user-data erasure. An error may still have persisted the marker. */
esp_err_t app_factory_reset_arm(void);
/* Boot only, before settings, radio, trust or recording workers exist. Resume
 * an explicitly armed reset; leave ordinary boots (including old layouts
 * without the optional control partition) untouched. On any error the
 * caller must stop boot, never start workers with a partially cleared store. */
esp_err_t app_factory_reset_boot(bool *performed);
