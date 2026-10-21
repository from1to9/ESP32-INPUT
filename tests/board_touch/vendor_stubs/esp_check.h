// SPDX-License-Identifier: Apache-2.0
#pragma once
#define ESP_RETURN_ON_FALSE(a, e, ...) do { if (!(a)) return (e); } while (0)
