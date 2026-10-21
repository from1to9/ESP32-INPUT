// SPDX-License-Identifier: Apache-2.0
#include "audio_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ae_format_t mono = {16000, 1, 20, AE_PCM_S16_LE};

static bool zero(const void *memory, size_t length)
{
    const unsigned char *p = memory;
    for (size_t i = 0; i < length; ++i) if (p[i]) return false;
    return true;
}

static ae_frame_t frame(uint64_t sequence)
{
    ae_frame_t value = {.audio_generation = 7, .consumer_generation = 19,
        .sequence = sequence, .sample_offset = sequence * 320, .format = mono, .bytes = 640};
    for (unsigned i = 0; i < 320; ++i) {
        uint16_t sample = (uint16_t)(int16_t)((int)i - 160);
        value.data[i * 2] = (uint8_t)sample;
        value.data[i * 2 + 1] = (uint8_t)(sample >> 8);
    }
    return value;
}

static void format_checks(void)
{
    const uint32_t rates[] = {8000, 16000, 44100, 48000};
    for (size_t r = 0; r < 4; ++r) {
        for (unsigned channels = 1; channels <= 2; ++channels) {
            for (unsigned ms = 10; ms <= 20; ms += 10) {
                ae_format_t format = {rates[r], (uint8_t)channels, (uint8_t)ms, AE_PCM_S16_LE};
                size_t bytes = 13;
                uint32_t samples = 17;
                ae_result_t result = ae_format_size(&format, &bytes, &samples);
                uint32_t expected = rates[r] * ms / 1000 * channels * 2;
                if (expected <= AE_FRAME_BYTES_MAX) {
                    assert(result == AE_OK && bytes == expected);
                    assert(samples == rates[r] * ms / 1000);
                } else assert(result == AE_NOT_SUPPORTED && bytes == 13 && samples == 17);
            }
        }
    }
    for (unsigned field = 0; field < 4; ++field) {
        ae_format_t invalid = mono;
        if (field == 0) invalid.sample_rate_hz = 16001;
        if (field == 1) invalid.channels = 0;
        if (field == 2) invalid.frame_ms = 11;
        if (field == 3) invalid.encoding = (ae_encoding_t)99;
        size_t bytes; uint32_t samples;
        assert(ae_format_size(&invalid, &bytes, &samples) != AE_OK);
        assert(!ae_format_equal(&invalid, &mono));
    }
    ae_id_t a = {{1}}, b = {{1}}, empty = {{0}};
    assert(ae_id_equal(&a, &b) && !ae_id_empty(&a));
    b.bytes[15] = 2;
    assert(!ae_id_equal(&a, &b) && ae_id_empty(&empty) && ae_id_empty(NULL));
    puts("PASS: discrete PCM formats, exact frame budgets and independent identities");
}

static void queue_checks(void)
{
    ae_ring_t ring = {0};
    ae_frame_t slots[2], output;
    memset(slots, 0x5a, sizeof(slots));
    assert(!zero(slots, sizeof(slots)));
    assert(ae_ring_init(&ring, slots, 2, &mono, 7, 19, 0, 0, NULL) == AE_OK);
    assert(zero(slots, sizeof(slots)));
    ae_frame_t input = frame(0);
    assert(ae_ring_push(&ring, &input) == AE_OK);
    input.data[0] ^= 0xff;
    assert(ae_ring_front(&ring)->data[0] != input.data[0]);
    input = frame(1);
    assert(ae_ring_push(&ring, &input) == AE_OK);
    input = frame(2);
    assert(ae_ring_push(&ring, &input) == AE_OVERFLOW && ring.count == 2);
    assert(ae_ring_front(&ring)->sequence == 0);
    assert(ae_ring_pop(&ring, &output) == AE_OK && output.sequence == 0);
    assert(zero(&slots[0], sizeof(slots[0])));
    assert(ae_ring_push(&ring, &input) == AE_OK);
    assert(ae_ring_pop(&ring, &output) == AE_OK && output.sequence == 1);
    assert(ae_ring_pop(&ring, &output) == AE_OK && output.sequence == 2);
    assert(ae_ring_pop(&ring, &output) == AE_AGAIN && zero(slots, sizeof(slots)));
    input = frame(3);
    for (unsigned bad = 0; bad < 6; ++bad) {
        ae_frame_t rejected = input;
        if (bad == 0) --rejected.audio_generation;
        if (bad == 1) --rejected.consumer_generation;
        if (bad == 2) --rejected.sequence;
        if (bad == 3) ++rejected.sample_offset;
        if (bad == 4) --rejected.bytes;
        if (bad == 5) rejected.format.channels = 2;
        assert(ae_ring_push(&ring, &rejected) != AE_OK);
        assert(ring.count == 0 && ring.next_sequence == 3 && zero(slots, sizeof(slots)));
    }
    assert(ae_ring_push(&ring, &input) == AE_OK);
    ae_ring_reset(&ring);
    assert(ring.count == 0 && ring.next_sequence == 4 && zero(slots, sizeof(slots)));
    assert(ae_ring_push(&ring, &input) == AE_STALE); /* Stop cannot permit replay. */
    assert(ae_ring_init(&ring, slots, 2, &mono, 7, 20, UINT64_MAX, 0, NULL) == AE_OK);
    input = frame(0); input.consumer_generation = 20; input.sequence = UINT64_MAX;
    assert(ae_ring_push(&ring, &input) == AE_EXHAUSTED && zero(slots, sizeof(slots)));
    assert(ae_ring_init(&ring, slots, 2, &mono, 7, 20, 0, UINT64_MAX - 319, NULL) == AE_OK);
    input.sequence = 0; input.sample_offset = UINT64_MAX - 319;
    assert(ae_ring_push(&ring, &input) == AE_EXHAUSTED && zero(slots, sizeof(slots)));
    ae_route_token_t token = {.peer_id = {{1}}, .stream_id = {{2}}, .link_generation = 17};
    assert(ae_route_token_valid(&token, false));
    assert(ae_ring_init(&ring, slots, 2, &mono, 7, 19, 0, 0, &token) == AE_OK);
    input = frame(0); input.route_token = token;
    for (unsigned bad = 0; bad < 3; ++bad) {
        ae_frame_t stale = input;
        if (bad == 0) ++stale.route_token.peer_id.bytes[0];
        if (bad == 1) ++stale.route_token.stream_id.bytes[0];
        if (bad == 2) --stale.route_token.link_generation;
        assert(ae_ring_push(&ring, &stale) == AE_STALE && ring.count == 0);
    }
    assert(ae_ring_push(&ring, &input) == AE_OK);
    ae_ring_reset(&ring);
    puts("PASS: copied ownership, exact ordering, stale rejection, no overwrite, wrap prevention and complete queue erasure");
}

static void wav_checks(const char *output_path)
{
    uint8_t header[AE_WAV_HEADER_BYTES];
    assert(ae_wav_header(&mono, 640, header) == AE_OK);
    static const uint8_t expected[44] = {
        'R','I','F','F',0xa4,2,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',0x80,2,0,0
    };
    assert(memcmp(header, expected, 44) == 0);
    ae_wav_info_t info;
    assert(ae_wav_inspect(header, 684, &info) == AE_OK);
    assert(info.sample_rate_hz == 16000 && info.channels == 1 && info.sample_frames == 320);
    assert(info.pcm_bytes == 640 && info.wav_bytes == 684);
    for (unsigned byte = 0; byte < sizeof(header); ++byte) {
        uint8_t corrupt[44]; memcpy(corrupt, header, 44); corrupt[byte] ^= 0x80;
        assert(ae_wav_inspect(corrupt, 684, &info) == AE_INTEGRITY);
    }
    assert(ae_wav_inspect(header, 683, &info) == AE_INTEGRITY);
    assert(ae_wav_inspect(header, 685, &info) == AE_INTEGRITY);
    assert(ae_wav_header(&mono, 641, header) == AE_INVALID);
    assert(ae_wav_header(&mono, UINT32_MAX, header) == AE_INVALID);
    assert(ae_wav_header(&mono, 640, header) == AE_OK);
    FILE *output = fopen(output_path, "wb"); assert(output);
    ae_frame_t data = frame(0);
    assert(fwrite(header, 1, sizeof(header), output) == sizeof(header));
    assert(fwrite(data.data, 1, data.bytes, output) == data.bytes);
    assert(fclose(output) == 0);
    puts("PASS: canonical bounded WAV, corruption/truncation/trailing-byte rejection and exact sample count");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    format_checks(); queue_checks(); wav_checks(argv[1]);
    printf("Resource bytes: frame=%zu capture-rings=%zu playback-ring=%zu\n", sizeof(ae_frame_t),
        sizeof(ae_frame_t) * AE_CONSUMERS_MAX * AE_CAPTURE_RING_FRAMES,
        sizeof(ae_frame_t) * AE_RING_FRAMES);
    return 0;
}
