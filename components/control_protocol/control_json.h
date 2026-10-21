// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"
#define CJ_CAPACITY 8192U
#define CJ_TOKENS 384U
#define CJ_NONE UINT16_MAX

typedef enum { CJ_NULL, CJ_BOOL, CJ_INTEGER, CJ_NUMBER, CJ_STRING, CJ_OBJECT, CJ_ARRAY } cj_type_t;
typedef struct {
    cj_type_t type;
    uint16_t key, key_length, offset, length, child, next;
    union {
        int64_t integer;
        double number;
        bool boolean;
    } value;
} cj_node_t;
typedef struct {
    char text[CJ_CAPACITY + 1U];
    cj_node_t nodes[CJ_TOKENS];
    uint16_t count;
    size_t position, length;
} cj_document_t;
bool cj_parse(cj_document_t *, const char *, size_t);
uint16_t cj_get(const cj_document_t *, uint16_t object, const char *key);
bool cj_integer(const cj_document_t *, uint16_t node, int64_t *);
bool cj_u64(const cj_document_t *, uint16_t node, uint64_t *);
const char *cj_string(const cj_document_t *, uint16_t node);
bool cj_canonical(const cj_document_t *, uint16_t node, char *, size_t, size_t *);
bool cj_escape(const char *, char *, size_t);
bool cj_utf8(const unsigned char *, size_t);
/* Generic generated schema; all declared object members are exact/unique. */
typedef struct cs_node cs_node_t;
typedef struct {
    const char *name;
    const cs_node_t *schema;
    bool required;
} cs_property_t;
struct cs_node {
    cj_type_t type;
    double minimum, maximum, multiple;
    uint16_t min_length, max_length, max_items;
    const char *format, *enumeration, *multiple_decimal;
    const cs_property_t *properties;
    size_t property_count;
    const cs_node_t *items;
    bool true_only;
};
bool cs_validate(const cj_document_t *, uint16_t, const cs_node_t *);
