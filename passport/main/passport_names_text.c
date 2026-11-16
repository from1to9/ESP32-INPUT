#include "passport_names.h"

#include "lvgl.h"

bool passport_names_text_valid(const char *label, size_t length)
{
    if (!label || !length || length > PASSPORT_NAME_BYTES) return false;
    const uint8_t *bytes = (const uint8_t *)label;
    for (unsigned count = 0; length; ++count) {
        if (count == PASSPORT_NAME_SCALARS) return false;
        uint32_t scalar = *bytes++;
        --length;
        if (scalar >= 0x80) {
            unsigned tails;
            uint32_t min;
            if (scalar >= 0xC2 && scalar <= 0xDF) { scalar &= 0x1F; tails = 1; min = 0x80; }
            else if (scalar >= 0xE0 && scalar <= 0xEF) { scalar &= 0x0F; tails = 2; min = 0x800; }
            else if (scalar >= 0xF0 && scalar <= 0xF4) { scalar &= 0x07; tails = 3; min = 0x10000; }
            else return false;
            if (length < tails) return false;
            length -= tails;
            while (tails--) {
                uint8_t tail = *bytes++;
                if ((tail & 0xC0) != 0x80) return false;
                scalar = (scalar << 6) | (tail & 0x3F);
            }
            if (scalar < min || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) return false;
        }
        if (scalar < 0x20 || (scalar >= 0x7F && scalar <= 0x9F) || scalar == 0x061C ||
            scalar == 0x200E || scalar == 0x200F || (scalar >= 0x202A && scalar <= 0x202E) ||
            (scalar >= 0x2066 && scalar <= 0x2069)) return false;
        lv_font_glyph_dsc_t small, normal;
        if (!lv_font_get_glyph_dsc(&lv_font_montserrat_12, &small, scalar, 0) || small.is_placeholder ||
            !lv_font_get_glyph_dsc(&lv_font_montserrat_14, &normal, scalar, 0) || normal.is_placeholder) return false;
    }
    return true;
}
