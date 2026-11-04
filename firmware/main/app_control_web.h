// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_http_server.h"

/* Public static assets only, on the existing station TLS listener. Domain
 * authentication and every mutation stay in the control protocol owner. */
esp_err_t app_control_web_serve(httpd_req_t *, const char *allowed_host,
                                const char *browser_origin);
