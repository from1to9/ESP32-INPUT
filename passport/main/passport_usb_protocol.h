// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PUSB_FRAME_CAPACITY 256
#define PUSB_ARGUMENTS 12
#define PUSB_PARTIAL_TIMEOUT_MS 1000

typedef struct {
    char bytes[PUSB_FRAME_CAPACITY];
    uint64_t last_byte;
    unsigned length;
    bool active, carriage_return;
} passport_usb_parser_t;
typedef struct {
    uint32_t boot, id;
    unsigned count;
    char *args[PUSB_ARGUMENTS]; // Operation first; borrowed until parser reset.
} passport_usb_request_t;
typedef enum { PUSB_MORE, PUSB_FRAME, PUSB_BAD_FRAME } passport_usb_parse_t;

void passport_usb_parser_reset(passport_usb_parser_t *parser);
bool passport_usb_parser_expire(passport_usb_parser_t *parser, uint64_t now);
passport_usb_parse_t passport_usb_feed(passport_usb_parser_t *, unsigned char,
                                      uint64_t now, passport_usb_request_t *);
uint16_t passport_usb_crc(const char *bytes, size_t length);
// Append *CRC and newline to a buffer beginning with RS, P1 and its payload.
// The VFS may translate newline to CRLF; the host accepts both.
size_t passport_usb_seal(char *frame, size_t capacity, size_t used);
bool passport_usb_u32(const char *text, uint32_t *value);
bool passport_usb_i32(const char *text, int32_t *value);
bool passport_usb_unhex(const char *text, void *bytes, size_t capacity, size_t *length);
void passport_usb_hex(const void *bytes, size_t length, char *text);
