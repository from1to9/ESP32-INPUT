// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

/* In-place response to one UDP DNS question for the provisioned AP name.
 * A zero return drops malformed or unsupported messages. */
size_t app_control_dns_reply(const char *name, uint32_t address, uint8_t *packet,
                             size_t length, size_t capacity);
