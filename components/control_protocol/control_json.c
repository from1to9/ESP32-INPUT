// SPDX-License-Identifier: Apache-2.0
#include "control_json.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool cj_utf8(const unsigned char *s, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned c = s[i++], rest = 0, value = 0, minimum = 0;
        if (c < 0x80) {
            if (!c) {
                return false;
            }
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            rest = 1;
            value = c & 31U;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            rest = 2;
            value = c & 15U;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            rest = 3;
            value = c & 7U;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + rest > length) {
            return false;
        }
        while (rest--) {
            c = s[i++];
            if ((c & 0xc0U) != 0x80U) {
                return false;
            }
            value = value * 64U + (c & 63U);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
    }
    return true;
}
static void whitespace(cj_document_t *d)
{
    while (d->position < d->length && strchr(" \t\r\n", d->text[d->position])) {
        ++d->position;
    }
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
static bool unicode(cj_document_t *d, uint32_t *out)
{
    if (d->position + 4 > d->length) {
        return false;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int digit = hex(d->text[d->position++]);
        if (digit < 0) {
            return false;
        }
        value = value * 16U + (unsigned)digit;
    }
    *out = value;
    return true;
}
static bool string(cj_document_t *d, uint16_t *offset, uint16_t *length)
{
    if (d->position >= d->length || d->text[d->position++] != '"') {
        return false;
    }
    size_t start = d->position, write = start;
    while (d->position < d->length) {
        unsigned char c = (unsigned char)d->text[d->position++];
        if (c == '"') {
            d->text[write] = 0;
            *offset = (uint16_t)start;
            *length = (uint16_t)(write - start);
            return true;
        }
        if (c < 32) {
            return false;
        }
        if (c != '\\') {
            d->text[write++] = (char)c;
            continue;
        }
        if (d->position >= d->length) {
            return false;
        }
        c = (unsigned char)d->text[d->position++];
        if (c == '"' || c == '\\' || c == '/') {
            d->text[write++] = (char)c;
        } else if (c == 'b') {
            d->text[write++] = '\b';
        } else if (c == 'f') {
            d->text[write++] = '\f';
        } else if (c == 'n') {
            d->text[write++] = '\n';
        } else if (c == 'r') {
            d->text[write++] = '\r';
        } else if (c == 't') {
            d->text[write++] = '\t';
        } else if (c == 'u') {
            uint32_t value;
            if (!unicode(d, &value) || !value) {
                return false;
            }
            if (value >= 0xd800 && value <= 0xdbff) {
                if (d->position + 2 > d->length || d->text[d->position++] != '\\' ||
                    d->text[d->position++] != 'u') {
                    return false;
                }
                uint32_t low;
                if (!unicode(d, &low) || low < 0xdc00 || low > 0xdfff) {
                    return false;
                }
                value = 0x10000U + ((value - 0xd800U) << 10U) + low - 0xdc00U;
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                return false;
            }
            if (value < 0x80) {
                d->text[write++] = (char)value;
            } else if (value < 0x800) {
                d->text[write++] = (char)(0xc0 | (value >> 6));
                d->text[write++] = (char)(0x80 | (value & 63));
            } else if (value < 0x10000) {
                d->text[write++] = (char)(0xe0 | (value >> 12));
                d->text[write++] = (char)(0x80 | ((value >> 6) & 63));
                d->text[write++] = (char)(0x80 | (value & 63));
            } else {
                d->text[write++] = (char)(0xf0 | (value >> 18));
                d->text[write++] = (char)(0x80 | ((value >> 12) & 63));
                d->text[write++] = (char)(0x80 | ((value >> 6) & 63));
                d->text[write++] = (char)(0x80 | (value & 63));
            }
        } else {
            return false;
        }
    }
    return false;
}
static uint16_t value(cj_document_t *d, unsigned depth)
{
    whitespace(d);
    if (d->position >= d->length || d->count == CJ_TOKENS) {
        return CJ_NONE;
    }
    uint16_t index = d->count++;
    cj_node_t *n = &d->nodes[index];
    *n = (cj_node_t){.key = CJ_NONE, .child = CJ_NONE, .next = CJ_NONE};
    char c = d->text[d->position];
    if (c == '"') {
        n->type = CJ_STRING;
        return string(d, &n->offset, &n->length) ? index : CJ_NONE;
    }
    if (c == '{' || c == '[') {
        if (depth > 4) {
            return CJ_NONE;
        }
        n->type = c == '{' ? CJ_OBJECT : CJ_ARRAY;
        ++d->position;
        whitespace(d);
        char close = c == '{' ? '}' : ']';
        uint16_t last = CJ_NONE;
        unsigned members = 0;
        if (d->position < d->length && d->text[d->position] == close) {
            ++d->position;
            return index;
        }
        for (;;) {
            if (++members > 32) {
                return CJ_NONE;
            }
            uint16_t key = CJ_NONE, key_length = 0;
            if (n->type == CJ_OBJECT) {
                if (!string(d, &key, &key_length)) {
                    return CJ_NONE;
                }
                for (uint16_t k = n->child; k != CJ_NONE; k = d->nodes[k].next) {
                    if (d->nodes[k].key_length == key_length &&
                        memcmp(d->text + d->nodes[k].key, d->text + key, key_length) == 0) {
                        return CJ_NONE;
                    }
                }
                whitespace(d);
                if (d->position == d->length || d->text[d->position++] != ':') {
                    return CJ_NONE;
                }
            }
            uint16_t child = value(d, depth + 1);
            if (child == CJ_NONE) {
                return CJ_NONE;
            }
            d->nodes[child].key = key;
            d->nodes[child].key_length = key_length;
            if (last == CJ_NONE) {
                n->child = child;
            } else {
                d->nodes[last].next = child;
            }
            last = child;
            whitespace(d);
            if (d->position == d->length) {
                return CJ_NONE;
            }
            c = d->text[d->position++];
            if (c == close) {
                return index;
            }
            if (c != ',') {
                return CJ_NONE;
            }
            whitespace(d);
        }
    }
    for (unsigned k = 0; k < 3; ++k) {
        const char *word = k == 0 ? "null" : k == 1 ? "false" : "true";
        size_t length = strlen(word);
        if (d->position + length <= d->length && memcmp(d->text + d->position, word, length) == 0) {
            n->type = k == 0 ? CJ_NULL : CJ_BOOL;
            n->value.boolean = k == 2;
            d->position += length;
            return index;
        }
    }
    size_t start = d->position;
    if (c == '-') {
        ++d->position;
    }
    if (d->position == d->length) {
        return CJ_NONE;
    }
    if (d->text[d->position] == '0') {
        ++d->position;
    } else {
        if (d->text[d->position] < '1' || d->text[d->position] > '9') {
            return CJ_NONE;
        }
        while (d->position < d->length && d->text[d->position] >= '0' &&
               d->text[d->position] <= '9') {
            ++d->position;
        }
    }
    bool integer = true;
    if (d->position < d->length && d->text[d->position] == '.') {
        integer = false;
        size_t before = ++d->position;
        while (d->position < d->length && d->text[d->position] >= '0' &&
               d->text[d->position] <= '9') {
            ++d->position;
        }
        if (d->position == before) {
            return CJ_NONE;
        }
    }
    if (d->position < d->length && (d->text[d->position] == 'e' || d->text[d->position] == 'E')) {
        integer = false;
        ++d->position;
        if (d->position < d->length &&
            (d->text[d->position] == '+' || d->text[d->position] == '-')) {
            ++d->position;
        }
        size_t before = d->position;
        while (d->position < d->length && d->text[d->position] >= '0' &&
               d->text[d->position] <= '9') {
            ++d->position;
        }
        if (d->position == before) {
            return CJ_NONE;
        }
    }
    size_t length = d->position - start;
    if (length == 0 || length >= 64) {
        return CJ_NONE;
    }
    n->offset = (uint16_t)start;
    n->length = (uint16_t)length;
    char number[64];
    memcpy(number, d->text + start, length);
    number[length] = 0;
    char *end;
    errno = 0;
    if (integer) {
        n->type = CJ_INTEGER;
        n->value.integer = strtoll(number, &end, 10);
    } else {
        n->type = CJ_NUMBER;
        n->value.number = strtod(number, &end);
        if (!isfinite(n->value.number)) {
            return CJ_NONE;
        }
    }
    return errno || *end ? CJ_NONE : index;
}
bool cj_parse(cj_document_t *d, const char *text, size_t length)
{
    if (!d || !text || !length || length > CJ_CAPACITY ||
        !cj_utf8((const unsigned char *)text, length)) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    memcpy(d->text, text, length);
    d->length = length;
    if (value(d, 0) != 0) {
        return false;
    }
    whitespace(d);
    return d->position == d->length;
}
uint16_t cj_get(const cj_document_t *d, uint16_t object, const char *key)
{
    if (object >= d->count || d->nodes[object].type != CJ_OBJECT) {
        return CJ_NONE;
    }
    for (uint16_t i = d->nodes[object].child; i != CJ_NONE; i = d->nodes[i].next) {
        if (strcmp(d->text + d->nodes[i].key, key) == 0) {
            return i;
        }
    }
    return CJ_NONE;
}
const char *cj_string(const cj_document_t *d, uint16_t index)
{
    return index < d->count && d->nodes[index].type == CJ_STRING ? d->text + d->nodes[index].offset
                                                                 : NULL;
}
static bool exact_integer(const cj_document_t *, const cj_node_t *, int64_t *);
bool cj_integer(const cj_document_t *d, uint16_t index, int64_t *out)
{
    if (index >= d->count || !out) {
        return false;
    }
    const cj_node_t *n = &d->nodes[index];
    if (n->type == CJ_INTEGER) {
        *out = n->value.integer;
        return true;
    }
    return n->type == CJ_NUMBER && exact_integer(d, n, out);
}
bool cj_u64(const cj_document_t *d, uint16_t index, uint64_t *out)
{
    const char *s = cj_string(d, index);
    if (!s || !*s || (s[0] == '0' && s[1])) {
        return false;
    }
    uint64_t result = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || result > (UINT64_MAX - (unsigned)(*s - '0')) / 10U) {
            return false;
        }
        result = result * 10U + (unsigned)(*s - '0');
    }
    *out = result;
    return true;
}
static bool put(char *out, size_t cap, size_t *used, const char *text, size_t length)
{
    if (*used + length >= cap) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = 0;
    return true;
}
static bool quoted(char *out, size_t cap, size_t *used, const char *text)
{
    if (!put(out, cap, used, "\"", 1)) {
        return false;
    }
    for (const unsigned char *s = (const unsigned char *)text; *s; ++s) {
        char escape[7];
        size_t length = 1;
        const char *piece = (const char *)s;
        if (*s == '"' || *s == '\\') {
            escape[0] = '\\';
            escape[1] = (char)*s;
            piece = escape;
            length = 2;
        } else if (*s < 32) {
            snprintf(escape, sizeof(escape), "\\u%04x", *s);
            piece = escape;
            length = 6;
        }
        if (!put(out, cap, used, piece, length)) {
            return false;
        }
    }
    return put(out, cap, used, "\"", 1);
}
typedef struct {
    char digits[64];
    size_t count;
    int exponent;
    bool negative;
} decimal_t;
static bool decimal_parts(const char *text, size_t length, decimal_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    int fraction = 0;
    bool dot = false;
    if (i < length && text[i] == '-') {
        out->negative = true;
        ++i;
    }
    for (; i < length && text[i] != 'e' && text[i] != 'E'; ++i) {
        if (text[i] == '.') {
            if (dot) {
                return false;
            }
            dot = true;
            continue;
        }
        if (text[i] < '0' || text[i] > '9' || out->count >= sizeof(out->digits) - 1) {
            return false;
        }
        out->digits[out->count++] = text[i];
        if (dot) {
            ++fraction;
        }
    }
    int exponent = 0;
    bool negative = false;
    if (i < length) {
        ++i;
        if (i < length && (text[i] == '-' || text[i] == '+')) {
            negative = text[i] == '-';
            ++i;
        }
        if (i == length) {
            return false;
        }
        for (; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
            if (exponent < 10000) {
                exponent = exponent * 10 + text[i] - '0';
            }
        }
    }
    size_t leading = 0;
    while (leading < out->count && out->digits[leading] == '0') {
        ++leading;
    }
    if (leading == out->count) {
        out->digits[0] = '0';
        out->digits[1] = 0;
        out->count = 1;
        out->negative = false;
        out->exponent = 0;
        return true;
    }
    memmove(out->digits, out->digits + leading, out->count - leading);
    out->count -= leading;
    if (exponent > 10000) {
        return false;
    }
    out->exponent = (negative ? -exponent : exponent) - fraction;
    while (out->count > 1 && out->digits[out->count - 1] == '0') {
        --out->count;
        ++out->exponent;
    }
    out->digits[out->count] = 0;
    return true;
}
static bool exact_integer(const cj_document_t *d, const cj_node_t *n, int64_t *out)
{
    decimal_t v;
    if (!decimal_parts(d->text + n->offset, n->length, &v) || v.exponent < 0 || v.exponent > 19 ||
        v.count + (size_t)v.exponent > 19) {
        return false;
    }
    uint64_t limit = v.negative ? UINT64_C(9223372036854775808) : INT64_MAX, integer = 0;
    for (size_t i = 0; i < v.count; ++i) {
        unsigned digit = (unsigned)(v.digits[i] - '0');
        if (integer > (limit - digit) / 10) {
            return false;
        }
        integer = integer * 10 + digit;
    }
    for (int i = 0; i < v.exponent; ++i) {
        if (integer > limit / 10) {
            return false;
        }
        integer *= 10;
    }
    *out = v.negative ? (integer == UINT64_C(9223372036854775808) ? INT64_MIN : -(int64_t)integer)
                      : (int64_t)integer;
    return true;
}
static bool decimal_multiple(const cj_document_t *d, const cj_node_t *n, const cs_node_t *schema)
{
    char step[64];
    const char *text = schema->multiple_decimal;
    if (!text) {
        snprintf(step, sizeof(step), "%.15g", schema->multiple);
        text = step;
    }
    decimal_t value, multiple;
    if (!decimal_parts(d->text + n->offset, n->length, &value) ||
        !decimal_parts(text, strlen(text), &multiple) || multiple.negative) {
        return false;
    }
    if (value.count == 1 && value.digits[0] == '0') {
        return true;
    }
    if (multiple.count > 9 || (multiple.count == 1 && multiple.digits[0] == '0')) {
        return false;
    }
    int power = value.exponent - multiple.exponent;
    if (power < 0 || power > 10000) {
        return false;
    }
    uint64_t divisor = 0, remainder = 0;
    for (size_t i = 0; i < multiple.count; ++i) {
        divisor = divisor * 10 + (unsigned)(multiple.digits[i] - '0');
    }
    for (size_t i = 0; i < value.count; ++i) {
        remainder = (remainder * 10 + (unsigned)(value.digits[i] - '0')) % divisor;
    }
    for (int i = 0; i < power && remainder; ++i) {
        remainder = (remainder * 10) % divisor;
    }
    return remainder == 0;
}
static bool decimal_emit(const cj_document_t *d, const cj_node_t *n, char *out, size_t cap,
                         size_t *used)
{
    decimal_t v;
    if (!decimal_parts(d->text + n->offset, n->length, &v)) {
        return false;
    }
    if (v.negative && !put(out, cap, used, "-", 1)) {
        return false;
    }
    int point = (int)v.count + v.exponent;
    /* Preserve exact decimal value through canonicalization. Never expand a
     * bounded non-integral exponent into an unbounded run of zeroes. */
    if (v.exponent >= 0 && point <= 18) {
        if (!put(out, cap, used, v.digits, v.count)) {
            return false;
        }
        for (int i = 0; i < v.exponent; ++i) {
            if (!put(out, cap, used, "0", 1)) {
                return false;
            }
        }
        return true;
    }
    if (v.exponent < 0 && point > 0) {
        return put(out, cap, used, v.digits, (size_t)point) && put(out, cap, used, ".", 1) &&
               put(out, cap, used, v.digits + point, v.count - (size_t)point);
    }
    if (v.exponent < 0 && point > -18) {
        if (!put(out, cap, used, "0.", 2)) {
            return false;
        }
        for (int i = 0; i < -point; ++i) {
            if (!put(out, cap, used, "0", 1)) {
                return false;
            }
        }
        return put(out, cap, used, v.digits, v.count);
    }
    char exponent[16];
    int count = snprintf(exponent, sizeof(exponent), "e%d", v.exponent);
    return count > 0 && (size_t)count < sizeof(exponent) &&
           put(out, cap, used, v.digits, v.count) && put(out, cap, used, exponent, (size_t)count);
}
static bool emit(const cj_document_t *d, uint16_t index, char *out, size_t cap, size_t *used)
{
    if (index >= d->count) {
        return false;
    }
    const cj_node_t *n = &d->nodes[index];
    char number[64];
    switch (n->type) {
    case CJ_NULL:
        return put(out, cap, used, "null", 4);
    case CJ_BOOL:
        return put(out, cap, used, n->value.boolean ? "true" : "false", n->value.boolean ? 4 : 5);
    case CJ_INTEGER:
        snprintf(number, sizeof(number), "%lld", (long long)n->value.integer);
        return put(out, cap, used, number, strlen(number));
    case CJ_NUMBER:
        return decimal_emit(d, n, out, cap, used);
    case CJ_STRING:
        return quoted(out, cap, used, d->text + n->offset);
    case CJ_OBJECT:
    case CJ_ARRAY: {
        bool object = n->type == CJ_OBJECT;
        uint16_t children[32];
        size_t count = 0;
        for (uint16_t i = n->child; i != CJ_NONE; i = d->nodes[i].next) {
            children[count++] = i;
        }
        if (object) {
            for (size_t i = 1; i < count; ++i) {
                uint16_t v = children[i];
                size_t j = i;
                while (j && strcmp(d->text + d->nodes[children[j - 1]].key,
                                   d->text + d->nodes[v].key) > 0) {
                    children[j] = children[j - 1];
                    --j;
                }
                children[j] = v;
            }
        }
        if (!put(out, cap, used, object ? "{" : "[", 1)) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (i && !put(out, cap, used, ",", 1)) {
                return false;
            }
            if (object && (!quoted(out, cap, used, d->text + d->nodes[children[i]].key) ||
                           !put(out, cap, used, ":", 1))) {
                return false;
            }
            if (!emit(d, children[i], out, cap, used)) {
                return false;
            }
        }
        return put(out, cap, used, object ? "}" : "]", 1);
    }
    }
    return false;
}
bool cj_canonical(const cj_document_t *d, uint16_t index, char *out, size_t capacity,
                  size_t *length)
{
    size_t used = 0;
    if (!out || !capacity || !emit(d, index, out, capacity, &used)) {
        return false;
    }
    if (length) {
        *length = used;
    }
    return true;
}
bool cj_escape(const char *text, char *out, size_t capacity)
{
    size_t used = 0;
    return out && capacity && quoted(out, capacity, &used, text);
}
static bool format(const char *name, const char *text)
{
    if (!name || !*name) {
        return true;
    }
    size_t length = strlen(text);
    if (strcmp(name, "uint64") == 0) {
        if (!length || (length > 1 && text[0] == '0')) {
            return false;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9' ||
                v > (UINT64_MAX - (unsigned)(text[i] - '0')) / 10U) {
                return false;
            }
            v = v * 10U + (unsigned)(text[i] - '0');
        }
        return true;
    }
    if (strcmp(name, "id128") == 0 || strcmp(name, "sha256") == 0) {
        if (length != (strcmp(name, "id128") == 0 ? 32U : 64U)) {
            return false;
        }
        for (size_t i = 0; i < length; ++i) {
            if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
                return false;
            }
        }
        return true;
    }
    if (strcmp(name, "passphrase") == 0 && length < 8) {
        return false;
    }
    if ((strcmp(name, "ssid") == 0 || strcmp(name, "device_name") == 0) && !length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c == 127) {
            return false;
        }
        if ((strcmp(name, "ansi_text") == 0 || strcmp(name, "passphrase") == 0) && c > 126) {
            return false;
        }
        if (strcmp(name, "device_name") == 0 && c == 0xc2 && i + 1 < length &&
            (unsigned char)text[i + 1] >= 0x80 && (unsigned char)text[i + 1] <= 0x9f) {
            return false;
        }
    }
    return strcmp(name, "device_name") != 0 || (text[0] != ' ' && text[length - 1] != ' ');
}
bool cs_validate(const cj_document_t *d, uint16_t index, const cs_node_t *s)
{
    if (!s || index >= d->count) {
        return false;
    }
    const cj_node_t *n = &d->nodes[index];
    if (s->type == CJ_INTEGER || s->type == CJ_NUMBER) {
        double v;
        if (s->type == CJ_INTEGER) {
            int64_t i;
            if (!cj_integer(d, index, &i)) {
                return false;
            }
            v = (double)i;
        } else if (n->type == CJ_INTEGER) {
            v = (double)n->value.integer;
        } else if (n->type == CJ_NUMBER) {
            v = n->value.number;
        } else {
            return false;
        }
        if (v < s->minimum || v > s->maximum ||
            (s->multiple != 0.0 && !decimal_multiple(d, n, s))) {
            return false;
        }
        if (s->enumeration) {
            char value_text[64];
            snprintf(value_text, sizeof(value_text), "%.17g", v);
            const char *p = s->enumeration;
            bool found = false;
            while (*p) {
                const char *end = strchr(p, '|');
                size_t length = end ? (size_t)(end - p) : strlen(p);
                if (strlen(value_text) == length && memcmp(p, value_text, length) == 0) {
                    found = true;
                }
                p += length;
                if (*p) {
                    ++p;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }
    if (s->type != n->type) {
        return false;
    }
    if (n->type == CJ_BOOL) {
        return !s->true_only || n->value.boolean;
    }
    if (n->type == CJ_STRING) {
        const char *text = d->text + n->offset;
        if (n->length < s->min_length || n->length > s->max_length || !format(s->format, text)) {
            return false;
        }
        if (s->enumeration) {
            const char *p = s->enumeration;
            while (*p) {
                const char *end = strchr(p, '|');
                size_t length = end ? (size_t)(end - p) : strlen(p);
                if (n->length == length && memcmp(text, p, length) == 0) {
                    return true;
                }
                p += length;
                if (*p) {
                    ++p;
                }
            }
            return false;
        }
        return true;
    }
    if (n->type == CJ_ARRAY) {
        size_t count = 0;
        for (uint16_t i = n->child; i != CJ_NONE; i = d->nodes[i].next) {
            if (++count > s->max_items || !cs_validate(d, i, s->items)) {
                return false;
            }
        }
        return true;
    }
    if (n->type == CJ_OBJECT) {
        for (uint16_t i = n->child; i != CJ_NONE; i = d->nodes[i].next) {
            const cs_property_t *p = NULL;
            for (size_t j = 0; j < s->property_count; ++j) {
                if (strcmp(d->text + d->nodes[i].key, s->properties[j].name) == 0) {
                    p = &s->properties[j];
                }
            }
            if (!p || !cs_validate(d, i, p->schema)) {
                return false;
            }
        }
        for (size_t j = 0; j < s->property_count; ++j) {
            if (s->properties[j].required && cj_get(d, index, s->properties[j].name) == CJ_NONE) {
                return false;
            }
        }
    }
    return true;
}
