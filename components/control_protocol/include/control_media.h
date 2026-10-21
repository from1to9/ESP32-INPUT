// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CP_MEDIA_HEADER_BYTES 64U
#define CP_MEDIA_PCM_BYTES 1920U
#define CP_MEDIA_FRAME_BYTES (CP_MEDIA_HEADER_BYTES + CP_MEDIA_PCM_BYTES)
typedef enum { CP_MEDIA_CAPTURE = 1, CP_MEDIA_PLAYBACK = 2 } cp_media_kind_t;
typedef struct {
    uint8_t job_id[16];
    uint64_t audio_generation, consumer_generation, sequence, sample_offset;
    uint32_t sample_rate_hz;
    uint16_t bytes;
    uint8_t channels, frame_ms;
    cp_media_kind_t kind;
} cp_media_header_t;
static inline uint64_t cp_media_read_be(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}
static inline void cp_media_write_be(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = n; i > 0; --i) {
        p[i - 1] = (uint8_t)v;
        v >>= 8;
    }
}
static inline bool cp_media_header_valid(const cp_media_header_t *h)
{
    uint8_t id = 0;
    for (unsigned i = 0; i < 16; ++i) {
        id |= h->job_id[i];
    }
    uint64_t samples = (uint64_t)h->sample_rate_hz * h->frame_ms;
    return id && (h->kind == CP_MEDIA_CAPTURE || h->kind == CP_MEDIA_PLAYBACK) &&
           h->audio_generation && h->consumer_generation && h->sequence &&
           (h->channels == 1 || h->channels == 2) && (h->frame_ms == 10 || h->frame_ms == 20) &&
           h->sample_rate_hz >= 8000 && h->sample_rate_hz <= 48000 && samples % 1000 == 0 &&
           h->bytes > 0 && h->bytes <= CP_MEDIA_PCM_BYTES &&
           samples / 1000 * h->channels * 2 == h->bytes;
}
/* ACM1 uses a 64-byte big-endian header; PCM samples are signed 16-bit little
 * endian, interleaved by channel. No compressed/fragmented media in v1. */
static inline bool cp_media_decode(const uint8_t *packet, size_t n, cp_media_header_t *h)
{
    if (!packet || !h || n < CP_MEDIA_HEADER_BYTES || n > CP_MEDIA_FRAME_BYTES ||
        memcmp(packet, "ACM1", 4) || packet[4] != 1 || cp_media_read_be(packet + 6, 2) != 64) {
        return false;
    }
    memset(h, 0, sizeof(*h));
    h->kind = (cp_media_kind_t)packet[5];
    memcpy(h->job_id, packet + 8, 16);
    h->audio_generation = cp_media_read_be(packet + 24, 8);
    h->consumer_generation = cp_media_read_be(packet + 32, 8);
    h->sequence = cp_media_read_be(packet + 40, 8);
    h->sample_offset = cp_media_read_be(packet + 48, 8);
    h->sample_rate_hz = (uint32_t)cp_media_read_be(packet + 56, 4);
    h->channels = packet[60];
    h->frame_ms = packet[61];
    h->bytes = (uint16_t)cp_media_read_be(packet + 62, 2);
    return n == CP_MEDIA_HEADER_BYTES + h->bytes && cp_media_header_valid(h);
}
static inline bool cp_media_encode(const cp_media_header_t *h, const uint8_t *pcm, uint8_t *out,
                                   size_t capacity, size_t *n)
{
    if (!h || !pcm || !out || !n || !cp_media_header_valid(h) ||
        capacity < CP_MEDIA_HEADER_BYTES + h->bytes) {
        return false;
    }
    memset(out, 0, CP_MEDIA_HEADER_BYTES);
    memcpy(out, "ACM1", 4);
    out[4] = 1;
    out[5] = (uint8_t)h->kind;
    cp_media_write_be(out + 6, 2, 64);
    memcpy(out + 8, h->job_id, 16);
    cp_media_write_be(out + 24, 8, h->audio_generation);
    cp_media_write_be(out + 32, 8, h->consumer_generation);
    cp_media_write_be(out + 40, 8, h->sequence);
    cp_media_write_be(out + 48, 8, h->sample_offset);
    cp_media_write_be(out + 56, 4, h->sample_rate_hz);
    out[60] = h->channels;
    out[61] = h->frame_ms;
    cp_media_write_be(out + 62, 2, h->bytes);
    memcpy(out + 64, pcm, h->bytes);
    *n = 64 + h->bytes;
    return true;
}
