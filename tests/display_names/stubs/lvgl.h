// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool is_placeholder; } lv_font_glyph_dsc_t;
typedef struct { unsigned unused; } lv_font_t;
extern const lv_font_t lv_font_montserrat_16;
bool lv_font_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t value, uint32_t next);
