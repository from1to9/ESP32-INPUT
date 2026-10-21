// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "app_audio.h"
#include "audio_media.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct test_task { pthread_t thread; pthread_mutex_t lock; pthread_cond_t changed; unsigned notifications; TaskFunction_t fn; void *argument; };
static struct test_task task;
static atomic_uint allocations, writes, receipts;
static atomic_bool hold_worker, worker_held;
static size_t allocated_bytes;
static ae_format_t pcm = {48000, 1, 10, AE_PCM_S16_LE};
static ae_route_token_t route_token = {.peer_id = {{1}}, .stream_id = {{2}}, .link_generation = 1};
static void (*notify_done)(void *);
static void *notify_context;
static unsigned pending;
static uint64_t codec_generation;
static bool mic, speaker;
static uint8_t submitted[AE_FRAME_BYTES_MAX];
static int64_t micros(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int64_t esp_timer_get_time(void) { return micros(); }
static void pause_ms(unsigned ms) { struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); void *p = calloc(n, bytes); if (p) { atomic_fetch_add(&allocations, 1); allocated_bytes += n * bytes; } return p; }
void heap_caps_free(void *p) { if (p) { atomic_fetch_sub(&allocations, 1); free(p); } }
void esp_fill_random(void *p, size_t bytes) { static uint64_t id; memset(p, 0, bytes); ++id; memcpy(p, &id, bytes < 8 ? bytes : 8); }
static void *thread_main(void *p) { struct test_task *t = p; t->fn(t->argument); return NULL; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t bytes, void *argument, unsigned priority, TaskHandle_t *out)
{
    (void)name; assert(bytes == 8192 && priority == 4); task.fn = fn; task.argument = argument;
    assert(!pthread_mutex_init(&task.lock, NULL)); assert(!pthread_cond_init(&task.changed, NULL));
    assert(!pthread_create(&task.thread, NULL, thread_main, &task)); if (out) *out = &task; return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &task; }
void xTaskNotifyGive(TaskHandle_t t)
{ pthread_mutex_lock(&t->lock); ++t->notifications; pthread_cond_signal(&t->changed); pthread_mutex_unlock(&t->lock); }
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)clear;
    while (atomic_load(&hold_worker)) { atomic_store(&worker_held, true); pause_ms(1); }
    atomic_store(&worker_held, false);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    /* Model the project's 100 Hz fallback tick. Codec/ingress notifications
     * must sustain 10 ms media without depending on a hypothetical 1 ms tick. */
    until.tv_nsec += (long)ticks * 10000000;
    until.tv_sec += until.tv_nsec / 1000000000; until.tv_nsec %= 1000000000;
    pthread_mutex_lock(&task.lock);
    if (!task.notifications) (void)pthread_cond_timedwait(&task.changed, &task.lock, &until);
    unsigned count = task.notifications; task.notifications = 0; pthread_mutex_unlock(&task.lock); return count;
}
void vTaskDelay(TickType_t ticks) { pause_ms(ticks * 10); }
void vTaskDelete(TaskHandle_t t) { assert(!t); pthread_exit(NULL); }
static void completed(void) { if (notify_done) notify_done(notify_context); }
static void completion(void *c, void (*fn)(void *), void *context) { (void)c; notify_done = fn; notify_context = context; }
static ae_result_t exchange(unsigned operation)
{
    if (!pending) { pending = operation; completed(); return AE_AGAIN; }
    assert(pending == operation); pending = 0; return AE_OK;
}
static ae_result_t capabilities(void *c, ae_codec_capabilities_t *out)
{
    (void)c; *out = (ae_codec_capabilities_t){.implemented = true, .available = true, .full_duplex = true,
        .safe_volume_max = 80, .format_count = 1, .formats = {{.format = pcm, .roles = 3}}}; return AE_OK;
}
static ae_result_t prepare(void *c, const ae_format_t *f, uint8_t roles, uint64_t generation)
{ (void)c; assert(ae_format_equal(f, &pcm) && roles == 3); codec_generation = generation; return exchange(1); }
static ae_result_t mute_codec(void *c, ae_role_t role, bool muted, uint64_t generation)
{
    (void)c; assert(generation == codec_generation); ae_result_t r = exchange(muted ? 2 : 3);
    if (r == AE_OK) { if (role == AE_MICROPHONE) mic = !muted; else speaker = !muted; } return r;
}
static ae_result_t level(void *c, uint8_t value, uint64_t generation)
{ (void)c; (void)value; assert(generation == codec_generation); return exchange(4); }
static ae_result_t stop_codec(void *c, uint64_t generation)
{ (void)c; (void)generation; pending = 0; mic = speaker = false; return AE_OK; }
static ae_result_t write_codec(void *c, uint64_t generation, const void *data, size_t bytes, size_t *actual)
{
    (void)c; assert(speaker && generation == codec_generation && bytes == 960); *actual = 0;
    if (!pending) memcpy(submitted, data, bytes); else assert(!memcmp(data, submitted, bytes));
    ae_result_t r = exchange(5); if (r == AE_OK) { *actual = bytes; atomic_fetch_add(&writes, 1); } return r;
}
static ae_result_t route_caps(void *c, ae_route_t route, ae_route_capabilities_t *out)
{
    (void)c; assert(route == AE_ROUTE_USB_UAC); *out = (ae_route_capabilities_t){.implemented = true, .available = true,
        .connected = true, .capture = true, .playback = true, .full_duplex = true, .format = pcm, .token = route_token}; return AE_OK;
}
static ae_result_t route_prepare(void *c, ae_route_t route, const ae_format_t *f, uint8_t roles, uint64_t generation, const ae_route_token_t *token)
{ (void)c; assert(route == AE_ROUTE_USB_UAC && ae_format_equal(f, &pcm) && roles == 3 && generation && ae_route_token_equal(token, &route_token)); return AE_OK; }
static ae_result_t route_stop(void *c, ae_route_t route, uint64_t generation, const ae_route_token_t *token)
{ (void)c; (void)route; (void)generation; (void)token; return AE_OK; }
static void consumed(void *c, ae_route_t route, const ae_frame_t *frame)
{ (void)c; assert(route == AE_ROUTE_USB_UAC && frame->sequence == atomic_load(&receipts) + 1); atomic_fetch_add(&receipts, 1); }
static ae_result_t authority(void *c, ae_authority_operation_t op, const ae_start_request_t *request, uint64_t now, ae_authority_t *out)
{ (void)c; (void)op; (void)request; *out = (ae_authority_t){.allowed = true, .owner_session_alive = true, .source_valid_until_ms = now + 3600000}; return AE_OK; }
static ae_result_t wall(void *c, uint64_t *seconds) { (void)c; *seconds = 100000; return AE_OK; }
static ae_snapshot_t snapshot(void)
{
    ae_snapshot_t view = {0}; for (unsigned i = 0; i < 500 && !app_audio_snapshot(&view); ++i) pause_ms(1);
    assert(view.audio_generation); return view;
}
static app_audio_result_t command(app_audio_command_t *request)
{
    static uint64_t token; request->request_token = ++token; assert(app_audio_submit(request) == AE_OK);
    app_audio_result_t result;
    for (unsigned i = 0; i < 2000; ++i) { if (app_audio_take_result(&result)) { assert(result.request_token == token); return result; } pause_ms(1); }
    assert(!"async command timed out"); return (app_audio_result_t){0};
}
int main(void)
{
    ae_dependencies_t d = {.codec = {.capabilities = capabilities, .prepare = prepare, .mute = mute_codec,
        .volume = level, .gain = level, .stop = stop_codec, .write = write_codec, .completion_notify = completion},
        .route = {.capabilities = route_caps, .prepare = route_prepare, .stop = route_stop, .playback_consumed = consumed},
        .authority = authority, .trusted_unix_time = wall};
    assert(app_audio_init(&d) == AE_OK); ae_snapshot_t view = snapshot();
    ae_environment_t environment = {.unlocked = true, .screen_visible = true}; app_audio_set_environment(&environment);
    app_audio_command_t request = {.kind = APP_AUDIO_CMD_VOLUME, .expected_generation = view.audio_generation, .expected_revision = view.revision, .level = 35};
    assert(command(&request).result == AE_OK); view = snapshot();
    request = (app_audio_command_t){.kind = APP_AUDIO_CMD_ROUTE, .route = AE_ROUTE_USB_UAC,
        .expected_generation = view.audio_generation, .expected_revision = view.revision};
    assert(command(&request).result == AE_OK); view = snapshot();
    ae_frame_t frame = {.audio_generation = view.audio_generation, .format = pcm, .route_token = route_token, .sequence = 1, .bytes = 960};
    assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &frame) == AE_STALE);
    assert(app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation) == 0);
    request = (app_audio_command_t){.kind = APP_AUDIO_CMD_START, .start = {.request_id = {{1}}, .principal_id = {{2}}, .owner_session_id = {{3}},
        .expected_audio_generation = view.audio_generation, .expected_revision = view.revision, .kind = AE_PLAYBACK_STREAM,
        .route = AE_ROUTE_USB_UAC, .route_token = route_token, .format = pcm, .duration_seconds = 3600, .byte_limit = 1000000}};
    app_audio_result_t begun = command(&request); assert(begun.result == AE_OK); view = snapshot();
    frame.audio_generation = view.audio_generation;
    for (unsigned i = 0; i < 80; ++i) {
        frame.sequence = i + AE_FIRST_SEQUENCE; frame.sample_offset = i * 480U; memset(frame.data, (int)i, frame.bytes);
        assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &frame) == AE_OK); pause_ms(10);
        if (i == 40) {
            view = snapshot(); request = (app_audio_command_t){.kind = APP_AUDIO_CMD_VOLUME, .level = 45,
                .expected_generation = view.audio_generation, .expected_revision = view.revision};
            assert(command(&request).result == AE_OK);
        }
    }
    for (unsigned i = 0; i < 200 && atomic_load(&writes) < 80; ++i) pause_ms(1);
    assert(atomic_load(&writes) == 80 && atomic_load(&receipts) == 80);
    view = snapshot(); assert(view.playback.state == AE_JOB_RUNNING && !view.overruns);
    assert(app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation + 1) == 0);
    assert(app_audio_transport_playback_capacity(AE_ROUTE_HFP_HF, view.audio_generation) == 0);
    atomic_store(&hold_worker, true);
    for (unsigned i = 0; i < 500 && !atomic_load(&worker_held); ++i) pause_ms(1);
    assert(atomic_load(&worker_held));
    for (unsigned i = 0; i < 4; ++i) {
        assert(app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation) == 4 - i);
        frame.sequence++; frame.sample_offset += 480;
        assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &frame) == AE_OK);
    }
    assert(app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation) == 0);
    atomic_store(&hold_worker, false);
    for (unsigned i = 0; i < 500 && atomic_load(&receipts) < 84; ++i) pause_ms(1);
    assert(atomic_load(&receipts) == 84 && !snapshot().overruns);
    frame.sequence++; frame.sample_offset += 480; frame.route_token.link_generation++;
    assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &frame) == AE_STALE);
    app_audio_request_safety_stop(AE_STOP_PRIVACY);
    assert(app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation) == 0);
    assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &frame) != AE_OK);
    assert(app_audio_shutdown() == AE_AGAIN); assert(!pthread_join(task.thread, NULL));
    assert(app_audio_shutdown() == AE_OK && atomic_load(&allocations) == 0);
    pthread_cond_destroy(&task.changed); pthread_mutex_destroy(&task.lock);
    printf("Resource bytes: app audio allocations=%zu (including engine, excluding worker stack/SDK drivers)\n", allocated_bytes);
    puts("PASS: actual audio worker, async codec completion notifications at 100 Hz tick, 80 real-rate frames plus four-frame credit catchup, exact consumer binding, consumption receipts and safety shutdown");
    return 0;
}
