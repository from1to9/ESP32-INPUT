// SPDX-License-Identifier: Apache-2.0
#define main board_fixture_main
#include "test_board_audio.c"
#undef main

static board_audio_pcm_snapshot_t observed(void)
{
    board_audio_pcm_snapshot_t out = {0}; unsigned before = calls;
    board_audio_pcm_snapshot(&out); board_audio_pcm_snapshot(NULL);
    assert(calls == before && !lock_depth); return out;
}
static void setup_pcm(const ae_format_t *format, uint8_t roles)
{
    port = board_audio_mosaico_port();
    board_audio_pcm_snapshot_t out = observed();
    assert(!out.generation_known && !out.write_result_known && !out.pcm_known &&
        !out.volume_requested_known && !out.volume_read_known && !out.mute_read_known);
    assert(board_audio_mosaico_init() == AE_OK); drain();
    open_audio(format, roles, 1);
    out = observed(); assert(out.generation_known && out.generation == 1 && !out.write_result_known &&
        !out.pcm_known && out.mute_read_known && out.mute_requested && out.mute_matches && (out.mute_register & 0x60U) == 0x60U);
}
static ae_result_t write_pcm_case(const uint8_t *data, size_t bytes, size_t *actual)
{
    uint64_t before = observed().write_calls;
    assert(port.write(NULL, 1, data, bytes, actual) == AE_AGAIN);
    assert(observed().write_calls == before); drain();
    ae_result_t r = port.write(NULL, 1, data, bytes, actual);
    assert(observed().write_calls == before + 1 || before == UINT64_MAX);
    return r;
}
int main(int argc, char **argv)
{
    assert(argc == 2); const char *name = argv[1];
    if (!strcmp(name, "existing")) return board_fixture_main();
    const ae_format_t pcm = {48000,1,10,AE_PCM_S16_LE};
    const ae_format_t stereo = {48000,2,10,AE_PCM_S16_LE};
    bool capture_only = !strcmp(name, "capture");
    setup_pcm(!strcmp(name, "stereo") ? &stereo : &pcm, capture_only ? AE_MICROPHONE : AE_SPEAKER);
    static const uint8_t signed_pcm[] = {0,0,0,0x80,0xff,0x7f,0xff,0xff,1,0,0,0xf0,0,0x10};
    size_t actual = 0;
    if (capture_only) {
        unmute_role(AE_MICROPHONE,1); uint8_t data[16];
        assert(port.read(NULL,1,data,sizeof(data),&actual)==AE_AGAIN);drain();
        assert(port.read(NULL,1,data,sizeof(data),&actual)==AE_OK && actual==sizeof(data));
        board_audio_pcm_snapshot_t out=observed();assert(!out.write_result_known && !out.pcm_known &&
            out.write_calls==0 && out.reported_samples==0 && out.reported_nonzero_samples==0);
    } else if (!strcmp(name,"levels")) {
        assert(port.volume(NULL,80,1)==AE_AGAIN);drain();assert(port.volume(NULL,80,1)==AE_OK);
        board_audio_pcm_snapshot_t out=observed(); assert(out.volume_requested_known && out.volume_requested==80 &&
            out.volume_read_known && out.volume_matches && out.volume_register==0xb2);
        corrupt_volume=true;assert(port.volume(NULL,30,1)==AE_AGAIN);drain();assert(port.volume(NULL,30,1)==AE_INTEGRITY);
        out=observed();assert(out.volume_read_known && !out.volume_matches && out.volume_requested==30 && out.volume_register==0x81);
        corrupt_volume=false;fail_register=0x32;fail_read=true;
        assert(port.volume(NULL,50,1)==AE_AGAIN);drain();assert(port.volume(NULL,50,1)==AE_IO_ERROR);
        out=observed();assert(out.volume_requested_known && out.volume_requested==50 && !out.volume_read_known && !out.volume_matches);
        fail_read=false;fail_register=-1;unmute_role(AE_SPEAKER,1);
        out=observed();assert(out.mute_read_known && !out.mute_requested && out.mute_matches && (out.mute_register&0x60U)==0);
        fail_register=0x31;fail_read=true;assert(port.mute(NULL,AE_SPEAKER,true,1)==AE_AGAIN);drain();
        assert(port.mute(NULL,AE_SPEAKER,true,1)==AE_UNCERTAIN);out=observed();assert(!out.mute_read_known && !out.mute_matches);
        fail_read=false;fail_register=-1;
    } else {
        unmute_role(AE_SPEAKER,1);
        if (!strcmp(name,"zero")) {
            const uint8_t zeros[16]={0};assert(write_pcm_case(zeros,sizeof(zeros),&actual)==AE_OK);
            board_audio_pcm_snapshot_t out=observed();assert(out.pcm_known && out.reported_samples==8 && out.reported_nonzero_samples==0 && out.reported_peak_abs==0);
        } else if (!strcmp(name,"cadence")) {
            const uint8_t data[16] = {0};
            assert(write_pcm_case(data, sizeof(data), &actual) == AE_OK);
            assert(!observed().write_intervals);
            board_test_time_us += 30000; /* 6 x 240 frames at 48 kHz. */
            assert(write_pcm_case(data, sizeof(data), &actual) == AE_OK);
            assert(observed().write_intervals == 1 && !observed().write_idle_over_dma);
            board_test_time_us += 30001;
            assert(write_pcm_case(data, sizeof(data), &actual) == AE_OK);
            assert(observed().write_idle_over_dma == 1 && observed().max_write_idle_us == 30001);
            assert(port.mute(NULL, AE_SPEAKER, true, 1) == AE_AGAIN); drain();
            assert(port.mute(NULL, AE_SPEAKER, true, 1) == AE_OK);
            board_test_time_us += 1000000;
            unmute_role(AE_SPEAKER, 1);
            assert(write_pcm_case(data, sizeof(data), &actual) == AE_OK);
            assert(observed().write_intervals == 2 && observed().write_idle_over_dma == 1);
        } else if (!strcmp(name,"stereo")) {
            const uint8_t data[]={0xff,0x7f,0,0x80,0,0x80,0,0x80};
            assert(write_pcm_case(data,sizeof(data),&actual)==AE_OK);board_audio_pcm_snapshot_t out=observed();
            assert(out.reported_samples==2 && out.reported_nonzero_samples==1 && out.reported_peak_abs==32768);
            assert(emitted[0]==0 && emitted[1]==0 && emitted[4]==0 && emitted[5]==0x80);
        } else if (!strcmp(name,"prefix")) {
            const uint8_t data[]={0,0,0xff,0x7f,0,0x80,0xff,0xff};transfer_limit=8;
            assert(write_pcm_case(data,sizeof(data),&actual)==AE_TIMEOUT && actual==4);
            board_audio_pcm_snapshot_t out=observed();assert(out.last_prefix_valid && out.pcm_known && out.reported_samples==2 &&
                out.reported_nonzero_samples==1 && out.reported_peak_abs==32767 && out.failed_writes==0 && out.last_write_sdk_result==ESP_OK);
            transfer_limit=12;transfer_error=ESP_ERR_TIMEOUT;
            assert(write_pcm_case(data,sizeof(data),&actual)==AE_TIMEOUT && actual==6);out=observed();
            assert(out.write_calls==2 && out.failed_writes==1 && out.reported_samples==5 && out.reported_nonzero_samples==3 &&
                out.reported_peak_abs==32768 && out.last_write_sdk_result==ESP_ERR_TIMEOUT);
            transfer_error=ESP_OK;transfer_limit=0;
            assert(write_pcm_case(data,sizeof(data),&actual)==AE_TIMEOUT && actual==0);out=observed();assert(out.reported_samples==5 && out.write_calls==3);
        } else if (!strcmp(name,"invalid")) {
            transfer_limit=3;assert(write_pcm_case(signed_pcm,sizeof(signed_pcm),&actual)==AE_INTEGRITY && actual==0);
            board_audio_pcm_snapshot_t out=observed();assert(out.write_result_known && !out.last_prefix_valid && !out.pcm_known && out.invalid_prefix_writes==1);
            transfer_limit=SIZE_MAX;write_overreport=4;
            assert(write_pcm_case(signed_pcm,sizeof(signed_pcm),&actual)==AE_INTEGRITY && actual==0);out=observed();
            assert(out.invalid_prefix_writes==2 && !out.pcm_known && out.reported_samples==0);write_overreport=0;
        } else {
            if (!strcmp(name,"saturation")) {
                pcm_observation.write_calls=UINT64_MAX;
                pcm_observation.reported_samples=pcm_observation.reported_nonzero_samples=UINT64_MAX-1;
            }
            assert(write_pcm_case(signed_pcm,sizeof(signed_pcm),&actual)==AE_OK && actual==sizeof(signed_pcm));
            board_audio_pcm_snapshot_t out=observed();assert(out.pcm_known && out.last_prefix_valid && out.reported_peak_abs==32768);
            if (!strcmp(name,"saturation")) assert(out.saturated && out.write_calls==UINT64_MAX && out.reported_samples==UINT64_MAX && out.reported_nonzero_samples==UINT64_MAX);
            else assert(out.write_calls==1 && out.reported_samples==7 && out.reported_nonzero_samples==6 && !out.saturated);
        }
    }
    board_audio_pcm_snapshot_t before=observed();close_audio(1,AE_OK);
    board_audio_pcm_snapshot_t stopped=observed();assert(stopped.generation==before.generation && stopped.write_calls==before.write_calls && stopped.reported_samples==before.reported_samples);
    if (!strcmp(name,"generation")) {
        open_audio(&pcm,AE_SPEAKER,2);board_audio_pcm_snapshot_t fresh=observed();
        assert(fresh.generation_known && fresh.generation==2 && !fresh.write_result_known && !fresh.pcm_known && !fresh.saturated &&
            fresh.write_calls==0 && fresh.reported_samples==0 && !fresh.volume_requested_known);
        unsigned io_before=io_calls;
        assert(port.write(NULL,1,signed_pcm,sizeof(signed_pcm),&actual)==AE_STALE);
        assert(io_calls==io_before);board_audio_pcm_snapshot_t after=observed();assert(!memcmp(&fresh,&after,sizeof(fresh)));
        close_audio(2,AE_OK);
    }
    printf("PASS: board PCM snapshot %s (cached, prefix-only, capture excluded; acceptance retained)\n",name);
    return 0;
}
