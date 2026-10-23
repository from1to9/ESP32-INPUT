// SPDX-License-Identifier: Apache-2.0
#include "sensor_wire.h"
#include <string.h>

static void put(uint8_t *out, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

uint32_t sensor_wire_crc32(const uint8_t *bytes, size_t count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

size_t sensor_wire_encode(const uint8_t boot[16], uint64_t request_id, uint64_t after,
    const board_sensor_batch_t *page, const board_sensor_record_t *records,
    uint8_t *out, size_t capacity)
{
    if (!boot || !page || !out || page->count > BOARD_SENSOR_JOURNAL_READ_MAX ||
        (page->count && !records)) return 0;
    const size_t length = SENSOR_WIRE_HEADER_BYTES + (size_t)page->count * SENSOR_WIRE_RECORD_BYTES + 4U;
    if (capacity < length || (int)page->state < 0 || page->state > BOARD_SENSOR_READ_UNAVAILABLE ||
        (int)page->journal_state < 0 || page->journal_state > BOARD_SENSOR_JOURNAL_EXHAUSTED) return 0;
    if (page->latest_sequence ? (!page->oldest_sequence || page->oldest_sequence > page->latest_sequence ||
        after > page->latest_sequence) : (page->oldest_sequence || after)) return 0;
    if ((page->journal_state == BOARD_SENSOR_JOURNAL_UNAVAILABLE) !=
        (page->state == BOARD_SENSOR_READ_UNAVAILABLE)) return 0;
    if (page->state == BOARD_SENSOR_READ_DATA) {
        if (!page->count || after == UINT64_MAX || page->lost_records ||
            page->journal_state == BOARD_SENSOR_JOURNAL_UNAVAILABLE) return 0;
        uint64_t previous = after;
        for (uint32_t i = 0; i < page->count; ++i) {
            if (previous == UINT64_MAX || records[i].sequence != previous + 1U ||
                records[i].sequence < page->oldest_sequence || records[i].sequence > page->latest_sequence) return 0;
            previous = records[i].sequence;
        }
        if (page->next_after != previous) return 0;
    } else if (page->count) return 0;
    if (page->state == BOARD_SENSOR_READ_GAP &&
        (!page->oldest_sequence || after >= page->oldest_sequence - 1U ||
         page->next_after != page->oldest_sequence - 1U ||
         page->lost_records != page->oldest_sequence - after - 1U)) return 0;
    if (page->state == BOARD_SENSOR_READ_EMPTY &&
        (page->next_after != after || after != page->latest_sequence || page->lost_records)) return 0;
    if (page->state == BOARD_SENSOR_READ_UNAVAILABLE &&
        (page->journal_state != BOARD_SENSOR_JOURNAL_UNAVAILABLE || after ||
         page->oldest_sequence || page->latest_sequence || page->next_after || page->lost_records)) return 0;
    uint32_t flags = (page->state == BOARD_SENSOR_READ_GAP ? SENSOR_PAGE_GAP : 0U) |
        (page->next_after < page->latest_sequence ? SENSOR_PAGE_MORE : 0U) |
        (page->journal_state == BOARD_SENSOR_JOURNAL_EXHAUSTED ? SENSOR_PAGE_EXHAUSTED : 0U) |
        (page->state == BOARD_SENSOR_READ_UNAVAILABLE ? SENSOR_PAGE_UNAVAILABLE : 0U);
    memset(out, 0, length);
    out[0] = SENSOR_WIRE_VERSION;
    out[1] = (uint8_t)page->count;
    put(out + 2, SENSOR_WIRE_RECORD_BYTES, 2);
    put(out + 4, flags, 4);
    put(out + 8, request_id, 8);
    memcpy(out + 16, boot, 16);
    put(out + 32, after, 8);
    put(out + 40, page->oldest_sequence, 8);
    put(out + 48, page->latest_sequence, 8);
    for (uint32_t i = 0; i < page->count; ++i) {
        const board_sensor_record_t *r = &records[i];
        uint8_t *p = out + SENSOR_WIRE_HEADER_BYTES + (size_t)i * SENSOR_WIRE_RECORD_BYTES;
        put(p, r->sequence, 8); put(p + 8, r->source_sequence, 8);
        put(p + 16, r->source_epoch, 8); put(p + 24, r->sample_us, 8);
        put(p + 32, r->observed_us, 8); put(p + 40, r->kind, 2);
        put(p + 42, r->flags, 2); put(p + 44, (uint32_t)r->error, 4);
        put(p + 48, r->discontinuities, 4); put(p + 52, r->validity, 4);
        for (unsigned word = 0; word < 8; ++word) put(p + 56U + word * 4U, r->payload[word], 4);
    }
    put(out + length - 4U, sensor_wire_crc32(out, length - 4U), 4);
    return length;
}

bool sensor_wire_base64(const uint8_t *data, size_t length, char *out, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if ((!data && length) || !out || length > (SIZE_MAX - 2U) / 4U * 3U ||
        capacity <= ((length + 2U) / 3U) * 4U) return false;
    size_t written = 0;
    for (size_t i = 0; i < length; i += 3) {
        const size_t left = length - i;
        const uint32_t bits = (uint32_t)data[i] << 16 |
            (left > 1 ? (uint32_t)data[i + 1] << 8 : 0U) | (left > 2 ? data[i + 2] : 0U);
        out[written++] = alphabet[(bits >> 18) & 63U];
        out[written++] = alphabet[(bits >> 12) & 63U];
        out[written++] = left > 1 ? alphabet[(bits >> 6) & 63U] : '=';
        out[written++] = left > 2 ? alphabet[bits & 63U] : '=';
    }
    out[written] = 0;
    return true;
}
