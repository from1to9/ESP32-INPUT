#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "../main/passport_sound.c"
#include "../main/passport_speaker.c"
#include "codec_ref_mgr.h"

static uint8_t registers[256] = {[0xFD] = 0x83, [0xFE] = 0x11};
static unsigned bus_token, tx_token, worker_token;
static bool added, tx_created, tx_enabled, create_fails, commit_fails, close_fails, short_write, stop_mid_cue;
static bool has_key;
static int fail_register = -1;
static unsigned codec_refs[CODEC_REF_STAGE_NUM], writes, played_frames, waits, commits, tasks;
static uint8_t stored, staged;
static void (*entry)(void *);
static jmp_buf worker_done;

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "error"; }
esp_err_t bsp_i2c_init(void) { return ESP_OK; }
i2c_master_bus_handle_t bsp_i2c_bus(void) { return &bus_token; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *dev)
{
    assert(bus == &bus_token && !added && cfg->device_address == 0x18 && cfg->dev_addr_length == 7);
    added = true;
    *dev = &bus_token;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    assert(added && dev == &bus_token);
    added = false;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout)
{
    assert(added && dev == &bus_token && len == 2 && timeout <= 20);
    ++writes;
    uint8_t reg = data[0], value = data[1];
    if (reg == fail_register || (close_fails && waits && reg == 0x32)) return ESP_ERR_TIMEOUT;
    if (reg == 0x00) assert(value & 0x02);
    if (reg == 0x01) assert(!(value & 0x0A));
    if (reg == 0x07) assert(value & 0x10);
    if (reg == 0x0A) assert(value & 0x40);
    if (reg == 0x0D) assert((value & 0x30) == 0x30);
    if (reg == 0x0E) assert((value & 0x60) == 0x60);
    if (reg == 0x14 || reg == 0x17) assert(value == 0);
    registers[reg] = value;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *address, size_t address_len,
                                      uint8_t *data, size_t len, int timeout)
{
    assert(added && dev == &bus_token && address_len == 1 && len == 1 && timeout <= 20);
    *data = registers[*address];
    return ESP_OK;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *out, i2s_chan_handle_t *in)
{
    assert(!in && out && !tx_created && cfg->dma_desc_num == 3 && cfg->dma_frame_num == 128 && cfg->auto_clear_after_cb);
    tx_created = true;
    *out = &tx_token;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *cfg)
{
    assert(tx_created && channel == &tx_token && cfg->clk_cfg.rate == 16000);
    assert(cfg->gpio_cfg.din == -1 && cfg->gpio_cfg.dout == BSP_I2S_DOUT);
    assert(cfg->slot_cfg.bits == 16 && cfg->slot_cfg.mode == I2S_SLOT_MODE_STEREO);
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    assert(tx_created && channel == &tx_token && !tx_enabled);
    tx_enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel)
{
    assert(tx_created && channel == &tx_token && tx_enabled);
    tx_enabled = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t channel)
{
    assert(tx_created && channel == &tx_token && !tx_enabled);
    tx_created = false;
    return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *data, size_t size, size_t *written, uint32_t wait)
{
    assert(tx_enabled && channel == &tx_token && size == 512 && wait == 25);
    const int16_t *samples = data;
    for (unsigned i = 0; i < size / sizeof(*samples); i += 2) {
        assert(samples[i] == samples[i + 1] && abs(samples[i]) <= 4096);
    }
    played_frames += size / (2 * sizeof(*samples));
    if (stop_mid_cue) assert(passport_sound_set(false) == ESP_OK);
    *written = short_write ? size / 2 : size;
    return ESP_OK;
}

int codec_ref_acquire(const audio_codec_ctrl_info_t *info, codec_ref_stage_t stage)
{
    assert(info->i2c.addr == 0x30);
    return ++codec_refs[stage];
}
int codec_ref_release(const audio_codec_ctrl_info_t *info, codec_ref_stage_t stage)
{
    assert(info->i2c.addr == 0x30 && codec_refs[stage]);
    unsigned left = --codec_refs[stage];
    if (stage == CODEC_REF_STAGE_OPEN && !left) codec_refs[CODEC_REF_STAGE_ENABLE] = 0;
    return left;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "passport_ui"));
    *handle = mode + 1;
    return mode == NVS_READONLY && !has_key ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 1 && !strcmp(key, "sound"));
    *value = stored;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 2 && !strcmp(key, "sound") && value <= 1);
    staged = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 2);
    ++commits;
    if (commit_fails) return ESP_FAIL;
    has_key = true;
    stored = staged;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2); }

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &worker_token; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, int prio, TaskHandle_t *handle)
{
    assert(!entry && !handle && !arg && stack >= 2048 && prio == 1 && !strcmp(name, "passport_sound"));
    ++tasks;
    if (create_fails) return 0;
    entry = fn;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t handle) { assert(handle == &worker_token); }
unsigned ulTaskNotifyTake(bool clear, unsigned delay)
{
    assert(clear && delay == portMAX_DELAY && passport_sound_state().output == PASSPORT_SOUND_READY);
    if (++waits == 1) {
        assert((registers[0x01] & 0x05) == 0x05 && !(registers[0x12] & 0x02));
        assert(registers[0x32] && !(registers[0x31] & 0x60));
        assert(passport_sound_cue(PASSPORT_CUE_SUCCESS));
        assert(!passport_sound_cue(PASSPORT_CUE_ERROR));
    } else {
        assert(waits == 2);
        assert(passport_sound_set(false) == (commit_fails ? ESP_FAIL : ESP_OK));
        assert(!passport_sound_cue(PASSPORT_CUE_CONFIRM));
    }
    return 1;
}
void vTaskDelete(TaskHandle_t handle)
{
    assert(!handle && !added && !tx_created && codec_refs[CODEC_REF_STAGE_OPEN] == 0);
    entry = NULL;
    longjmp(worker_done, 1);
}

static void run_worker(void)
{
    assert(entry && passport_sound_state().output != PASSPORT_SOUND_READY);
    waits = played_frames = 0;
    if (setjmp(worker_done) == 0) entry(NULL);
}

int main(void)
{
    passport_sound_load();
    assert(passport_sound_state().preference_known && !passport_sound_state().enabled);
    assert(!tasks && !writes && !passport_sound_cue(PASSPORT_CUE_CONFIRM));
    assert(passport_sound_set(false) == ESP_OK && !commits);

    has_key = true;
    stored = 7;
    passport_sound_load();
    assert(!passport_sound_state().preference_known && !tasks);
    commit_fails = true;
    assert(passport_sound_set(true) == ESP_FAIL && !tasks);
    commit_fails = false;
    stored = 1;
    passport_sound_load();
    assert(tasks == 1 && passport_sound_state().output == PASSPORT_SOUND_STARTING);
    assert(passport_sound_set(false) == ESP_OK);
    run_worker();
    assert(!writes && passport_sound_state().output == PASSPORT_SOUND_OFF);

    registers[0xFE] = 0;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE && stored == 1 && !writes);
    registers[0xFE] = 0x11;
    fail_register = 0x04;  // The pinned driver can otherwise drop this clock setup failure.
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE && !waits);
    assert(!registers[0x32] && !registers[0x01] && (registers[0x12] & 0x02));

    fail_register = -1;
    create_fails = true;
    assert(passport_sound_set(true) == ESP_OK && !entry);
    assert(passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE);
    create_fails = false;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(played_frames == 1024 && waits == 2 && stored == 0);
    assert(passport_sound_state().output == PASSPORT_SOUND_OFF);

    assert(passport_sound_set(true) == ESP_OK);
    commit_fails = true;
    run_worker();
    assert(stored == 1 && !passport_sound_state().preference_known);
    assert(passport_sound_state().output == PASSPORT_SOUND_OFF);

    commit_fails = false;
    stop_mid_cue = true;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(played_frames == 128 && stored == 0 && passport_sound_state().output == PASSPORT_SOUND_OFF);
    stop_mid_cue = false;

    short_write = true;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(played_frames == 128 && passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE);

    short_write = false;
    close_fails = true;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(stored == 0 && passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE);
    assert(passport_sound_set(false) == ESP_OK && passport_sound_state().output == PASSPORT_SOUND_UNAVAILABLE);
    close_fails = false;
    assert(passport_sound_set(true) == ESP_OK);
    run_worker();
    assert(passport_sound_state().output == PASSPORT_SOUND_OFF);
    puts("PASS: default Off, NVS uncertainty, TX only, ADC power guards with pinned ES8311, cleanup, and bounded cues");
}
