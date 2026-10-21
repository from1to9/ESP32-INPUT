// SPDX-License-Identifier: Apache-2.0
#include "audio_media.h"
#include <string.h>

void ae_secure_zero(void *memory, size_t bytes)
{
    volatile unsigned char *cursor = memory;
    while (bytes--) *cursor++ = 0;
}

bool ae_id_equal(const ae_id_t *left, const ae_id_t *right)
{
    return left && right && memcmp(left->bytes, right->bytes, AE_ID_BYTES) == 0;
}

bool ae_id_empty(const ae_id_t *id)
{
    if (!id) return true;
    uint8_t value = 0;
    for (size_t i = 0; i < AE_ID_BYTES; ++i) value |= id->bytes[i];
    return value == 0;
}

bool ae_route_token_equal(const ae_route_token_t *left, const ae_route_token_t *right)
{
    return left && right && left->link_generation == right->link_generation &&
        ae_id_equal(&left->peer_id, &right->peer_id) && ae_id_equal(&left->stream_id, &right->stream_id);
}

bool ae_route_token_valid(const ae_route_token_t *token, bool local)
{
    return token && (local ? token->link_generation == 0 && ae_id_empty(&token->peer_id) &&
        ae_id_empty(&token->stream_id) : token->link_generation != 0 &&
        !ae_id_empty(&token->peer_id) && !ae_id_empty(&token->stream_id));
}

bool ae_format_equal(const ae_format_t *left, const ae_format_t *right)
{
    return left && right && left->sample_rate_hz == right->sample_rate_hz &&
        left->channels == right->channels && left->frame_ms == right->frame_ms &&
        left->encoding == right->encoding;
}

ae_result_t ae_format_size(const ae_format_t *format, size_t *bytes, uint32_t *samples)
{
    if (!format || !bytes || !samples || format->encoding != AE_PCM_S16_LE ||
        (format->channels != 1 && format->channels != 2) ||
        (format->frame_ms != 10 && format->frame_ms != 20)) return AE_INVALID;
    switch (format->sample_rate_hz) {
    case 8000: case 16000: case 44100: case 48000: break;
    default: return AE_NOT_SUPPORTED;
    }
    uint32_t frames = format->sample_rate_hz * format->frame_ms / 1000;
    size_t count = (size_t)frames * format->channels * 2;
    if (!count || count > AE_FRAME_BYTES_MAX) return AE_NOT_SUPPORTED;
    *bytes = count;
    *samples = frames;
    return AE_OK;
}

ae_result_t ae_ring_init(ae_ring_t *ring, ae_frame_t *storage, uint8_t capacity,
    const ae_format_t *format, uint64_t generation, uint64_t consumer_generation,
    uint64_t first_sequence, uint64_t first_sample_offset, const ae_route_token_t *route_token)
{
    size_t bytes;
    uint32_t samples;
    if (!ring || !storage || !capacity || capacity > AE_RING_FRAMES || !generation ||
        !consumer_generation || ae_format_size(format, &bytes, &samples) != AE_OK)
        return AE_INVALID;
    *ring = (ae_ring_t){.storage = storage, .capacity = capacity, .format = *format,
        .audio_generation = generation, .consumer_generation = consumer_generation,
        .next_sequence = first_sequence, .next_sample_offset = first_sample_offset};
    if (route_token) ring->route_token = *route_token;
    ae_secure_zero(storage, sizeof(*storage) * capacity);
    return AE_OK;
}

ae_result_t ae_ring_push(ae_ring_t *ring, const ae_frame_t *frame)
{
    size_t bytes;
    uint32_t samples;
    if (!ring || !ring->storage || !frame ||
        ae_format_size(&frame->format, &bytes, &samples) != AE_OK ||
        frame->bytes != bytes || !ae_format_equal(&frame->format, &ring->format)) return AE_INVALID;
    if (frame->audio_generation != ring->audio_generation ||
        frame->consumer_generation != ring->consumer_generation ||
        frame->sequence != ring->next_sequence || frame->sample_offset != ring->next_sample_offset ||
        !ae_route_token_equal(&frame->route_token, &ring->route_token))
        return AE_STALE;
    if (frame->sequence == UINT64_MAX || frame->sample_offset > UINT64_MAX - samples)
        return AE_EXHAUSTED;
    if (ring->count == ring->capacity) return AE_OVERFLOW;
    ae_frame_t *target = &ring->storage[(ring->head + ring->count) % ring->capacity];
    ae_secure_zero(target, sizeof(*target));
    target->audio_generation = frame->audio_generation;
    target->consumer_generation = frame->consumer_generation;
    target->sequence = frame->sequence;
    target->sample_offset = frame->sample_offset;
    target->route_token = frame->route_token;
    target->format = frame->format;
    target->bytes = frame->bytes;
    memcpy(target->data, frame->data, frame->bytes);
    ++ring->count;
    ++ring->next_sequence;
    ring->next_sample_offset += samples;
    return AE_OK;
}

const ae_frame_t *ae_ring_front(const ae_ring_t *ring)
{
    return ring && ring->storage && ring->count ? &ring->storage[ring->head] : NULL;
}

void ae_ring_drop(ae_ring_t *ring)
{
    if (!ring || !ring->storage || !ring->count) return;
    ae_secure_zero(&ring->storage[ring->head], sizeof(*ring->storage));
    ring->head = (uint8_t)((ring->head + 1) % ring->capacity);
    --ring->count;
}

ae_result_t ae_ring_pop(ae_ring_t *ring, ae_frame_t *output)
{
    if (!ring || !ring->storage || !output) return AE_INVALID;
    const ae_frame_t *front = ae_ring_front(ring);
    if (!front) return AE_AGAIN;
    *output = *front;
    ae_ring_drop(ring);
    return AE_OK;
}

void ae_ring_reset(ae_ring_t *ring)
{
    if (!ring || !ring->storage) return;
    ae_secure_zero(ring->storage, sizeof(*ring->storage) * ring->capacity);
    ring->head = ring->count = 0;
}

static void little16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
}

static void little32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t read16(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] | (uint16_t)in[1] << 8);
}

static uint32_t read32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

ae_result_t ae_wav_header(const ae_format_t *format, uint64_t pcm_bytes,
    uint8_t output[AE_WAV_HEADER_BYTES])
{
    size_t bytes;
    uint32_t samples;
    if (!output || ae_format_size(format, &bytes, &samples) != AE_OK ||
        pcm_bytes > UINT32_MAX - 36U || pcm_bytes % (format->channels * 2U)) return AE_INVALID;
    memset(output, 0, AE_WAV_HEADER_BYTES);
    memcpy(output, "RIFF", 4); little32(output + 4, (uint32_t)pcm_bytes + 36);
    memcpy(output + 8, "WAVEfmt ", 8); little32(output + 16, 16);
    little16(output + 20, 1); little16(output + 22, format->channels);
    little32(output + 24, format->sample_rate_hz);
    little32(output + 28, format->sample_rate_hz * format->channels * 2U);
    little16(output + 32, (uint16_t)(format->channels * 2U)); little16(output + 34, 16);
    memcpy(output + 36, "data", 4); little32(output + 40, (uint32_t)pcm_bytes);
    return AE_OK;
}

ae_result_t ae_wav_inspect(const uint8_t header[AE_WAV_HEADER_BYTES],
    uint64_t actual_file_bytes, ae_wav_info_t *info)
{
    if (!header || !info || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) ||
        read32(header + 16) != 16 || read16(header + 20) != 1 || read16(header + 34) != 16 ||
        memcmp(header + 36, "data", 4)) return AE_INTEGRITY;
    const uint16_t channels = read16(header + 22);
    ae_format_t format = {.sample_rate_hz = read32(header + 24), .channels = (uint8_t)channels,
        .frame_ms = 10, .encoding = AE_PCM_S16_LE};
    size_t bytes;
    uint32_t samples;
    const uint32_t pcm = read32(header + 40);
    if (channels != format.channels || ae_format_size(&format, &bytes, &samples) != AE_OK ||
        read16(header + 32) != channels * 2U ||
        read32(header + 28) != format.sample_rate_hz * channels * 2U ||
        pcm % (channels * 2U) || (uint64_t)pcm + 36 != read32(header + 4) ||
        actual_file_bytes != (uint64_t)pcm + AE_WAV_HEADER_BYTES) return AE_INTEGRITY;
    *info = (ae_wav_info_t){.sample_rate_hz = format.sample_rate_hz, .channels = format.channels,
        .pcm_bytes = pcm, .sample_frames = pcm / (channels * 2U), .wav_bytes = actual_file_bytes};
    return AE_OK;
}
