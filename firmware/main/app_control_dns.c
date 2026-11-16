// SPDX-License-Identifier: Apache-2.0
#include "app_control_dns.h"
#include <stdbool.h>
#include <string.h>

static uint8_t lowercase(uint8_t byte)
{
    return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + ('a' - 'A')) : byte;
}

size_t app_control_dns_reply(const char *name, uint32_t address, uint8_t *packet,
                             size_t length, size_t capacity)
{
    enum { HEADER = 12, QUESTION = 4, MAX_NAME = 255 };
    if (!name || !packet || length > capacity || length < HEADER + 1 + QUESTION ||
        (packet[2] & 0xfe) || packet[4] || packet[5] != 1 ||
        packet[6] || packet[7] || packet[8] || packet[9]) return 0;

    size_t cursor = HEADER;
    bool matches = true;
    while (cursor < length && packet[cursor]) {
        unsigned label = packet[cursor++];
        if (label > 63 || label > length - cursor || cursor + label - HEADER >= MAX_NAME)
            return 0;
        if (matches) {
            size_t expected = strcspn(name, ".");
            matches = expected == label;
            for (unsigned i = 0; matches && i < label; ++i)
                matches = lowercase(packet[cursor + i]) == (uint8_t)name[i];
            if (matches) name += expected + (name[expected] == '.');
        }
        cursor += label;
    }
    if (length - cursor < 1 + QUESTION) return 0;
    matches = matches && !*name;
    const uint8_t *question = packet + ++cursor;
    const bool own_name = matches && question[2] == 0 && question[3] == 1;
    const bool a_record = own_name && !question[0] && question[1] == 1;
    cursor += QUESTION;

    /* Authoritative A, or an empty answer for AAAA/HTTPS at this IPv4-only
     * name. Refuse other names and classes. Discard any query OPT record. */
    packet[2] = (uint8_t)((packet[2] & 1) | 0x80 | (own_name ? 0x04 : 0));
    packet[3] = own_name ? 0 : 5;
    memset(packet + 6, 0, 6);
    if (!a_record) return cursor;
    const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
    if (capacity - cursor < sizeof(answer) + sizeof(address)) return 0;
    packet[7] = 1;
    memcpy(packet + cursor, answer, sizeof(answer));
    memcpy(packet + cursor + sizeof(answer), &address, sizeof(address)); /* Network byte order. */
    return cursor + sizeof(answer) + sizeof(address);
}
