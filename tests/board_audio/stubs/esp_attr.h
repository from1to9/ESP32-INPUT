// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Host memory has no ESP32 regions. Target section placement is checked from
 * the actual target object, independently of these behavioral tests. */
#define EXT_RAM_BSS_ATTR
