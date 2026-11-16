#pragma once
#include "esp_http_server.h"

// Register all six AP-only provisioning routes; availability changes only on success.
esp_err_t passport_setup_register(httpd_handle_t server);
