// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../firmware/main/board_audio_mosaico.c"
#include "codec_ref_mgr.h"
#include "esp_heap_caps.h"

#ifndef TEST_EXPECT_PSRAM_DMA
#define TEST_EXPECT_PSRAM_DMA 0
#endif
#ifndef TEST_EXPECT_EXTERNAL_STACK
#define TEST_EXPECT_EXTERNAL_STACK 0
#endif

struct test_i2s_channel { bool allocated, initialized, enabled; size_t preload; };
static struct test_i2s_channel tx_channel, rx_channel;
static unsigned calls, notifications, task_creations, io_calls;
static unsigned channel_allocations;
static unsigned dma_init_calls, fail_dma_channel;
static unsigned fail_enable_channel, fail_disable_channel;
static unsigned tx_enables, rx_enables;
static bool stop_on_enable;
static unsigned lock_depth, rail_calls, pa_level;
static int fail_register = -1;
static bool fail_read, fail_write, fail_disable, fail_delete, fail_new, fail_task;
static bool fail_init, fail_remove, readback_corrupt, inject_stop, inject_mute;
static bool fail_start, corrupt_volume;
static esp_err_t transfer_error;
static size_t transfer_limit = SIZE_MAX;
static size_t write_overreport;
static size_t read_overreport;
static bool custom_capture;
static int16_t capture_samples[2];
static unsigned peer_busy_ms, initialization_calls, runtime_control_calls, control_payloads;
static unsigned control_waited_ms;
static bool peer_on_write;
static esp_err_t control_transfer_error;
static void (*control_hook)(void);
static unsigned control_hook_at;
static int references[CODEC_REF_STAGE_NUM];
static uint8_t registers[256];
static uint8_t emitted[WIRE_BYTES_MAX];
static uint8_t dma_storage[DMA_BYTES];
static size_t emitted_bytes;
static ae_codec_port_t port;
static unsigned completion_wakes;
static void completion_wake(void *context)
{
    assert(!lock_depth && context == &completion_wakes);
    assert(board_audio_mosaico_status().worker_started);
    ++completion_wakes;
}

void audio_test_enter(portMUX_TYPE *lock) { (void)lock; assert(lock_depth++ == 0); }
void audio_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth-- == 1); }
static void sdk(void) { assert(lock_depth == 0); ++calls; }
static void assert_cleared(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) assert(p[i] == 0);
}

static BaseType_t create_task(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    sdk();
    assert(function == worker && !strcmp(name, "board_audio") && stack == 6144 && !argument && priority == 5);
    ++task_creations;
    *handle = fail_task ? NULL : (void *)&task_creations;
    return fail_task ? 0 : pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(!TEST_EXPECT_EXTERNAL_STACK);
    return create_task(function, name, stack, argument, priority, handle);
}
BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    assert(TEST_EXPECT_EXTERNAL_STACK && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return create_task(function, name, stack, argument, priority, handle);
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task); ++notifications; }
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; abort(); }
void vTaskDelay(TickType_t ticks) { sdk(); assert(ticks == 10); }
esp_err_t gpio_config(const gpio_config_t *config)
{
    sdk(); assert(config->pin_bit_mask == (UINT64_C(1) << 45) && config->mode == GPIO_MODE_OUTPUT);
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, int level) { sdk(); assert(pin == 45); pa_level = (unsigned)level; return ESP_OK; }
esp_err_t bsp_i2c_init(void) { sdk(); return ESP_OK; }
i2c_master_bus_handle_t bsp_i2c_get_handle(void) { sdk(); return (void *)&registers; }
esp_err_t bsp_power_set_vcc_3v3(bool on) { sdk(); assert(on); ++rail_calls; return ESP_OK; }
esp_err_t bsp_power_set_codec_3v3(bool on) { sdk(); (void)on; ++rail_calls; return ESP_OK; }

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device)
{
    sdk(); assert(bus == (void *)&registers && config->device_address == 0x19);
    assert(config->scl_speed_hz == 100000); *device = (void *)&registers; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    sdk(); assert(device == (void *)&registers); return fail_remove ? ESP_FAIL : ESP_OK;
}
static esp_err_t control_boundary(int timeout, bool writing)
{
    assert(timeout == 5 || timeout == 100);
    if (timeout == 100) ++initialization_calls; else ++runtime_control_calls;
    if (control_hook && initialization_calls == control_hook_at) {
        void (*hook)(void) = control_hook; control_hook = NULL; hook();
    }
    if (peer_busy_ms && (!peer_on_write || writing)) {
        unsigned waited = peer_busy_ms < (unsigned)timeout ? peer_busy_ms : (unsigned)timeout;
        peer_busy_ms -= waited; control_waited_ms += waited;
        if (peer_busy_ms) return ESP_ERR_TIMEOUT; /* Bus admission precedes payload. */
    }
    ++control_payloads;
    return control_transfer_error;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *address,
    size_t address_bytes, uint8_t *data, size_t bytes, int timeout)
{
    sdk(); assert(device && address_bytes == 1 && bytes == 1);
    esp_err_t admitted = control_boundary(timeout, false);
    if (admitted != ESP_OK) return admitted;
    if ((int)*address == fail_register && fail_read) return ESP_ERR_TIMEOUT;
    *data = registers[*address];
    if (readback_corrupt && *address == 0x31) *data ^= 0x60;
    if (corrupt_volume && *address == 0x32) *data ^= 1;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t bytes, int timeout)
{
    sdk(); assert(device && bytes == 2);
    esp_err_t admitted = control_boundary(timeout, true);
    if (admitted != ESP_OK) return admitted;
    if ((int)data[0] == fail_register && fail_write) return ESP_ERR_TIMEOUT;
    if (fail_start && references[CODEC_REF_STAGE_ENABLE] == 1 && data[0] == 0 && data[1] == 0x80)
        return ESP_ERR_TIMEOUT;
    registers[data[0]] = data[1];
    return ESP_OK;
}
int codec_ref_acquire(const audio_codec_ctrl_info_t *info, codec_ref_stage_t stage)
{
    sdk(); assert(info->type == AUDIO_CODEC_CTRL_I2C && info->i2c.addr == 0x32 && info->i2c.port == 0);
    assert(stage < CODEC_REF_STAGE_NUM && references[stage] == 0);
    return ++references[stage];
}
int codec_ref_release(const audio_codec_ctrl_info_t *info, codec_ref_stage_t stage)
{
    sdk(); assert(info->type == AUDIO_CODEC_CTRL_I2C && info->i2c.addr == 0x32);
    assert(stage < CODEC_REF_STAGE_NUM && references[stage] == 1);
    return --references[stage];
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    sdk(); assert(config->dma_desc_num == 6 && config->dma_frame_num == 240 && config->auto_clear);
    assert(tx && rx && config->dma_buffer_in_psram == TEST_EXPECT_PSRAM_DMA);
    ++channel_allocations;
    if (fail_new) return ESP_ERR_INVALID_STATE;
    assert(!tx_channel.allocated && !rx_channel.allocated);
    tx_channel = (struct test_i2s_channel){.allocated = true};
    rx_channel = (struct test_i2s_channel){.allocated = true};
    memset(dma_storage, 0, sizeof(dma_storage));
    *tx = &tx_channel; *rx = &rx_channel; return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config)
{
    sdk(); assert(channel->allocated && !channel->initialized);
    assert(config->clk_cfg.mclk_multiple == 256 && config->slot_cfg.bits == 16 && config->slot_cfg.mode == 2);
    assert(config->gpio_cfg.dout == 52 && config->gpio_cfg.din == 40);
    ++dma_init_calls;
    if (fail_dma_channel == (channel == &tx_channel ? 1U : 2U)) return ESP_ERR_NO_MEM;
    if (fail_init) return ESP_FAIL;
    channel->initialized = true; return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    sdk(); assert(channel->initialized && !channel->enabled);
    if (fail_enable_channel == (channel == &tx_channel ? 1U : 2U)) return ESP_ERR_TIMEOUT;
    channel->enabled = true;
    if (channel == &tx_channel) ++tx_enables; else ++rx_enables;
    if (stop_on_enable) { stop_on_enable = false; assert(port.stop(NULL, s.generation) == AE_AGAIN); }
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel)
{
    sdk(); assert(channel->initialized && channel->enabled);
    if (fail_disable || fail_disable_channel == (channel == &tx_channel ? 1U : 2U)) return ESP_FAIL;
    channel->enabled = false; channel->preload = 0; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t channel)
{
    sdk(); assert(channel->allocated && !channel->enabled);
    if (fail_delete) return ESP_FAIL;
    channel->allocated = channel->initialized = false; return ESP_OK;
}
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t channel, const void *data, size_t bytes, size_t *actual)
{
    sdk(); assert(channel == &tx_channel && channel->initialized && !channel->enabled);
    assert(data == buffers.wire);
    const uint8_t *pcm = data;
    for (size_t i = 0; i < bytes; ++i) assert(pcm[i] == 0);
    size_t available = DMA_BYTES - channel->preload;
    *actual = bytes < available ? bytes : available;
    memcpy(dma_storage + channel->preload, data, *actual);
    channel->preload += *actual;
    return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void *data, size_t bytes, size_t *actual, uint32_t timeout)
{
    sdk(); ++io_calls; assert(channel == &rx_channel && channel->enabled && timeout == 20);
    assert(data == buffers.wire);
    /* Pinned SDK initializes TX first as master and RX second as its slave.
     * Enabled RX cannot complete a DMA buffer without the TX clock source. */
    if (!tx_channel.enabled) { *actual = 0; return ESP_ERR_TIMEOUT; }
    *actual = bytes < transfer_limit ? bytes : transfer_limit;
    uint8_t *output = data;
    for (size_t i = 0; i < *actual; ++i) output[i] = (uint8_t)(i + 1U);
    if (custom_capture) {
        for (size_t i = 0; i + 4U <= *actual; i += 4U) {
            sample_put(output + i, capture_samples[(i / 4U) % 2U]);
            sample_put(output + i + 2U, 32767); /* The right slot must not affect mono capture. */
        }
    }
    *actual += read_overreport;
    if (inject_stop) { inject_stop = false; assert(port.stop(NULL, s.generation) == AE_AGAIN); }
    if (inject_mute) { inject_mute = false; assert(port.mute(NULL, AE_MICROPHONE, true, s.generation) == AE_AGAIN); }
    return transfer_error;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *data, size_t bytes, size_t *actual, uint32_t timeout)
{
    sdk(); ++io_calls; assert(channel == &tx_channel && channel->enabled && timeout == 20);
    assert(data == buffers.wire);
    *actual = bytes < transfer_limit ? bytes : transfer_limit;
    assert(*actual <= sizeof(emitted)); memcpy(emitted, data, *actual); emitted_bytes = *actual;
    memcpy(dma_storage, data, *actual);
    *actual += write_overreport; /* Explicit SDK boundary fault; never a buffer read. */
    return transfer_error;
}

static const ae_format_t mono = {16000, 1, 20, AE_PCM_S16_LE};
static void drain(void) { unsigned count = 0; while (service()) assert(++count < 10); }
static void open_audio(const ae_format_t *format, uint8_t roles, uint64_t generation)
{
    unsigned before = calls;
    assert(port.prepare(NULL, format, roles, generation) == AE_AGAIN);
    assert(calls == before);
    drain();
    assert(port.prepare(NULL, format, roles, generation) == AE_OK);
    assert(board_audio_mosaico_status().prepared && !pa_level && !s.capture_gate && !s.playback_gate);
    assert((registers[0x31] & 0x60) == 0x60);
}
static void unmute_role(ae_role_t role, uint64_t generation)
{
    unsigned before = calls;
    assert(port.mute(NULL, role, false, generation) == AE_AGAIN);
    assert(calls == before); drain();
    assert(port.mute(NULL, role, false, generation) == AE_OK);
}
static void close_audio(uint64_t generation, ae_result_t expected)
{
    unsigned before = calls;
    assert(port.stop(NULL, generation) == AE_AGAIN && !s.capture_gate && !s.playback_gate);
    assert(calls == before); drain();
    assert(port.stop(NULL, generation) == expected);
    assert(!s.prepared && !pa_level && references[0] == 0 && references[1] == 0);
    assert_cleared(&buffers, sizeof(buffers));
}

static void formats_and_pcm(void)
{
    ae_format_t bad = mono;
    bad.sample_rate_hz = 22050;
    assert(port.prepare(NULL, &bad, 3, 1) == AE_NOT_SUPPORTED);
    bad = mono; bad.channels = 2;
    assert(port.prepare(NULL, &bad, 3, 1) == AE_NOT_SUPPORTED);
    bad = (ae_format_t){48000, 2, 20, AE_PCM_S16_LE};
    assert(port.prepare(NULL, &bad, AE_SPEAKER, 1) == AE_NOT_SUPPORTED);
    open_audio(&mono, 3, 1);
    uint8_t pcm[640] = {0}; size_t actual = 99;
    assert(port.read(NULL, 1, pcm, sizeof(pcm), &actual) == AE_PRIVACY && actual == 0);
    assert(port.mute(NULL, AE_MICROPHONE, false, 2) == AE_STALE);
    assert(port.gain(NULL, 7, 1) == AE_NOT_SUPPORTED);
    assert(port.volume(NULL, 101, 1) == AE_INVALID);
    assert(port.gain(NULL, 18, 1) == AE_AGAIN); drain();
    assert(port.gain(NULL, 18, 1) == AE_OK && registers[0x16] == 3 && !s.capture_gate);
    assert(port.volume(NULL, 60, 1) == AE_AGAIN); drain();
    assert(port.volume(NULL, 60, 1) == AE_OK && !s.playback_gate);
    unmute_role(AE_MICROPHONE, 1);
    assert(port.read(NULL, 1, pcm, 3, &actual) == AE_INVALID && actual == 0);
    unsigned before = calls;
    assert(port.read(NULL, 1, pcm, sizeof(pcm), &actual) == AE_AGAIN && calls == before);
    assert(port.read(NULL, 1, pcm, sizeof(pcm), &actual) == AE_AGAIN);
    transfer_limit = 8; transfer_error = ESP_ERR_TIMEOUT;
    drain();
    assert(port.read(NULL, 1, pcm, sizeof(pcm), &actual) == AE_TIMEOUT && actual == 4);
    assert(pcm[0] == 1 && pcm[1] == 2 && pcm[2] == 5 && pcm[3] == 6);
    transfer_limit = 3;
    assert(port.read(NULL, 1, pcm, 4, &actual) == AE_AGAIN); drain();
    assert(port.read(NULL, 1, pcm, 4, &actual) == AE_INTEGRITY && actual == 0);
    transfer_limit = SIZE_MAX; transfer_error = ESP_OK;
    unmute_role(AE_SPEAKER, 1);
    const uint8_t samples[] = {0x00, 0x80, 0xff, 0x7f};
    assert(port.write(NULL, 1, samples, sizeof(samples), &actual) == AE_AGAIN); drain();
    assert(port.write(NULL, 1, samples, sizeof(samples), &actual) == AE_OK && actual == 4);
    const uint8_t expected[] = {0x00,0x80,0x00,0x80,0xff,0x7f,0xff,0x7f};
    assert(emitted_bytes == sizeof(expected) && !memcmp(emitted, expected, sizeof(expected)));
    close_audio(1, AE_OK);
    assert(port.prepare(NULL, &mono, 3, 1) == AE_STALE);
    const ae_format_t stereo = {48000, 2, 10, AE_PCM_S16_LE};
    open_audio(&stereo, AE_SPEAKER, 2);
    assert(port.mute(NULL, AE_MICROPHONE, false, 2) == AE_NOT_SUPPORTED);
    unmute_role(AE_SPEAKER, 2);
    const uint8_t stereo_pcm[] = {0xff,0x7f,0x01,0x80,0xe8,0x03,0xb8,0x0b};
    assert(port.write(NULL, 2, stereo_pcm, sizeof(stereo_pcm), &actual) == AE_AGAIN); drain();
    assert(port.write(NULL, 2, stereo_pcm, sizeof(stereo_pcm), &actual) == AE_OK && actual == 8);
    const uint8_t mixed[] = {0,0,0,0,0xd0,0x07,0xd0,0x07};
    assert(emitted_bytes == 8 && !memcmp(emitted, mixed, 8));
    transfer_limit = 4; transfer_error = ESP_ERR_TIMEOUT;
    assert(port.write(NULL, 2, stereo_pcm, sizeof(stereo_pcm), &actual) == AE_AGAIN); drain();
    assert(port.write(NULL, 2, stereo_pcm, sizeof(stereo_pcm), &actual) == AE_TIMEOUT && actual == 4);
    assert(emitted_bytes == 4 && !memcmp(emitted, mixed, 4));
    transfer_limit = SIZE_MAX; transfer_error = ESP_OK;
    close_audio(2, AE_OK);
}

static void privacy_and_failures(void)
{
    const uint64_t offset = s.last_generation - 2;
    open_audio(&mono, 3, offset + 3); unmute_role(AE_MICROPHONE, offset + 3);
    uint8_t pcm[32]; memset(pcm, 0xa5, sizeof(pcm)); size_t actual;
    assert(port.read(NULL, offset + 3, pcm, sizeof(pcm), &actual) == AE_AGAIN);
    inject_mute = true; drain();
    assert(port.read(NULL, offset + 3, pcm, sizeof(pcm), &actual) == AE_STALE && actual == 0);
    for (size_t i = 0; i < sizeof(pcm); ++i) assert(pcm[i] == 0xa5);
    assert(port.mute(NULL, AE_MICROPHONE, true, 999) == AE_OK);
    assert(!s.rx_running && !s.capture_gate);
    /* Safety mute cancels an unmute that has not reached hardware. */
    assert(port.mute(NULL, AE_MICROPHONE, false, offset + 3) == AE_AGAIN);
    assert(port.mute(NULL, AE_MICROPHONE, true, offset + 3) == AE_AGAIN); drain();
    assert(port.mute(NULL, AE_MICROPHONE, true, offset + 3) == AE_OK);
    assert(port.mute(NULL, AE_MICROPHONE, false, offset + 3) == AE_STALE && !s.rx_running);
    unmute_role(AE_MICROPHONE, offset + 3);
    assert(port.read(NULL, offset + 3, pcm, sizeof(pcm), &actual) == AE_AGAIN); drain();
    /* A completed read is still private if mute precedes result delivery. */
    assert(port.mute(NULL, AE_MICROPHONE, true, offset + 3) == AE_AGAIN);
    assert(port.read(NULL, offset + 3, pcm, sizeof(pcm), &actual) == AE_STALE && actual == 0);
    drain(); assert(port.mute(NULL, AE_MICROPHONE, true, offset + 3) == AE_OK);
    unmute_role(AE_MICROPHONE, offset + 3);
    assert(port.read(NULL, offset + 3, pcm, sizeof(pcm), &actual) == AE_AGAIN);
    inject_stop = true; drain();
    assert(port.stop(NULL, offset + 3) == AE_OK && !s.prepared);
    assert(!s.slot && !s.rx_running);
    open_audio(&mono, 3, offset + 4); unmute_role(AE_SPEAKER, offset + 4);
    /* Muting capture must not destroy an independently queued speaker frame. */
    const uint8_t speaker_pcm[] = {0x22, 0x11, 0x44, 0x33};
    assert(port.write(NULL, offset + 4, speaker_pcm, sizeof(speaker_pcm), &actual) == AE_AGAIN);
    assert(port.mute(NULL, AE_MICROPHONE, true, offset + 4) == AE_AGAIN); drain();
    assert(port.mute(NULL, AE_MICROPHONE, true, offset + 4) == AE_OK);
    assert(port.write(NULL, offset + 4, speaker_pcm, sizeof(speaker_pcm), &actual) == AE_OK && actual == 4);
    assert(emitted[0] == 0x22 && emitted[1] == 0x11 && emitted[4] == 0x44 && emitted[5] == 0x33);
    fail_register = 0x31; fail_write = true;
    assert(port.mute(NULL, AE_SPEAKER, true, offset + 4) == AE_AGAIN); drain();
    /* The actual pinned driver's mute returns its successful read and discards
     * the failed write. The adapter's sticky bus check must reject this. */
    assert(port.mute(NULL, AE_SPEAKER, true, offset + 4) == AE_UNCERTAIN);
    assert(board_audio_mosaico_status().cleanup_uncertain);
    fail_write = false; fail_register = -1;
    close_audio(offset + 4, AE_OK);
    open_audio(&mono, 3, offset + 5); unmute_role(AE_MICROPHONE, offset + 5);
    fail_disable = true;
    close_audio(offset + 5, AE_UNCERTAIN);
    assert(s.rx && s.rx_running && board_audio_mosaico_status().cleanup_uncertain);
    assert(port.prepare(NULL, &mono, 3, offset + 6) == AE_BUSY);
    fail_disable = false;
    close_audio(offset + 5, AE_OK);
    fail_new = true; unsigned rails_before = rail_calls;
    assert(port.prepare(NULL, &mono, 3, offset + 6) == AE_AGAIN); drain();
    assert(port.prepare(NULL, &mono, 3, offset + 6) == AE_BUSY && rail_calls == rails_before);
    fail_new = false;
    fail_init = true;
    assert(port.prepare(NULL, &mono, 3, offset + 7) == AE_AGAIN); drain();
    assert(port.prepare(NULL, &mono, 3, offset + 7) == AE_IO_ERROR);
    assert(!s.tx && !s.rx && !board_audio_mosaico_status().cleanup_uncertain);
    fail_init = false;
    open_audio(&mono, 3, offset + 8);
    fail_remove = true;
    close_audio(offset + 8, AE_UNCERTAIN);
    assert(s.control.device && board_audio_mosaico_status().cleanup_uncertain);
    fail_remove = false;
    close_audio(offset + 8, AE_OK);
    open_audio(&mono, 3, offset + 9);
    fail_register = 0x32; fail_write = true;
    assert(port.volume(NULL, 50, offset + 9) == AE_AGAIN); drain();
    assert(port.volume(NULL, 50, offset + 9) == AE_TIMEOUT && !s.playback_gate);
    fail_register = 0x16;
    assert(port.gain(NULL, 24, offset + 9) == AE_AGAIN); drain();
    assert(port.gain(NULL, 24, offset + 9) != AE_OK && !s.capture_gate);
    fail_register = -1; fail_write = false; corrupt_volume = true;
    assert(port.volume(NULL, 50, offset + 9) == AE_AGAIN); drain();
    assert(port.volume(NULL, 50, offset + 9) == AE_INTEGRITY && !s.playback_gate);
    corrupt_volume = false;
    close_audio(offset + 9, AE_OK);
    /* Pinned driver enable acquires its reference before the failing start.
     * No private reference repair, false teardown acknowledgement or re-open. */
    fail_start = true;
    assert(port.prepare(NULL, &mono, 3, offset + 10) == AE_AGAIN); drain();
    assert(port.prepare(NULL, &mono, 3, offset + 10) == AE_UNCERTAIN);
    assert(s.codec && s.control.device && s.codec_lifecycle_uncertain && references[1] == 1);
    fail_start = false;
    assert(port.stop(NULL, offset + 10) == AE_AGAIN); drain();
    assert(port.stop(NULL, offset + 10) == AE_UNCERTAIN && !s.tx && !s.rx && !s.capture_gate && !s.playback_gate);
    assert(port.prepare(NULL, &mono, 3, offset + 11) == AE_BUSY);
}

static void dma_allocation_failures(void)
{
    for (unsigned failed_channel = 1; failed_channel <= 2; ++failed_channel) {
        const uint64_t generation = s.last_generation + 1;
        const unsigned allocations_before = channel_allocations;
        const unsigned init_before = dma_init_calls;
        fail_dma_channel = failed_channel;
        assert(port.prepare(NULL, &mono, AE_MICROPHONE | AE_SPEAKER, generation) == AE_AGAIN);
        drain();
        assert(port.prepare(NULL, &mono, AE_MICROPHONE | AE_SPEAKER, generation) == AE_EXHAUSTED);
        assert(channel_allocations == allocations_before + 1 && dma_init_calls == init_before + failed_channel);
        assert(!s.tx && !s.rx && !tx_channel.allocated && !rx_channel.allocated);
        assert(!s.prepared && !s.capture_gate && !s.playback_gate && !s.power_owned && !pa_level);
        const board_audio_status_t status = board_audio_mosaico_status();
        assert(status.last_sdk_error == ESP_ERR_NO_MEM && !status.cleanup_uncertain);
        fail_dma_channel = 0;
        open_audio(&mono, AE_MICROPHONE | AE_SPEAKER, generation + 1);
        close_audio(generation + 1, AE_OK);
    }
}

static void mailbox_scratch_lifetime(void)
{
    const uint64_t generation = s.last_generation + 1;
    open_audio(&mono, AE_MICROPHONE | AE_SPEAKER, generation);
    unmute_role(AE_SPEAKER, generation);
    const uint8_t first[] = {0x12, 0x34, 0x56, 0x78};
    const uint8_t competing[] = {0x87, 0x65, 0x43, 0x21};
    size_t actual = 0;
    assert(port.write(NULL, generation, first, sizeof(first), &actual) == AE_AGAIN);
    assert(!memcmp(buffers.pcm, first, sizeof(first)));
    assert(port.write(NULL, generation, competing, sizeof(competing), &actual) == AE_BUSY);
    assert(!memcmp(buffers.pcm, first, sizeof(first)));
    drain();
    assert(port.write(NULL, generation, first, sizeof(first), &actual) == AE_OK && actual == sizeof(first));
    assert_cleared(buffers.pcm, sizeof(buffers.pcm));
    close_audio(generation, AE_OK);
}

static void advertised_profiles_match_actual_prepare(void)
{
    ae_codec_capabilities_t caps;
    assert(port.capabilities(NULL, &caps) == AE_OK && caps.available && caps.format_count == 14);
    const ae_format_t expected[] = {
        {8000,1,10,AE_PCM_S16_LE}, {8000,1,20,AE_PCM_S16_LE},
        {8000,2,10,AE_PCM_S16_LE}, {8000,2,20,AE_PCM_S16_LE},
        {16000,1,10,AE_PCM_S16_LE}, {16000,1,20,AE_PCM_S16_LE},
        {16000,2,10,AE_PCM_S16_LE}, {16000,2,20,AE_PCM_S16_LE},
        {44100,1,10,AE_PCM_S16_LE}, {44100,1,20,AE_PCM_S16_LE}, {44100,2,10,AE_PCM_S16_LE},
        {48000,1,10,AE_PCM_S16_LE}, {48000,1,20,AE_PCM_S16_LE}, {48000,2,10,AE_PCM_S16_LE},
    };
    bool usb_mono_48k_10ms = false, spoken_mono_16k_10ms = false;
    assert(caps.format_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t format = 0; format < sizeof(expected) / sizeof(expected[0]); ++format) {
        uint8_t roles = expected[format].channels == 1 ? AE_MICROPHONE | AE_SPEAKER : AE_SPEAKER;
        unsigned matches = 0;
        for (uint8_t i = 0; i < caps.format_count; ++i)
            if (same_format(&expected[format], &caps.formats[i].format) && caps.formats[i].roles == roles) ++matches;
        assert(matches == 1);
    }
    uint64_t generation = s.last_generation;
    for (uint8_t i = 0; i < caps.format_count; ++i) {
        const ae_profile_t *profile = &caps.formats[i];
        assert(!profile->qualified);
        open_audio(&profile->format, profile->roles, ++generation);
        if (profile->format.channels == 1 && profile->format.frame_ms == 10) {
            usb_mono_48k_10ms |= profile->format.sample_rate_hz == 48000;
            spoken_mono_16k_10ms |= profile->format.sample_rate_hz == 16000;
        }
        close_audio(generation, AE_OK);
    }
    assert(usb_mono_48k_10ms && spoken_mono_16k_10ms);
}

int main(void)
{
    port = board_audio_mosaico_port();
    ae_codec_capabilities_t caps;
    assert(port.capabilities(NULL, &caps) == AE_OK && caps.implemented && !caps.available && !caps.qualified);
    assert(port.stop(NULL, 1) == AE_UNAVAILABLE);
    fail_task = true; assert(board_audio_mosaico_init() == AE_EXHAUSTED);
    assert(task_creations == 1 && !s.task && !s.status.worker_started && !s.probe_pending);
    assert(!s.capture_gate && !s.playback_gate && !s.tx && !s.rx && !io_calls);
    fail_task = false; assert(board_audio_mosaico_init() == AE_OK);
    unsigned tasks = task_creations;
    assert(board_audio_mosaico_init() == AE_OK && task_creations == tasks);
    assert(port.capabilities(NULL, &caps) == AE_OK && !caps.available && caps.reason == AE_AGAIN);
    drain();
    assert(board_audio_mosaico_status().probe_complete && board_audio_mosaico_status().hardware_observed);
    assert(port.completion_notify);
    port.completion_notify(NULL, completion_wake, &completion_wakes);
    assert(port.capabilities(NULL, &caps) == AE_OK && caps.available && !caps.qualified && caps.format_count == 14);
    formats_and_pcm();
    advertised_profiles_match_actual_prepare();
    dma_allocation_failures();
    mailbox_scratch_lifetime();
    privacy_and_failures();
    assert(completion_wakes > 30);
    unsigned wakes_before = completion_wakes;
    port.completion_notify(NULL, NULL, NULL);
    assert(port.stop(NULL, s.generation) == AE_AGAIN); drain();
    assert(port.stop(NULL, s.generation) == AE_UNCERTAIN && completion_wakes == wakes_before);
    assert(lock_depth == 0 && io_calls > 5 && notifications > 10);
    puts("Board audio: real ES8311 driver, bounded worker mailbox, PCM, privacy and failure regressions passed.");
    printf("DMA policy: PSRAM=%u; 6 x 240 stereo frames per direction; TX/RX allocation failure and cleanup passed.\n",
        (unsigned)TEST_EXPECT_PSRAM_DMA);
    return 0;
}
