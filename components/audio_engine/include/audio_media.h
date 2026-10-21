// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_io.h"

typedef struct {
    ae_frame_t *storage;
    ae_format_t format;
    ae_route_token_t route_token;
    uint64_t audio_generation, consumer_generation;
    uint64_t next_sequence, next_sample_offset;
    uint8_t capacity, head, count;
} ae_ring_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint32_t pcm_bytes;
    uint64_t sample_frames;
    uint64_t wav_bytes;
} ae_wav_info_t;

/* Format/size validation is not hardware capability or qualification. */
ae_result_t ae_format_size(const ae_format_t *, size_t *bytes, uint32_t *sample_frames);
bool ae_format_equal(const ae_format_t *, const ae_format_t *);
bool ae_id_equal(const ae_id_t *, const ae_id_t *);
bool ae_id_empty(const ae_id_t *);
bool ae_route_token_equal(const ae_route_token_t *, const ae_route_token_t *);
bool ae_route_token_valid(const ae_route_token_t *, bool local);
void ae_secure_zero(void *, size_t);

/* Caller owns storage for the entire ring lifetime. No allocation or overwrite.
 * Frames are copied on push/pop; a successful pop clears the stored copy.
 * reset discards/clears every slot without resetting sequence acceptance. */
ae_result_t ae_ring_init(ae_ring_t *, ae_frame_t *storage, uint8_t capacity,
    const ae_format_t *, uint64_t generation, uint64_t consumer_generation,
    uint64_t first_sequence, uint64_t first_sample_offset, const ae_route_token_t *);
ae_result_t ae_ring_push(ae_ring_t *, const ae_frame_t *);
ae_result_t ae_ring_pop(ae_ring_t *, ae_frame_t *);
const ae_frame_t *ae_ring_front(const ae_ring_t *);
void ae_ring_drop(ae_ring_t *);
void ae_ring_reset(ae_ring_t *);

/* Canonical PCM16LE WAV only. No host endian/struct-layout dependency.
 * A valid empty header is useful during reservation, never proof of a completed
 * recording. inspect requires the exact actual plaintext file length. */
ae_result_t ae_wav_header(const ae_format_t *, uint64_t pcm_bytes,
    uint8_t output[AE_WAV_HEADER_BYTES]);
ae_result_t ae_wav_inspect(const uint8_t header[AE_WAV_HEADER_BYTES],
    uint64_t actual_file_bytes, ae_wav_info_t *);
