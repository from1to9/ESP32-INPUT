// SPDX-License-Identifier: Apache-2.0
#include "passport_usb_protocol.h"
#include "passport_receipt.h"
#include <limits.h>
#include <string.h>

static const char digits[] = "0123456789abcdef";
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hex_number(const char *text, unsigned length, uint32_t *value)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < length; ++i) {
        int n = nibble(text[i]);
        if (n < 0) return false;
        result = result * 16 + (unsigned)n;
    }
    *value = result; return true;
}
uint16_t passport_usb_crc(const char *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)(unsigned char)bytes[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
void passport_usb_parser_reset(passport_usb_parser_t *parser)
{
    volatile unsigned char *p = (volatile unsigned char *)parser;
    for (size_t i = 0; i < sizeof(*parser); ++i) p[i] = 0;
}
bool passport_usb_parser_expire(passport_usb_parser_t *parser, uint64_t now)
{
    if (!parser->active || (now >= parser->last_byte && now - parser->last_byte < PUSB_PARTIAL_TIMEOUT_MS)) return false;
    passport_usb_parser_reset(parser); return true;
}
passport_usb_parse_t passport_usb_feed(passport_usb_parser_t *parser, unsigned char c,
                                      uint64_t now, passport_usb_request_t *out)
{
    (void)passport_usb_parser_expire(parser, now);
    if (c == 0x1e) {
        passport_usb_parser_reset(parser); parser->active = true; parser->last_byte = now;
        return PUSB_MORE;
    }
    if (!parser->active) return PUSB_MORE;
    parser->last_byte = now;
    if (parser->carriage_return && c != '\n') goto bad;
    if (c == '\r') { parser->carriage_return = true; return PUSB_MORE; }
    if (c != '\n') {
        if (c < 32 || c > 126 || parser->length >= PUSB_FRAME_CAPACITY - 3) goto bad;
        parser->bytes[parser->length++] = (char)c;
        return PUSB_MORE;
    }
    unsigned length = parser->length;
    if (length < 5 || parser->bytes[length - 5] != '*') goto bad;
    uint32_t checksum;
    if (!hex_number(parser->bytes + length - 4, 4, &checksum) ||
        checksum != passport_usb_crc(parser->bytes, length - 5)) goto bad;
    parser->bytes[length - 5] = 0;
    char *tokens[PUSB_ARGUMENTS + 4];
    unsigned count = 0;
    char *start = parser->bytes;
    for (char *p = start;; ++p) {
        if (*p && *p != ' ') continue;
        if (p == start || count == PUSB_ARGUMENTS + 4) goto bad;
        bool end = *p == 0; *p = 0; tokens[count++] = start; start = p + 1;
        if (end) break;
    }
    if (count < 5 || strcmp(tokens[0], "P1") || strcmp(tokens[3], "req") ||
        strlen(tokens[1]) != 8 || strlen(tokens[2]) != 8 ||
        !hex_number(tokens[1], 8, &out->boot) || !hex_number(tokens[2], 8, &out->id)) goto bad;
    out->count = count - 4;
    for (unsigned i = 0; i < out->count; ++i) out->args[i] = tokens[i + 4];
    parser->active = false;
    return PUSB_FRAME;
bad:
    passport_usb_parser_reset(parser);
    return PUSB_BAD_FRAME;
}
size_t passport_usb_seal(char *frame, size_t capacity, size_t used)
{
    if (used < 4 || used + 7 > capacity || frame[0] != 0x1e) return 0;
    uint16_t crc = passport_usb_crc(frame + 1, used - 1);
    frame[used++] = '*';
    for (int shift = 12; shift >= 0; shift -= 4) frame[used++] = digits[(crc >> shift) & 15];
    frame[used++] = '\n'; frame[used] = 0;
    return used;
}
bool passport_usb_u32(const char *text, uint32_t *value)
{
    if (!*text) return false;
    uint32_t result = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (result > (UINT32_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *value = result; return true;
}
bool passport_usb_i32(const char *text, int32_t *value)
{
    bool negative = *text == '-';
    uint32_t magnitude;
    if (!passport_usb_u32(text + negative, &magnitude) ||
        magnitude > (negative ? (uint32_t)INT32_MAX + 1u : INT32_MAX)) return false;
    *value = negative ? (int32_t)(-(int64_t)magnitude) : (int32_t)magnitude;
    return true;
}
bool passport_usb_unhex(const char *text, void *bytes, size_t capacity, size_t *length)
{
    size_t n = !strcmp(text, "-") ? 0 : strlen(text);
    if ((n & 1) || n / 2 > capacity) return false;
    unsigned char *out = bytes;
    for (size_t i = 0; i < n; i += 2) {
        int a = nibble(text[i]), b = nibble(text[i + 1]);
        if (a < 0 || b < 0) return false;
        out[i / 2] = (unsigned char)(a * 16 + b);
    }
    *length = n / 2; return true;
}
void passport_usb_hex(const void *bytes, size_t length, char *text)
{
    const unsigned char *in = bytes;
    if (!length) { text[0] = '-'; text[1] = 0; return; }
    for (size_t i = 0; i < length; ++i) {
        text[2 * i] = digits[in[i] >> 4]; text[2 * i + 1] = digits[in[i] & 15];
    }
    text[2 * length] = 0;
}
const char *passport_result_name(passport_result_t result)
{
    static const char *const names[] = {"none", "pending", "applied", "invalid", "stale", "locked", "busy",
        "unavailable", "storage_uncertain", "cancelled", "unknown", "timeout", "release_required"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown";
}
