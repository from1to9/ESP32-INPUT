// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_http_server.h"
#include <stdint.h>
esp_err_t passport_control_http_register(httpd_handle_t);
// Called only by the NimBLE owner, once per tick. Never waits for HTTP.
uint64_t passport_control_http_service(uint64_t now_ms);
// Complete the same mailbox after the HID step and protocol completion in this tick.
void passport_control_http_finish_move(uint64_t command_key, uint64_t now_ms);
