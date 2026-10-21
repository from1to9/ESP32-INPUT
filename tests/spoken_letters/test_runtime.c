// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_spoken_letters.c"

static jmp_buf end_worker;
static unsigned critical, script, step, commands, afe_creates, afe_destroys, detections;
static bool missing, no_memory, data_ready, cancel_in_detect;
static uint64_t milliseconds;
static void (*task)(void *);
static srmodel_list_t fake_models;
static model_iface_data_t fake_mn;
static int16_t fake_pcm[160];
static sl_binding_t binding = {.job_id = {{7}}, .token = 1, .epoch = 3, .ui_revision = 5,
    .audio_generation = 11, .consumer_generation = 13, .duration_ms = 6000};
static ae_frame_t pcm_frame = {.audio_generation = 11, .consumer_generation = 13,
    .format = {.sample_rate_hz = 16000, .channels = 1, .frame_ms = 10, .encoding = AE_PCM_S16_LE}, .bytes = 320};

void speech_test_enter(void) { assert(!critical); critical = 1; }
void speech_test_exit(void) { assert(critical == 1); critical = 0; }
int64_t esp_timer_get_time(void) { return (int64_t)(milliseconds * 1000); }
void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return no_memory ? NULL : calloc(n, bytes); }
void heap_caps_free(void *p) { free(p); }
int xTaskCreate(void (*function)(void *), const char *name, uint32_t stack, void *arg, unsigned priority, void *handle)
{ assert(!critical && name && stack >= 6144 && !arg && priority && !handle); task = function; return pdPASS; }
srmodel_list_t *esp_srmodel_init(const char *partition)
{ assert(!critical && !strcmp(partition,"model")); return missing ? NULL : &fake_models; }
void esp_srmodel_deinit(srmodel_list_t *m) { assert(m == &fake_models); }
char *esp_srmodel_filter(srmodel_list_t *m, const char *prefix, const char *name)
{ assert(m == &fake_models && !strcmp(prefix,"mn") && !strcmp(name,"mn7_en")); return "mn7_en"; }
static model_iface_data_t *mn_create(const char *name, int duration)
{ assert(!strcmp(name,"mn7_en") && duration == 8000); return &fake_mn; }
static int mn_rate(model_iface_data_t *m) { assert(m == &fake_mn); return 16000; }
static int mn_chunk(model_iface_data_t *m) { assert(m == &fake_mn); return 160; }
static void mn_clean(model_iface_data_t *m) { assert(m == &fake_mn); }
static void mn_destroy(model_iface_data_t *m) { assert(m == &fake_mn); }
static esp_mn_state_t mn_detect(model_iface_data_t *m, int16_t *data)
{
    assert(!critical && m == &fake_mn && data); ++detections;
    if (cancel_in_detect) {
        app_spoken_letters_capture_closed(&binding.job_id, binding.consumer_generation);
        app_spoken_letters_cancel(&binding.job_id, binding.consumer_generation);
    }
    if (cancel_in_detect) { sl_view_t view; assert(app_spoken_letters_snapshot(&view) && view.cleanup_pending); }
    return ESP_MN_STATE_DETECTED;
}
static esp_mn_results_t *mn_results(model_iface_data_t *m)
{ static esp_mn_results_t r = {.num=1,.command_id={1},.prob={.9f}}; assert(m == &fake_mn); return &r; }
esp_mn_iface_t *esp_mn_handle_from_name(const char *name)
{
    static esp_mn_iface_t api = {.create=mn_create,.get_samp_rate=mn_rate,.get_samp_chunksize=mn_chunk,
        .clean=mn_clean,.destroy=mn_destroy,.detect=mn_detect,.get_results=mn_results};
    assert(!strcmp(name,"mn7_en")); return &api;
}
esp_err_t esp_mn_commands_alloc(const esp_mn_iface_t *api, model_iface_data_t *m)
{ assert(api && m == &fake_mn); return ESP_OK; }
esp_err_t esp_mn_commands_free(void) { return ESP_OK; }
void esp_mn_commands_clear(void) { commands = 0; }
esp_err_t esp_mn_commands_add(int id, const char *word)
{ assert(id == (int)++commands && !strcmp(word,sl_command((unsigned)id))); return ESP_OK; }
void *esp_mn_commands_update(void) { assert(commands == 36); return NULL; }
afe_config_t *afe_config_init(const char *format, srmodel_list_t *models, int type, int mode)
{ assert(!strcmp(format,"M") && models && !type && !mode); return calloc(1,sizeof(afe_config_t)); }
void afe_config_free(afe_config_t *config) { free(config); }
static esp_afe_sr_data_t *afe_create(afe_config_t *config)
{
    assert(!config->wakenet_init && !config->aec_init && !config->se_init);
    assert(config->ns_init && config->vad_init && !config->ns_model_name && !config->vad_model_name);
    esp_afe_sr_data_t *m = malloc(sizeof *m); assert(m); m->generation = ++afe_creates; return m;
}
static int afe_chunk(esp_afe_sr_data_t *m) { assert(m); return 160; }
static int afe_channels(esp_afe_sr_data_t *m) { assert(m); return 1; }
static int afe_rate(esp_afe_sr_data_t *m) { assert(m); return 16000; }
static int afe_feed(esp_afe_sr_data_t *m, const int16_t *data)
{ assert(m && data); data_ready = true; return 1; }
static afe_fetch_result_t *afe_fetch(esp_afe_sr_data_t *m, TickType_t wait)
{
    static afe_fetch_result_t frame;
    assert(m && !wait);
    if (!data_ready) return NULL;
    data_ready = false;
    frame = (afe_fetch_result_t){.ret_value=ESP_OK,.data_size=sizeof fake_pcm,.data=fake_pcm}; return &frame;
}
static void afe_destroy(esp_afe_sr_data_t *m) { assert(m); free(m); ++afe_destroys; data_ready = false; }
const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *config)
{
    static const esp_afe_sr_iface_t api = {.create_from_config=afe_create,.get_feed_chunksize=afe_chunk,
        .get_fetch_chunksize=afe_chunk,.get_feed_channel_num=afe_channels,.get_samp_rate=afe_rate,
        .feed=afe_feed,.fetch_with_delay=afe_fetch,.destroy=afe_destroy};
    assert(config); return &api;
}

static void assert_queue_zero(void)
{
    const unsigned char *bytes = (const unsigned char *)s_queue;
    for (size_t i=0; i<SL_QUEUE_FRAMES*sizeof *s_queue; ++i) assert(!bytes[i]);
}

void vTaskDelay(TickType_t ticks)
{
    assert(!critical && ticks == 5);
    sl_view_t view; assert(app_spoken_letters_snapshot(&view));
    if (script == 3) {
        assert(view.state == SL_UNAVAILABLE && view.result == AE_UNAVAILABLE);
        assert(app_spoken_letters_begin(&binding) == AE_UNAVAILABLE); longjmp(end_worker,1);
    }
    if (step == 0) {
        assert(view.state == SL_IDLE && commands == 36);
        assert(app_spoken_letters_begin(&binding) == AE_OK);
        if (script == 1) {
            for (unsigned i=0; i<8; ++i) assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_OK);
            assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_OVERFLOW);
        } else {
            assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_OK);
            if (script == 2) milliseconds += 200;
        }
    } else if (script == 1) {
        assert(view.state == SL_FAULT && view.result == AE_OVERFLOW && view.token == binding.token);
        assert_queue_zero(); longjmp(end_worker,1);
    } else if (script == 2) {
        assert(view.state == SL_TIMED_OUT && view.result == AE_TIMEOUT && !detections);
        assert_queue_zero(); longjmp(end_worker,1);
    } else if (step == 1) {
        assert(view.state == SL_CANDIDATE && view.candidate == 'A' && !view.acoustically_qualified);
        if (script == 5) milliseconds += SL_CANDIDATE_TTL_MS;
        app_spoken_letters_capture_closed(&binding.job_id,binding.consumer_generation);
        if (script == 5) {
            assert(app_spoken_letters_snapshot(&view) && view.state == SL_TIMED_OUT && !view.candidate);
            char candidate = '!';
            assert(app_spoken_letters_confirm(1,3,5,&candidate) == AE_STALE && candidate == '!');
            ++step; return;
        }
        assert(app_spoken_letters_snapshot(&view) && view.state == SL_CANDIDATE &&
            view.candidate == 'A' && view.capture_closed && !view.cleanup_pending);
        assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_STALE);
        if (script == 4) {
            app_spoken_letters_cancel(&binding.job_id,binding.consumer_generation);
            char candidate = '!';
            assert(app_spoken_letters_confirm(1,3,5,&candidate) == AE_STALE && candidate == '!');
            ++step; return;
        }
        app_spoken_letters_cancel(&binding.job_id,binding.consumer_generation+1);
        char candidate = '!';
        assert(app_spoken_letters_confirm(1,3,6,&candidate) == AE_STALE && candidate == '!');
        assert(app_spoken_letters_confirm(1,3,5,&candidate) == AE_OK && candidate == 'A');
        assert(app_spoken_letters_confirm(1,3,5,&candidate) == AE_STALE);
        assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_STALE);
        assert_queue_zero();
    } else if (step == 2) {
        if (script == 5) {
            assert(view.state == SL_TIMED_OUT && view.result == AE_TIMEOUT && !view.cleanup_pending);
            assert_queue_zero(); longjmp(end_worker,1);
        }
        if (script == 4) {
            assert(view.state == SL_CANCELLED && !view.candidate && view.capture_closed && !view.cleanup_pending);
            assert_queue_zero(); longjmp(end_worker,1);
        }
        assert(view.state == SL_IDLE && !view.candidate);
        ++binding.token; ++pcm_frame.sequence; pcm_frame.sample_offset += 160;
        binding.job_id.bytes[0] = 9;
        ++binding.consumer_generation; ++pcm_frame.consumer_generation;
        assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_AGAIN);
        assert(app_spoken_letters_begin(&binding) == AE_OK);
        const ae_id_t old_job = {{7}};
        app_spoken_letters_capture_closed(&old_job,13);
        assert(app_spoken_letters_snapshot(&view) && view.state == SL_LISTENING && !view.capture_closed);
        assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_OK);
        cancel_in_detect = true;
    } else {
        assert(view.state == SL_CANCELLED && !view.candidate && view.token == 2 && !view.cleanup_pending);
        assert(app_spoken_letters_begin(&binding) == AE_STALE);
        assert(app_spoken_letters_capture(&binding.job_id,&pcm_frame) == AE_STALE);
        assert_queue_zero(); assert(afe_creates == afe_destroys); longjmp(end_worker,1);
    }
    ++step;
}

static void clear_runtime(void)
{
    destroy_model(&s_model);
    free(s_queue); s_queue=NULL;
    s_started=false; s_accepting=false; s_start_pending=false; s_cancel_pending=false; s_confirm_pending=false;
    s_bound=false; s_processing=false; s_capture_closed=false; s_last_token=0; s_head=0; s_count=0;
    s_view=(sl_view_t){.state=SL_UNAVAILABLE,.result=AE_UNAVAILABLE};
    s_binding=(sl_binding_t){0}; s_cancel_reason=AE_DENIED; s_candidate_started_ms=0; s_candidate_deadline_ms=0;
    step=0; commands=0; afe_creates=0; afe_destroys=0; detections=0;
    missing=false; no_memory=false; data_ready=false; cancel_in_detect=false; milliseconds=100;
    binding.token=1; pcm_frame.sequence=0; pcm_frame.sample_offset=0;
    binding.job_id.bytes[0]=7; binding.consumer_generation=13; pcm_frame.consumer_generation=13;
}

int main(void)
{
    for (script=0; script<6; ++script) {
        clear_runtime(); missing=script==3;
        assert(app_spoken_letters_init() == AE_OK && task);
        if (!setjmp(end_worker)) task(NULL);
    }
    clear_runtime(); no_memory=true;
    assert(app_spoken_letters_init() == AE_QUOTA);
    sl_view_t view; assert(app_spoken_letters_snapshot(&view));
    assert(view.state == SL_UNAVAILABLE && view.result == AE_QUOTA);
    clear_runtime();
    puts("PASS: production speech worker loading, bounded queue, cancellation, confirmation, PCM clearing and unavailable failures");
    return 0;
}
