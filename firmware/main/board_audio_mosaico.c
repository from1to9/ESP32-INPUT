// SPDX-License-Identifier: Apache-2.0
#include "board_audio_mosaico.h"

#include <string.h>

#include "audio_codec_ctrl_if.h"
#include "audio_codec_if.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && !CONFIG_I2C_ISR_IRAM_SAFE && !CONFIG_I2S_ISR_IRAM_SAFE
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#define BOARD_AUDIO_EXTERNAL_STACK 1
#else
#define BOARD_AUDIO_EXTERNAL_STACK 0
#endif

#define CONTROL_TIMEOUT_MS 5
#define DMA_DESCRIPTORS 6U
#define DMA_FRAMES 240U
#define WIRE_BYTES_MAX (960U * 4U)
#define DMA_BYTES (DMA_DESCRIPTORS * DMA_FRAMES * 4U)

typedef enum { OP_PROBE, OP_PREPARE, OP_UNMUTE, OP_VOLUME, OP_GAIN, OP_READ, OP_WRITE } operation_t;
typedef enum { SLOT_EMPTY, SLOT_PENDING, SLOT_RUNNING, SLOT_DONE } slot_state_t;
typedef struct {
    operation_t operation;
    uint64_t generation;
    ae_format_t format;
    uint8_t roles;
    uint8_t value;
    size_t bytes;
    uint64_t safety_epoch;
} request_t;

typedef struct {
    audio_codec_ctrl_if_t base;
    i2c_master_dev_handle_t device;
    esp_err_t first_error;
    uint8_t volume_written;
    bool volume_write_observed;
} control_t;

/* Mailbox/status are protected by lock. All SDK handles and wire bytes belong
 * exclusively to worker. A request owns pcm until its result is consumed. */
static struct {
    portMUX_TYPE lock;
    TaskHandle_t task;
    board_audio_status_t status;
    slot_state_t slot;
    request_t request;
    ae_result_t result;
    size_t actual;
    uint64_t last_generation;
    uint64_t safety_epoch;
    bool capture_gate;
    bool playback_gate;
    bool stop_pending;
    bool stop_done;
    uint64_t stop_generation;
    ae_result_t stop_result;
    uint8_t mute_pending;
    uint8_t mute_done;
    ae_result_t mute_result[2];
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    bool tx_running;
    bool rx_running;
    bool speaker_unmuted;
    bool speaker_mute_known;
    bool tx_initialized;
    bool tx_dirty;
    bool power_owned;
    bool pa_owned;
    bool prepared;
    bool quarantined;
    bool probe_complete;
    bool probe_pending;
    bool probe_running;
    ae_result_t probe_result;
    bool hardware_observed;
    bool codec_lifecycle_uncertain;
    control_t control;
    const audio_codec_if_t *codec;
    ae_format_t format;
    uint8_t roles;
    uint64_t generation;
    void (*completion_notify)(void *);
    void *completion_context;
} s = { .lock = portMUX_INITIALIZER_UNLOCKED,
    .status = {.last_result = AE_UNAVAILABLE, .probe_result = AE_AGAIN}, .probe_result = AE_AGAIN };

/* Task-only mailbox PCM and synchronous I2S copy scratch. Keep driver handles,
 * locks and status internal; the SDK owns separate DMA payloads/descriptors.
 * External BSS placement is static and never falls back through heap allocation. */
static EXT_RAM_BSS_ATTR struct {
    uint8_t pcm[AE_FRAME_BYTES_MAX];
    uint8_t wire[WIRE_BYTES_MAX];
} buffers;

/* Worker/task-only observations; keep payload and aggregates out of internal
 * BSS. Scanning is outside the owner lock; only small updates/copies lock. */
static EXT_RAM_BSS_ATTR board_audio_pcm_snapshot_t pcm_observation;
static EXT_RAM_BSS_ATTR struct {
    board_audio_capture_snapshot_t view;
    uint64_t energy, last_seen_ms, published_ms;
    uint32_t samples;
    uint16_t peak;
    bool clock_known, published, clock_failed;
} capture_observation;

/* Caller holds the owner lock. Invalidation never changes audio behavior. */
static void invalidate_capture_level(ae_result_t result, bool reset)
{
    if (reset) memset(&capture_observation, 0, sizeof(capture_observation));
    capture_observation.view.known = capture_observation.view.current = false;
    capture_observation.view.result = result;
    capture_observation.energy = capture_observation.samples = capture_observation.peak = 0;
}

static bool same_format(const ae_format_t *a, const ae_format_t *b)
{
    return a->sample_rate_hz == b->sample_rate_hz && a->channels == b->channels &&
        a->frame_ms == b->frame_ms && a->encoding == b->encoding;
}

static bool valid_format(const ae_format_t *f, uint8_t roles)
{
    if (!f || !roles || (roles & ~(AE_MICROPHONE | AE_SPEAKER)) ||
        f->encoding != AE_PCM_S16_LE || (f->frame_ms != 10 && f->frame_ms != 20) ||
        (f->channels != 1 && f->channels != 2) ||
        (f->channels == 2 && (roles & AE_MICROPHONE))) return false;
    switch (f->sample_rate_hz) {
    case 8000: case 16000: case 44100: case 48000: break;
    default: return false;
    }
    return (uint64_t)f->sample_rate_hz * f->frame_ms * f->channels * 2U / 1000U <= AE_FRAME_BYTES_MAX;
}

static ae_result_t sdk_result(esp_err_t error)
{
    if (error == ESP_OK) return AE_OK;
    if (error == ESP_ERR_TIMEOUT) return AE_TIMEOUT;
    if (error == ESP_ERR_NO_MEM) return AE_EXHAUSTED;
    if (error == ESP_ERR_INVALID_STATE) return AE_BUSY;
    return AE_IO_ERROR;
}

static void remember(esp_err_t error, board_audio_sdk_operation_t operation)
{
    if (error == ESP_OK) return;
    if (s.control.first_error == ESP_OK) s.control.first_error = error;
    portENTER_CRITICAL(&s.lock);
    s.status.last_sdk_error = error;
    s.status.last_sdk_operation = operation;
    if (s.status.first_sdk_operation == BOARD_AUDIO_SDK_NONE) {
        s.status.first_sdk_error = error;
        s.status.first_sdk_operation = operation;
    }
    portEXIT_CRITICAL(&s.lock);
}

static bool ctrl_open(const audio_codec_ctrl_if_t *base)
{
    return ((const control_t *)base)->device != NULL;
}

static int ctrl_read(const audio_codec_ctrl_if_t *base, int reg, int reg_len,
                     void *data, int data_len)
{
    control_t *c = (control_t *)base;
    if (!c->device || reg_len != 1 || reg < 0 || reg > 255 || !data || data_len != 1) {
        remember(ESP_ERR_INVALID_ARG, BOARD_AUDIO_SDK_I2C_READ);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t address = (uint8_t)reg;
    esp_err_t error = i2c_master_transmit_receive(c->device, &address, 1,
        data, (size_t)data_len, CONTROL_TIMEOUT_MS);
    remember(error, BOARD_AUDIO_SDK_I2C_READ);
    return error;
}

static int ctrl_write(const audio_codec_ctrl_if_t *base, int reg, int reg_len,
                      void *data, int data_len)
{
    control_t *c = (control_t *)base;
    if (!c->device || reg_len != 1 || reg < 0 || reg > 255 || !data || data_len != 1) {
        remember(ESP_ERR_INVALID_ARG, BOARD_AUDIO_SDK_I2C_WRITE);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t bytes[2] = {(uint8_t)reg, *(uint8_t *)data};
    esp_err_t error = i2c_master_transmit(c->device, bytes, sizeof(bytes), CONTROL_TIMEOUT_MS);
    remember(error, BOARD_AUDIO_SDK_I2C_WRITE);
    if (error == ESP_OK && reg == 0x32) { c->volume_written = bytes[1]; c->volume_write_observed = true; }
    return error;
}

static int ctrl_info(const audio_codec_ctrl_if_t *base, audio_codec_ctrl_info_t *info)
{
    if (!ctrl_open(base) || !info) return ESP_ERR_INVALID_ARG;
    *info = (audio_codec_ctrl_info_t) { .type = AUDIO_CODEC_CTRL_I2C,
        .i2c = { .addr = BSP_AUDIO_CODEC_I2C_ADDR, .port = BSP_I2C_PORT } };
    return ESP_OK;
}

static ae_result_t codec_result(int result)
{
    /* Pinned ES8311 set_fs/mute/close discard some individual bus errors. */
    if (s.control.first_error != ESP_OK) return sdk_result(s.control.first_error);
    return result == ESP_CODEC_DEV_OK ? AE_OK : AE_IO_ERROR;
}

static ae_result_t dac_mute(bool muted)
{
    if (!s.codec) return AE_OK;
    portENTER_CRITICAL(&s.lock);
    pcm_observation.mute_requested = muted;
    pcm_observation.mute_read_known = pcm_observation.mute_matches = false;
    portEXIT_CRITICAL(&s.lock);
    s.control.first_error = ESP_OK;
    int result = s.codec->mute(s.codec, muted);
    uint8_t value = 0;
    int read_result = ctrl_read(&s.control.base, 0x31, 1, &value, 1);
    if (read_result == ESP_OK) {
        portENTER_CRITICAL(&s.lock);
        pcm_observation.mute_read_known = true;
        pcm_observation.mute_register = value;
        pcm_observation.mute_matches = (value & 0x60U) == (muted ? 0x60U : 0U);
        portEXIT_CRITICAL(&s.lock);
    }
    ae_result_t checked = codec_result(result);
    if (checked != AE_OK) return checked;
    if (read_result != ESP_OK || (value & 0x60U) != (muted ? 0x60U : 0U)) return AE_INTEGRITY;
    return AE_OK;
}

static ae_result_t codec_enable(bool on)
{
    s.control.first_error = ESP_OK;
    ae_result_t result = codec_result(s.codec->enable(s.codec, on));
    /* The pinned enable path acquires a reference before hardware writes. A
     * failed start leaves enabled=false without releasing that reference; a
     * failed suspend can leave enabled=true after releasing it. Retain this
     * singleton/identity until reboot rather than guessing the private state. */
    if (result != AE_OK) s.codec_lifecycle_uncertain = true;
    return result;
}

static ae_result_t pa(bool on)
{
    if (!s.pa_owned) return AE_OK;
    esp_err_t error = gpio_set_level(BSP_AUDIO_PA_CTRL, on ? 1 : 0);
    remember(error, BOARD_AUDIO_SDK_PA_WRITE);
    return sdk_result(error);
}

static ae_result_t flush_tx(void)
{
    if (!s.tx || !s.tx_initialized || !s.tx_dirty || s.tx_running) return s.tx_running ? AE_BUSY : AE_OK;
    memset(buffers.wire, 0, sizeof(buffers.wire));
    size_t total = 0;
    while (total < DMA_BYTES) {
        size_t requested = DMA_BYTES - total;
        if (requested > sizeof(buffers.wire)) requested = sizeof(buffers.wire);
        size_t loaded = 0;
        esp_err_t error = i2s_channel_preload_data(s.tx, buffers.wire, requested, &loaded);
        remember(error, BOARD_AUDIO_SDK_I2S_PRELOAD_TX);
        if (error != ESP_OK || loaded == 0 || loaded > requested || loaded % 4U) return AE_IO_ERROR;
        total += loaded;
    }
    s.tx_dirty = false;
    return AE_OK;
}

static ae_result_t start_tx_clock(void)
{
    if (s.tx_running) return AE_OK;
    ae_result_t result = flush_tx();
    if (result != AE_OK) return result;
    esp_err_t error = i2s_channel_enable(s.tx);
    remember(error, BOARD_AUDIO_SDK_I2S_ENABLE_TX);
    if (error == ESP_OK) s.tx_running = true;
    return sdk_result(error);
}

static ae_result_t stop_tx_clock(void)
{
    if (s.tx_running) {
        esp_err_t error = i2s_channel_disable(s.tx);
        remember(error, BOARD_AUDIO_SDK_I2S_DISABLE_TX);
        if (error != ESP_OK) return sdk_result(error);
        s.tx_running = false;
    }
    return flush_tx();
}

static bool interrupted(void);

static ae_result_t hardware_mute(ae_role_t role)
{
    ae_result_t result = AE_OK;
    if (role == AE_MICROPHONE) {
        if (s.rx_running) {
            esp_err_t error = i2s_channel_disable(s.rx);
            remember(error, BOARD_AUDIO_SDK_I2S_DISABLE_RX);
            if (error == ESP_OK) s.rx_running = false;
            else result = AE_UNCERTAIN;
        }
        if (!s.rx_running && !s.speaker_unmuted && stop_tx_clock() != AE_OK) result = AE_UNCERTAIN;
    } else {
        bool mute_acknowledged = pa(false) == AE_OK;
        if (dac_mute(true) != AE_OK) mute_acknowledged = false;
        if (mute_acknowledged) {
            s.speaker_unmuted = false;
            if (s.pa_owned && s.codec) s.speaker_mute_known = true;
        } else {
            s.speaker_mute_known = false;
            result = AE_UNCERTAIN;
        }
        /* TX is also the master clock for slave RX. Pause it only long enough
         * to purge old speaker DMA, then resume zero-filled clocks for an
         * independently active microphone. DAC/PA remain acknowledged muted. */
        if (stop_tx_clock() != AE_OK) result = AE_UNCERTAIN;
        if (result == AE_OK && s.rx_running && !interrupted() && start_tx_clock() != AE_OK) result = AE_UNCERTAIN;
    }
    memset(buffers.wire, 0, sizeof(buffers.wire));
    return result;
}

static ae_result_t hardware_stop(void)
{
    ae_result_t result = hardware_mute(AE_MICROPHONE);
    if (hardware_mute(AE_SPEAKER) != AE_OK) result = AE_UNCERTAIN;
    if (s.codec) {
        if (!s.codec_lifecycle_uncertain && codec_enable(false) == AE_OK) {
            s.control.first_error = ESP_OK;
            if (codec_result(s.codec->close(s.codec)) != AE_OK) s.codec_lifecycle_uncertain = true;
            if (!s.codec_lifecycle_uncertain) {
                /* A successful close is idempotent in the pinned codec. */
                if (audio_codec_delete_codec_if(s.codec) != ESP_CODEC_DEV_OK) result = AE_UNCERTAIN;
                s.codec = NULL;
            }
        }
        if (s.codec_lifecycle_uncertain) result = AE_UNCERTAIN;
    }
    if (s.tx && !s.tx_running) {
        esp_err_t error = i2s_del_channel(s.tx);
        remember(error, BOARD_AUDIO_SDK_I2S_DELETE_TX);
        if (error == ESP_OK) { s.tx = NULL; s.tx_initialized = s.tx_dirty = false; }
        else result = AE_UNCERTAIN;
    }
    if (s.rx && !s.rx_running) {
        esp_err_t error = i2s_del_channel(s.rx);
        remember(error, BOARD_AUDIO_SDK_I2S_DELETE_RX);
        if (error == ESP_OK) s.rx = NULL;
        else result = AE_UNCERTAIN;
    }
    if (s.power_owned) {
        esp_err_t error = bsp_power_set_codec_3v3(false);
        remember(error, BOARD_AUDIO_SDK_POWER_CODEC);
        if (error == ESP_OK) s.power_owned = false;
        else result = AE_UNCERTAIN;
    }
    if (s.control.device && !s.codec) {
        esp_err_t error = i2c_master_bus_rm_device(s.control.device);
        remember(error, BOARD_AUDIO_SDK_DEVICE_REMOVE);
        if (error == ESP_OK) s.control.device = NULL;
        else result = AE_UNCERTAIN;
    }
    s.prepared = false;
    s.quarantined = result != AE_OK;
    return result;
}

static ae_result_t hardware_prepare(const request_t *request)
{
    if (s.tx || s.rx || s.codec || s.control.device || s.power_owned) return AE_BUSY;
    portENTER_CRITICAL(&s.lock);
    pcm_observation = (board_audio_pcm_snapshot_t){.generation = request->generation,
        .generation_known = request->generation != 0};
    portEXIT_CRITICAL(&s.lock);
    s.control.first_error = ESP_OK;
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = DMA_DESCRIPTORS;
    channels.dma_frame_num = DMA_FRAMES;
    channels.auto_clear = true;
#if SOC_PSRAM_DMA_CAPABLE && CONFIG_SPIRAM && !CONFIG_I2S_ISR_IRAM_SAFE
    /* The SDK keeps descriptors internal and synchronizes PSRAM payloads.
     * Allocation failure must propagate, without retrying in internal RAM.
     * IRAM-safe auto-clear requires internal buffers in the pinned driver. */
    channels.dma_buffer_in_psram = true;
#endif
    /* Claim I2S before touching power/PA. A preexisting BSP audio owner fails. */
    esp_err_t error = i2s_new_channel(&channels, &s.tx, &s.rx);
    remember(error, BOARD_AUDIO_SDK_I2S_ALLOCATE);
    if (error != ESP_OK) return sdk_result(error);
    gpio_config_t gpio = { .pin_bit_mask = UINT64_C(1) << BSP_AUDIO_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT };
    error = gpio_config(&gpio);
    remember(error, BOARD_AUDIO_SDK_PA_CONFIG);
    if (error != ESP_OK) goto fail;
    s.pa_owned = true;
    error = gpio_set_level(BSP_AUDIO_PA_CTRL, 0);
    remember(error, BOARD_AUDIO_SDK_PA_WRITE);
    if (error != ESP_OK) goto fail;
    error = bsp_i2c_init();
    remember(error, BOARD_AUDIO_SDK_BUS_INIT);
    if (error != ESP_OK) goto fail;
    error = bsp_power_set_vcc_3v3(true);
    remember(error, BOARD_AUDIO_SDK_POWER_VCC);
    if (error != ESP_OK) goto fail;
    s.power_owned = true;
    error = bsp_power_set_codec_3v3(true);
    remember(error, BOARD_AUDIO_SDK_POWER_CODEC);
    if (error != ESP_OK) goto fail;
    vTaskDelay(pdMS_TO_TICKS(10));
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(request->format.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = BSP_AUDIO_I2S_MCLK, .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK, .dout = BSP_AUDIO_I2S_SDOUT, .din = BSP_AUDIO_I2S_DSIN },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    error = i2s_channel_init_std_mode(s.tx, &config);
    remember(error, BOARD_AUDIO_SDK_I2S_INIT_TX);
    if (error != ESP_OK) goto fail;
    s.tx_initialized = true;
    s.tx_dirty = true;
    error = i2s_channel_init_std_mode(s.rx, &config);
    remember(error, BOARD_AUDIO_SDK_I2S_INIT_RX);
    if (error != ESP_OK) goto fail;
    i2c_device_config_t device = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_AUDIO_CODEC_I2C_ADDR >> 1, .scl_speed_hz = 100000 };
    error = i2c_master_bus_add_device(bsp_i2c_get_handle(), &device, &s.control.device);
    remember(error, BOARD_AUDIO_SDK_DEVICE_ADD);
    if (error != ESP_OK) goto fail;
    s.control.base = (audio_codec_ctrl_if_t) { .is_open = ctrl_open, .read_reg = ctrl_read,
        .write_reg = ctrl_write, .get_info = ctrl_info };
    es8311_codec_cfg_t codec = { .ctrl_if = &s.control.base,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH, .pa_pin = -1,
        .use_mclk = true, .no_dac_ref = true, .mclk_div = 256,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f } };
    s.codec = es8311_codec_new(&codec);
    if (!s.codec || s.control.first_error != ESP_OK) { error = ESP_FAIL; goto fail; }
    esp_codec_dev_sample_info_t sample = { .sample_rate = request->format.sample_rate_hz,
        .bits_per_sample = 16, .channel = 2, .mclk_multiple = 256 };
    if (codec_result(s.codec->set_fs(s.codec, &sample)) != AE_OK ||
        codec_enable(true) != AE_OK || dac_mute(true) != AE_OK ||
        flush_tx() != AE_OK) { error = ESP_FAIL; goto fail; }
    s.format = request->format;
    s.roles = request->roles;
    s.generation = request->generation;
    s.prepared = true;
    s.speaker_unmuted = false;
    s.speaker_mute_known = true;
    s.hardware_observed = true;
    return AE_OK;
fail: {
    ae_result_t failure = sdk_result(error);
    if (hardware_stop() != AE_OK) return AE_UNCERTAIN;
    return failure;
}
}

static bool interrupted(void)
{
    portENTER_CRITICAL(&s.lock);
    bool value = s.stop_pending;
    portEXIT_CRITICAL(&s.lock);
    return value;
}

static ae_result_t hardware_unmute(ae_role_t role)
{
    if (interrupted()) return AE_STALE;
    esp_err_t error;
    if (role == AE_MICROPHONE) {
        if (s.rx_running) return AE_OK;
        /* TX was initialized first and is the full-duplex master. Enabling
         * slave RX alone cannot produce BCLK/WS or any capture DMA frames.
         * Clock-only TX never opens DAC/PA or the playback permission gate. */
        bool clock_was_running = s.tx_running;
        ae_result_t clock_result = start_tx_clock();
        if (clock_result != AE_OK) return clock_result;
        if (interrupted()) {
            if (!clock_was_running && !s.speaker_unmuted && stop_tx_clock() != AE_OK) return AE_UNCERTAIN;
            return AE_STALE;
        }
        error = i2s_channel_enable(s.rx);
        remember(error, BOARD_AUDIO_SDK_I2S_ENABLE_RX);
        if (error != ESP_OK) {
            if (!clock_was_running && !s.speaker_unmuted && stop_tx_clock() != AE_OK) return AE_UNCERTAIN;
            return sdk_result(error);
        }
        s.rx_running = true;
    } else {
        ae_result_t clock_result = start_tx_clock();
        if (clock_result != AE_OK) return clock_result;
        if (interrupted()) {
            if (!s.rx_running && stop_tx_clock() != AE_OK) return AE_UNCERTAIN;
            return AE_STALE;
        }
        if (dac_mute(false) != AE_OK || pa(true) != AE_OK) {
            s.speaker_mute_known = false;
            (void)hardware_mute(AE_SPEAKER);
            return AE_UNCERTAIN;
        }
        s.speaker_unmuted = true;
        s.speaker_mute_known = true;
    }
    return AE_OK;
}

static int16_t sample_get(const uint8_t *data)
{
    uint16_t value = (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
    return value < 0x8000U ? (int16_t)value : (int16_t)((int32_t)value - 65536);
}

static void sample_put(uint8_t *data, int16_t value)
{
    uint16_t bits = (uint16_t)value;
    data[0] = (uint8_t)bits;
    data[1] = (uint8_t)(bits >> 8);
}

static void pcm_count(uint64_t *value, uint64_t amount)
{
    if (amount > UINT64_MAX - *value) {
        *value = UINT64_MAX; pcm_observation.saturated = true;
    } else *value += amount;
}
typedef struct {
    uint64_t energy;
    int64_t now_ms;
    uint32_t samples;
    uint16_t peak;
} capture_measurement_t;

static capture_measurement_t measure_capture(size_t bytes)
{
    capture_measurement_t measurement = {.samples = (uint32_t)(bytes / 2U)};
    int64_t now_us = esp_timer_get_time();
    measurement.now_ms = now_us < 0 ? -1 : now_us / 1000;
    /* The worker exclusively owns this successful read's mono PCM scratch. */
    for (size_t i = 0; i < bytes; i += 2U) {
        int32_t sample = sample_get(buffers.pcm + i);
        uint16_t magnitude = (uint16_t)(sample < 0 ? -sample : sample);
        measurement.energy += (uint64_t)((int64_t)sample * sample);
        if (magnitude > measurement.peak) measurement.peak = magnitude;
    }
    return measurement;
}

static void publish_capture_level(const request_t *request, const capture_measurement_t *measurement)
{
    /* Called under lock only after the normal completion/permission checks. */
    if (measurement->now_ms < 0 || capture_observation.clock_failed ||
        (capture_observation.clock_known && (uint64_t)measurement->now_ms < capture_observation.last_seen_ms)) {
        capture_observation.clock_failed = true;
        invalidate_capture_level(AE_TIME_UNKNOWN, false); return;
    }
    uint64_t now = (uint64_t)measurement->now_ms;
    capture_observation.last_seen_ms = now; capture_observation.clock_known = true;
    if (measurement->energy > UINT64_MAX - capture_observation.energy ||
        measurement->samples > UINT32_MAX - capture_observation.samples) {
        invalidate_capture_level(AE_EXHAUSTED, false); return;
    }
    capture_observation.energy += measurement->energy; capture_observation.samples += measurement->samples;
    if (measurement->peak > capture_observation.peak) capture_observation.peak = measurement->peak;
    capture_observation.view.result = AE_OK;
    if (!capture_observation.published || now - capture_observation.published_ms >= BOARD_AUDIO_CAPTURE_LEVEL_INTERVAL_MS) {
        capture_observation.view = (board_audio_capture_snapshot_t){.known = true,
            .generation = request->generation, .observed_ms = now, .sum_squares = capture_observation.energy,
            .samples = capture_observation.samples, .peak_abs = capture_observation.peak, .result = AE_OK};
        capture_observation.published = true; capture_observation.published_ms = now;
        capture_observation.energy = capture_observation.samples = capture_observation.peak = 0;
    }
}
static void observe_pcm_write(esp_err_t sdk_result_, size_t requested, size_t reported)
{
    const bool valid = reported <= requested && reported % 4U == 0;
    uint16_t peak = 0;
    uint64_t samples = 0, nonzero = 0;
    if (valid) {
        samples = reported / 4U;
        /* SDK has returned; this worker still owns the wire scratch. Never
         * scan capture data or an unreported/requested-only suffix. */
        for (size_t i = 0; i < samples; ++i) {
            int32_t sample = sample_get(buffers.wire + i * 4U);
            uint16_t magnitude = (uint16_t)(sample < 0 ? -sample : sample);
            if (magnitude) ++nonzero;
            if (magnitude > peak) peak = magnitude;
        }
    }
    portENTER_CRITICAL(&s.lock);
    pcm_observation.write_result_known = true;
    pcm_observation.last_write_sdk_result = sdk_result_;
    pcm_observation.last_prefix_valid = valid;
    pcm_count(&pcm_observation.write_calls, 1);
    if (sdk_result_ != ESP_OK) pcm_count(&pcm_observation.failed_writes, 1);
    if (!valid) pcm_count(&pcm_observation.invalid_prefix_writes, 1);
    if (samples) {
        pcm_observation.pcm_known = true;
        pcm_count(&pcm_observation.reported_samples, samples);
        pcm_count(&pcm_observation.reported_nonzero_samples, nonzero);
        if (peak > pcm_observation.reported_peak_abs) pcm_observation.reported_peak_abs = peak;
    }
    portEXIT_CRITICAL(&s.lock);
}

static ae_result_t hardware_pcm(const request_t *request, size_t *actual)
{
    bool reading = request->operation == OP_READ;
    size_t media_frame = (size_t)s.format.channels * 2U;
    size_t frames = request->bytes / media_frame;
    size_t wire_bytes = frames * 4U;
    *actual = 0;
    if (wire_bytes > sizeof(buffers.wire)) return AE_INVALID;
    if (!reading) {
        for (size_t i = 0; i < frames; ++i) {
            int32_t mono = sample_get(buffers.pcm + i * media_frame);
            if (s.format.channels == 2)
                mono = (mono + sample_get(buffers.pcm + i * media_frame + 2U)) / 2;
            sample_put(buffers.wire + i * 4U, (int16_t)mono);
            sample_put(buffers.wire + i * 4U + 2U, (int16_t)mono);
        }
    }
    size_t transferred = 0;
    if (!reading) s.tx_dirty = true;
    esp_err_t error = reading ? i2s_channel_read(s.rx, buffers.wire, wire_bytes, &transferred, AE_IO_TIMEOUT_MS) :
        i2s_channel_write(s.tx, buffers.wire, wire_bytes, &transferred, AE_IO_TIMEOUT_MS);
    if (!reading) observe_pcm_write(error, wire_bytes, transferred);
    remember(error, reading ? BOARD_AUDIO_SDK_I2S_READ : BOARD_AUDIO_SDK_I2S_WRITE);
    if (transferred > wire_bytes || transferred % 4U) return AE_INTEGRITY;
    frames = transferred / 4U;
    if (reading) {
        for (size_t i = 0; i < frames; ++i) memcpy(buffers.pcm + i * 2U, buffers.wire + i * 4U, 2U);
    }
    *actual = frames * media_frame;
    if (error == ESP_OK && transferred != wire_bytes) return AE_TIMEOUT;
    return sdk_result(error);
}

static ae_result_t execute(const request_t *request, size_t *actual)
{
    *actual = 0;
    if (request->operation == OP_PROBE) {
        ae_result_t result = hardware_prepare(request);
        if (result == AE_OK) result = hardware_stop();
        s.probe_complete = true;
        s.probe_result = result;
        if (result != AE_OK) s.hardware_observed = false;
        return result;
    }
    if (request->operation == OP_PREPARE) return hardware_prepare(request);
    if (!s.prepared || request->generation != s.generation || interrupted()) return AE_STALE;
    switch (request->operation) {
    case OP_UNMUTE: return hardware_unmute((ae_role_t)request->roles);
    case OP_VOLUME: {
        portENTER_CRITICAL(&s.lock);
        pcm_observation.volume_requested_known = true;
        pcm_observation.volume_requested = request->value;
        pcm_observation.volume_read_known = pcm_observation.volume_matches = false;
        portEXIT_CRITICAL(&s.lock);
        s.control.first_error = ESP_OK;
        s.control.volume_write_observed = false;
        ae_result_t result = codec_result(s.codec->set_vol(s.codec,
            request->value ? -50.0f + request->value * 0.5f : -96.0f));
        uint8_t volume = 0;
        if (result != AE_OK) return result;
        if (ctrl_read(&s.control.base, 0x32, 1, &volume, 1) != ESP_OK) return AE_IO_ERROR;
        portENTER_CRITICAL(&s.lock);
        pcm_observation.volume_read_known = true;
        pcm_observation.volume_register = volume;
        pcm_observation.volume_matches = s.control.volume_write_observed && volume == s.control.volume_written;
        portEXIT_CRITICAL(&s.lock);
        return s.control.volume_write_observed && volume == s.control.volume_written ? AE_OK : AE_INTEGRITY;
    }
    case OP_GAIN: {
        s.control.first_error = ESP_OK;
        ae_result_t result = codec_result(s.codec->set_mic_gain(s.codec, request->value));
        uint8_t gain = 255;
        if (ctrl_read(&s.control.base, 0x16, 1, &gain, 1) != ESP_OK) return AE_IO_ERROR;
        return result == AE_OK && gain != request->value / 6U ? AE_INTEGRITY : result;
    }
    case OP_READ: case OP_WRITE: return hardware_pcm(request, actual);
    default: return AE_INVALID;
    }
}

static void publish(ae_result_t result)
{
    if (result == AE_UNCERTAIN) s.quarantined = true;
    portENTER_CRITICAL(&s.lock);
    s.status.prepared = s.prepared;
    s.status.probe_complete = s.probe_complete;
    s.status.probe_result = s.probe_result;
    s.status.hardware_observed = s.hardware_observed;
    s.status.capture_running = s.rx_running;
    s.status.playback_running = s.tx_running && s.speaker_mute_known && s.speaker_unmuted;
    s.status.speaker_muted = s.speaker_mute_known && !s.speaker_unmuted;
    s.status.speaker_mute_confirmed = s.speaker_mute_known;
    s.status.tx_clock_running = s.tx_running;
    s.status.generation = s.generation;
    s.status.format = s.format;
    s.status.roles = s.roles;
    s.status.cleanup_uncertain = s.quarantined;
    if (s.quarantined) s.capture_gate = s.playback_gate = false;
    if (result != AE_OK && result != AE_AGAIN) invalidate_capture_level(result, false);
    s.status.last_result = result;
    portEXIT_CRITICAL(&s.lock);
}

static void notify_completion(void)
{
    portENTER_CRITICAL(&s.lock);
    void (*notify)(void *) = s.completion_notify;
    void *context = s.completion_context;
    portEXIT_CRITICAL(&s.lock);
    if (notify) notify(context);
}

/* One service turn is also exercised by boundary tests. No SDK substitution or
 * special scheduling branch exists in the production adapter. */
static bool service(void)
{
    portENTER_CRITICAL(&s.lock);
    if (s.stop_pending) {
        portEXIT_CRITICAL(&s.lock);
        ae_result_t result = hardware_stop();
        portENTER_CRITICAL(&s.lock);
        s.stop_pending = false;
        s.stop_done = true;
        s.stop_result = result;
        s.mute_pending = s.mute_done = 0;
        if (s.slot != SLOT_EMPTY) { s.result = AE_STALE; s.actual = 0; s.slot = SLOT_DONE; }
        portEXIT_CRITICAL(&s.lock);
        publish(result);
        notify_completion();
        return true;
    }
    if (s.mute_pending) {
        uint8_t role = (s.mute_pending & AE_MICROPHONE) ? AE_MICROPHONE : AE_SPEAKER;
        portEXIT_CRITICAL(&s.lock);
        ae_result_t result = hardware_mute((ae_role_t)role);
        portENTER_CRITICAL(&s.lock);
        s.mute_pending &= (uint8_t)~role;
        s.mute_done |= role;
        s.mute_result[role == AE_MICROPHONE ? 0 : 1] = result;
        portEXIT_CRITICAL(&s.lock);
        publish(result);
        notify_completion();
        return true;
    }
    if (s.probe_pending && s.slot == SLOT_EMPTY) {
        /* Readiness is a one-time boot obligation, not an ordinary request.
         * Initial ae_init stop may precede it; stop acknowledgement must not
         * erase or overwrite a queued/running probe. No PCM path is enabled. */
        s.probe_pending = false;
        s.probe_running = true;
        portEXIT_CRITICAL(&s.lock);
        const request_t probe = {.operation = OP_PROBE, .format = {16000, 1, 20, AE_PCM_S16_LE},
            .roles = AE_MICROPHONE | AE_SPEAKER};
        size_t actual = 0;
        ae_result_t result = execute(&probe, &actual);
        portENTER_CRITICAL(&s.lock);
        s.probe_running = false;
        portEXIT_CRITICAL(&s.lock);
        publish(result);
        notify_completion();
        return true;
    }
    if (s.slot != SLOT_PENDING) { portEXIT_CRITICAL(&s.lock); return false; }
    request_t request = s.request;
    s.slot = SLOT_RUNNING;
    bool gated = (request.operation == OP_READ && !s.capture_gate) ||
        (request.operation == OP_WRITE && !s.playback_gate);
    portEXIT_CRITICAL(&s.lock);
    size_t actual = 0;
    ae_result_t result = gated ? AE_STALE : execute(&request, &actual);
    capture_measurement_t measurement = {0};
    if (request.operation == OP_READ && result == AE_OK && actual == request.bytes)
        measurement = measure_capture(actual);
    portENTER_CRITICAL(&s.lock);
    if (s.stop_pending || (request.operation == OP_UNMUTE && request.safety_epoch != s.safety_epoch) ||
        (request.operation == OP_READ && !s.capture_gate) ||
        (request.operation == OP_WRITE && !s.playback_gate)) {
        result = AE_STALE;
        actual = 0;
        memset(buffers.pcm, 0, sizeof(buffers.pcm));
    }
    if (request.operation == OP_UNMUTE && result == AE_OK) {
        if (request.roles == AE_MICROPHONE && !(s.mute_pending & AE_MICROPHONE)) s.capture_gate = true;
        if (request.roles == AE_SPEAKER && !(s.mute_pending & AE_SPEAKER)) s.playback_gate = true;
    }
    if (request.operation == OP_READ) {
        if (result == AE_OK && actual == request.bytes && measurement.samples && request.generation == s.generation)
            publish_capture_level(&request, &measurement);
        else invalidate_capture_level(result == AE_OK ? AE_INTEGRITY : result, false);
    }
    s.result = result;
    s.actual = actual;
    s.slot = SLOT_DONE;
    portEXIT_CRITICAL(&s.lock);
    publish(result);
    notify_completion();
    return true;
}

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (service()) { }
    }
}

static bool same_request(const request_t *a, const request_t *b)
{
    return a->operation == b->operation && a->generation == b->generation &&
        a->roles == b->roles && a->value == b->value && a->bytes == b->bytes &&
        same_format(&a->format, &b->format);
}

static ae_result_t exchange(const request_t *request, void *data, size_t *actual)
{
    if (actual) *actual = 0;
    portENTER_CRITICAL(&s.lock);
    if (!s.status.worker_started) { portEXIT_CRITICAL(&s.lock); return AE_UNAVAILABLE; }
    if (s.slot != SLOT_EMPTY) {
        if (!same_request(request, &s.request) || (request->operation == OP_WRITE &&
            memcmp(data, buffers.pcm, request->bytes))) {
            portEXIT_CRITICAL(&s.lock); return AE_BUSY;
        }
        if (s.slot != SLOT_DONE) { portEXIT_CRITICAL(&s.lock); return AE_AGAIN; }
        ae_result_t result = s.result;
        size_t confirmed = s.actual;
        if (s.stop_pending || (request->operation == OP_READ && !s.capture_gate)) {
            result = AE_STALE;
            confirmed = 0;
        }
        if (actual) *actual = confirmed;
        if (request->operation == OP_READ && confirmed) memcpy(data, buffers.pcm, confirmed);
        memset(buffers.pcm, 0, sizeof(buffers.pcm));
        s.slot = SLOT_EMPTY;
        portEXIT_CRITICAL(&s.lock);
        return result;
    }
    if (s.stop_pending || s.stop_done || s.mute_pending || s.probe_pending || s.probe_running) {
        portEXIT_CRITICAL(&s.lock); return AE_AGAIN;
    }
    if (request->operation == OP_PREPARE) {
        if (request->generation <= s.last_generation) { portEXIT_CRITICAL(&s.lock); return AE_STALE; }
        if (s.status.prepared || s.status.cleanup_uncertain) { portEXIT_CRITICAL(&s.lock); return AE_BUSY; }
        s.last_generation = request->generation;
        invalidate_capture_level(AE_AGAIN, true);
    } else {
        if (s.status.cleanup_uncertain) { portEXIT_CRITICAL(&s.lock); return AE_UNCERTAIN; }
        if (!s.status.prepared || request->generation != s.status.generation) {
            portEXIT_CRITICAL(&s.lock); return AE_STALE;
        }
        if (request->operation == OP_UNMUTE && !(s.status.roles & request->roles)) {
            portEXIT_CRITICAL(&s.lock); return AE_NOT_SUPPORTED;
        }
        if ((request->operation == OP_READ && !s.capture_gate) ||
            (request->operation == OP_WRITE && !s.playback_gate)) {
            portEXIT_CRITICAL(&s.lock); return AE_PRIVACY;
        }
        if (request->operation == OP_READ || request->operation == OP_WRITE) {
            size_t frame = (size_t)s.status.format.channels * 2U;
            size_t max_bytes = (size_t)s.status.format.sample_rate_hz * s.status.format.frame_ms * frame / 1000U;
            if (request->bytes > max_bytes || request->bytes % frame) {
                portEXIT_CRITICAL(&s.lock); return AE_INVALID;
            }
        }
    }
    s.request = *request;
    s.request.safety_epoch = s.safety_epoch;
    if (request->operation == OP_WRITE) memcpy(buffers.pcm, data, request->bytes);
    s.slot = SLOT_PENDING;
    portEXIT_CRITICAL(&s.lock);
    xTaskNotifyGive(s.task);
    return AE_AGAIN;
}

static ae_result_t capabilities(void *context, ae_codec_capabilities_t *out)
{
    (void)context;
    if (!out) return AE_INVALID;
    board_audio_status_t status = board_audio_mosaico_status();
    *out = (ae_codec_capabilities_t) { .implemented = true,
        .available = status.hardware_observed && !status.cleanup_uncertain,
        .full_duplex = true, .safe_volume_max = 100,
        .reason = !status.worker_started ? AE_UNAVAILABLE : !status.probe_complete ? AE_AGAIN :
            status.hardware_observed ? AE_OK : status.probe_result };
    const uint32_t rates[] = {8000, 16000, 44100, 48000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        for (uint8_t channels = 1; channels <= 2; ++channels) {
            for (uint8_t frame_ms = 10; frame_ms <= 20; frame_ms += 10) {
                ae_format_t format = {rates[i], channels, frame_ms, AE_PCM_S16_LE};
                uint8_t roles = channels == 1 ? AE_MICROPHONE | AE_SPEAKER : AE_SPEAKER;
                if (!valid_format(&format, roles)) continue;
                if (out->format_count >= AE_FORMATS_MAX) return AE_EXHAUSTED;
                out->formats[out->format_count++] = (ae_profile_t){.format = format, .roles = roles};
            }
        }
    }
    if (status.cleanup_uncertain) out->reason = AE_UNCERTAIN;
    return AE_OK;
}

static ae_result_t prepare(void *context, const ae_format_t *format, uint8_t roles, uint64_t generation)
{
    (void)context;
    if (!generation || !format) return AE_INVALID;
    if (!valid_format(format, roles)) return AE_NOT_SUPPORTED;
    request_t request = {.operation = OP_PREPARE, .generation = generation, .format = *format, .roles = roles};
    return exchange(&request, NULL, NULL);
}

static ae_result_t mute(void *context, ae_role_t role, bool muted, uint64_t generation)
{
    (void)context;
    if (role != AE_MICROPHONE && role != AE_SPEAKER) return AE_INVALID;
    if (!muted) {
        request_t request = {.operation = OP_UNMUTE, .generation = generation, .roles = role};
        return exchange(&request, NULL, NULL);
    }
    portENTER_CRITICAL(&s.lock);
    if (!s.status.worker_started) { portEXIT_CRITICAL(&s.lock); return AE_UNAVAILABLE; }
    if (role == AE_MICROPHONE) { s.capture_gate = false; invalidate_capture_level(AE_AGAIN, true); }
    else s.playback_gate = false;
    if (s.mute_done & role) {
        s.mute_done &= (uint8_t)~role;
        ae_result_t result = s.mute_result[role == AE_MICROPHONE ? 0 : 1];
        portEXIT_CRITICAL(&s.lock); return result;
    }
    if (!(s.mute_pending & role)) ++s.safety_epoch;
    s.mute_pending |= role;
    if (s.slot == SLOT_PENDING &&
        ((s.request.operation == OP_UNMUTE && s.request.roles == role) ||
         (s.request.operation == OP_READ && role == AE_MICROPHONE) ||
         (s.request.operation == OP_WRITE && role == AE_SPEAKER))) {
        s.slot = SLOT_DONE;
        s.result = AE_STALE;
        s.actual = 0;
    }
    portEXIT_CRITICAL(&s.lock);
    xTaskNotifyGive(s.task);
    return AE_AGAIN;
}

static ae_result_t volume(void *context, uint8_t percent, uint64_t generation)
{
    (void)context;
    if (percent > 100) return AE_INVALID;
    request_t request = {.operation = OP_VOLUME, .generation = generation, .value = percent};
    return exchange(&request, NULL, NULL);
}

static ae_result_t gain(void *context, uint8_t db, uint64_t generation)
{
    (void)context;
    if (db > 42 || db % 6) return AE_NOT_SUPPORTED;
    request_t request = {.operation = OP_GAIN, .generation = generation, .value = db};
    return exchange(&request, NULL, NULL);
}

static ae_result_t read_pcm(void *context, uint64_t generation, void *data, size_t capacity, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual || !capacity || capacity > AE_FRAME_BYTES_MAX) return AE_INVALID;
    request_t request = {.operation = OP_READ, .generation = generation, .bytes = capacity};
    return exchange(&request, data, actual);
}

static ae_result_t write_pcm(void *context, uint64_t generation, const void *data, size_t bytes, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual || !bytes || bytes > AE_FRAME_BYTES_MAX) return AE_INVALID;
    request_t request = {.operation = OP_WRITE, .generation = generation, .bytes = bytes};
    return exchange(&request, (void *)data, actual);
}

static ae_result_t stop(void *context, uint64_t generation)
{
    (void)context;
    portENTER_CRITICAL(&s.lock);
    if (!s.status.worker_started) { portEXIT_CRITICAL(&s.lock); return AE_UNAVAILABLE; }
    s.capture_gate = s.playback_gate = false;
    invalidate_capture_level(AE_AGAIN, true);
    if (s.stop_done && generation == s.stop_generation) {
        ae_result_t result = s.stop_result;
        s.stop_done = false;
        s.slot = SLOT_EMPTY;
        memset(buffers.pcm, 0, sizeof(buffers.pcm));
        portEXIT_CRITICAL(&s.lock); return result;
    }
    if (!s.stop_pending) {
        ++s.safety_epoch;
        s.stop_generation = generation; s.stop_done = false; s.stop_pending = true;
    }
    portEXIT_CRITICAL(&s.lock);
    xTaskNotifyGive(s.task);
    return AE_AGAIN;
}

ae_result_t board_audio_mosaico_init(void)
{
    if (board_audio_mosaico_status().worker_started) return AE_OK;
#if BOARD_AUDIO_EXTERNAL_STACK
    /* This resident task only calls synchronous codec/I2C/I2S/GPIO APIs and
     * task notifications. It executes no flash/NVS operations. Keep cache-off
     * ISR configurations internal because I2C can borrow its caller buffer.
     * The SDK owns an internal TCB and unwinds partial allocation on failure;
     * PSRAM exhaustion must not fall back to internal RAM. */
    if (xTaskCreateWithCaps(worker, "board_audio", 6144, NULL, 5, &s.task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return AE_EXHAUSTED;
#else
    if (xTaskCreate(worker, "board_audio", 6144, NULL, 5, &s.task) != pdPASS) return AE_EXHAUSTED;
#endif
    portENTER_CRITICAL(&s.lock);
    s.probe_pending = true;
    s.status.worker_started = true;
    s.status.speaker_muted = true;
    portEXIT_CRITICAL(&s.lock);
    xTaskNotifyGive(s.task);
    return AE_OK;
}

static void completion_notify(void *context, void (*notify)(void *), void *notify_context)
{
    (void)context;
    portENTER_CRITICAL(&s.lock);
    s.completion_notify = notify;
    s.completion_context = notify_context;
    bool completed = s.slot == SLOT_DONE || s.stop_done || s.mute_done;
    portEXIT_CRITICAL(&s.lock);
    if (notify && completed) notify(notify_context);
}

ae_codec_port_t board_audio_mosaico_port(void)
{
    return (ae_codec_port_t) {.capabilities = capabilities, .prepare = prepare, .mute = mute,
        .volume = volume, .gain = gain, .read = read_pcm, .write = write_pcm, .stop = stop,
        .completion_notify = completion_notify};
}

board_audio_status_t board_audio_mosaico_status(void)
{
    portENTER_CRITICAL(&s.lock);
    board_audio_status_t status = s.status;
    portEXIT_CRITICAL(&s.lock);
    return status;
}

void board_audio_pcm_snapshot(board_audio_pcm_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s.lock);
    *out = pcm_observation;
    portEXIT_CRITICAL(&s.lock);
}

void board_audio_capture_snapshot(board_audio_capture_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s.lock);
    *out = capture_observation.view;
    out->current = out->known && s.capture_gate && !s.stop_pending && !(s.mute_pending & AE_MICROPHONE) &&
        s.status.prepared && s.status.capture_running && !s.status.cleanup_uncertain &&
        (s.status.roles & AE_MICROPHONE) && out->generation == s.status.generation;
    portEXIT_CRITICAL(&s.lock);
}
