// SPDX-License-Identifier: Apache-2.0
#define main board_fixture_main
#include "test_board_audio.c"
#undef main

static board_audio_capture_snapshot_t level_snapshot(void)
{
    board_audio_capture_snapshot_t out = {0}; unsigned before = calls;
    board_audio_capture_snapshot(&out); board_audio_capture_snapshot(NULL);
    assert(calls == before); return out;
}
static ae_result_t read_level(void)
{
    uint8_t data[16]; size_t actual = 0;
    assert(port.read(NULL, s.generation, data, sizeof(data), &actual) == AE_AGAIN); drain();
    return port.read(NULL, s.generation, data, sizeof(data), &actual);
}
static void setup_level(void)
{
    port = board_audio_mosaico_port(); assert(!level_snapshot().known);
    assert(board_audio_mosaico_init() == AE_OK); drain();
    open_audio(&mono, AE_MICROPHONE | AE_SPEAKER, 1); unmute_role(AE_MICROPHONE, 1);
    assert(!level_snapshot().known); custom_capture = true;
}
int main(int argc, char **argv)
{
    assert(argc == 2); setup_level();
    if (!strcmp(argv[1], "amplitude")) {
        capture_samples[0] = 16384; capture_samples[1] = -16384;
        assert(read_level() == AE_OK); board_audio_capture_snapshot_t first = level_snapshot();
        assert(first.known && first.current && first.generation == 1 && first.observed_ms == 1000 &&
            first.samples == 8 && first.sum_squares == UINT64_C(8) * 268435456U && first.peak_abs == 16384);
        capture_samples[0] = capture_samples[1] = 8192;
        board_test_time_us += 99000; assert(read_level() == AE_OK);
        board_audio_capture_snapshot_t next = level_snapshot(); assert(!memcmp(&first, &next, sizeof(first)));
        board_test_time_us += 1000; assert(read_level() == AE_OK); next = level_snapshot();
        assert(next.observed_ms == 1100 && next.samples == 16 && next.sum_squares == UINT64_C(16) * 67108864U && next.peak_abs == 8192);
    } else if (!strcmp(argv[1], "silence")) {
        assert(read_level() == AE_OK); board_audio_capture_snapshot_t out = level_snapshot();
        assert(out.known && out.current && out.samples == 8 && !out.sum_squares && !out.peak_abs);
    } else if (!strcmp(argv[1], "extrema")) {
        capture_samples[0] = INT16_MIN; capture_samples[1] = INT16_MAX;
        assert(read_level() == AE_OK); board_audio_capture_snapshot_t out = level_snapshot();
        assert(out.known && out.current && out.peak_abs == 32768 && out.samples == 8 &&
            out.sum_squares == UINT64_C(4) * (UINT64_C(1073741824) + UINT64_C(1073676289)));
    } else if (!strcmp(argv[1], "failure")) {
        capture_samples[0] = capture_samples[1] = 1000; assert(read_level() == AE_OK);
        transfer_limit = 8; assert(read_level() == AE_TIMEOUT);
        assert(!level_snapshot().known && level_snapshot().result == AE_TIMEOUT);
        transfer_limit = SIZE_MAX; transfer_error = ESP_ERR_TIMEOUT; assert(read_level() == AE_TIMEOUT);
        assert(!level_snapshot().known && !level_snapshot().current);
        transfer_error = ESP_OK; transfer_limit = 3; assert(read_level() == AE_INTEGRITY);
        assert(!level_snapshot().known && level_snapshot().result == AE_INTEGRITY);
        transfer_limit = SIZE_MAX; read_overreport = 4; assert(read_level() == AE_INTEGRITY);
        assert(!level_snapshot().known); read_overreport = 0;
        board_test_time_us += 100000; assert(read_level() == AE_OK); assert(level_snapshot().current);
    } else if (!strcmp(argv[1], "safety")) {
        assert(read_level() == AE_OK); assert(level_snapshot().current);
        inject_mute = true; assert(read_level() == AE_STALE);
        assert(!level_snapshot().known && !level_snapshot().current);
        assert(port.mute(NULL, AE_MICROPHONE, true, 1) == AE_OK); unmute_role(AE_MICROPHONE, 1);
        assert(read_level() == AE_OK); assert(level_snapshot().current);
        inject_stop = true; assert(read_level() == AE_STALE); assert(!level_snapshot().current);
        assert(port.stop(NULL, 1) == AE_OK); open_audio(&mono, AE_MICROPHONE, 2); unmute_role(AE_MICROPHONE, 2);
        assert(!level_snapshot().known); assert(read_level() == AE_OK);
        board_audio_capture_snapshot_t before = level_snapshot(); assert(before.current && before.generation == 2);
        uint8_t data[16]; size_t actual = 0;
        assert(port.read(NULL, 1, data, sizeof(data), &actual) == AE_STALE);
        board_audio_capture_snapshot_t after = level_snapshot(); assert(!memcmp(&before, &after, sizeof(before)));
        assert(port.mute(NULL, AE_MICROPHONE, true, 2) == AE_AGAIN); assert(!level_snapshot().current);
    } else {
        assert(!strcmp(argv[1], "clock")); board_test_time_us = 1000500; assert(read_level() == AE_OK);
        --board_test_time_us; assert(read_level() == AE_OK);
        /* Still the same millisecond, then an actual backward millisecond. */
        board_test_time_us -= 1000; assert(read_level() == AE_OK);
        assert(!level_snapshot().known && level_snapshot().result == AE_TIME_UNKNOWN);
        board_test_time_us = 2000000; assert(read_level() == AE_OK); assert(!level_snapshot().current);
        close_audio(1, AE_OK); open_audio(&mono, AE_MICROPHONE, 2); unmute_role(AE_MICROPHONE, 2);
        assert(read_level() == AE_OK && level_snapshot().current);
        board_test_time_us = -1; assert(read_level() == AE_OK);
        assert(!level_snapshot().known && level_snapshot().result == AE_TIME_UNKNOWN);
    }
    if (s.mute_pending) drain();
    if (s.prepared) close_audio(s.generation, AE_OK);
    printf("PASS: cached microphone energy %s, real board/codec with SDK PCM boundary\n", argv[1]);
    return 0;
}
