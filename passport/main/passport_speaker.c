#include "passport_speaker.h"

#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"

enum { I2C_TIMEOUT_MS = 20, TX_TIMEOUT_MS = 25 };
static i2c_master_dev_handle_t device;
static i2s_chan_handle_t tx;
static bool tx_running, codec_touched, cleanup_failed;
static const audio_codec_if_t *codec;
static esp_err_t io_error;

static void keep_error(esp_err_t *result, esp_err_t error)
{
    if (*result == ESP_OK) *result = error;
}

static bool ctrl_open(const audio_codec_ctrl_if_t *ctrl)
{
    (void)ctrl;
    return device != NULL;
}

static int ctrl_info(const audio_codec_ctrl_if_t *ctrl, audio_codec_ctrl_info_t *info)
{
    (void)ctrl;
    *info = (audio_codec_ctrl_info_t){
        .type = AUDIO_CODEC_CTRL_I2C,
        .i2c = {.addr = BSP_I2C_ES8311_ADDR << 1, .port = BSP_I2C_PORT},
    };
    return ESP_CODEC_DEV_OK;
}

static int ctrl_read(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
{
    (void)ctrl;
    if (reg_len != 1 || data_len != 1) return ESP_CODEC_DEV_INVALID_ARG;
    uint8_t address = reg;
    uint8_t value = 0;
    if (io_error == ESP_OK) {
        io_error = i2c_master_transmit_receive(device, &address, 1, &value, 1, I2C_TIMEOUT_MS);
    }
    *(uint8_t *)data = value;
    return io_error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

static uint8_t speaker_register(uint8_t address, uint8_t value)
{
    // ES8311 rev. 10, register definitions. The pinned codec driver writes ADC
    // power and clock controls even in DAC mode. Keep those blocks inactive on
    // every write, including setup and shutdown, while preserving DAC settings.
    switch (address) {
    case 0x00: return value | 0x02;  // Hold ADC digital reset.
    case 0x01: return value & ~0x0A; // Gate ADC digital and analog clocks.
    case 0x07: return value | 0x10;  // Tri-state ADCDAT.
    case 0x0A: return value | 0x40;  // Mute serial output.
    case 0x0D: return value | 0x30;  // Power down ADC bias and reference.
    case 0x0E: return value | 0x60;  // Power down ADC PGA and modulator.
    case 0x14: return 0;             // Deselect analog and digital mic inputs.
    case 0x17: return 0;             // Zero ADC volume.
    default: return value;
    }
}

static esp_err_t write_register(uint8_t address, uint8_t value)
{
    const uint8_t transfer[] = {address, speaker_register(address, value)};
    return i2c_master_transmit(device, transfer, sizeof(transfer), I2C_TIMEOUT_MS);
}

static int ctrl_write(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
{
    (void)ctrl;
    if (reg_len != 1 || data_len != 1) return ESP_CODEC_DEV_INVALID_ARG;
    // The pinned driver can drop earlier I/O failures. Latch the first failure
    // and stop sending setup writes once the state of the codec is unknown.
    if (io_error == ESP_OK) io_error = write_register(reg, *(const uint8_t *)data);
    return io_error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static const audio_codec_ctrl_if_t control = {
    .is_open = ctrl_open, .get_info = ctrl_info, .read_reg = ctrl_read, .write_reg = ctrl_write,
};

static esp_err_t open_tx(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_PORT, I2S_ROLE_MASTER);
    channel.dma_desc_num = 3;
    channel.dma_frame_num = PASSPORT_SPEAKER_BLOCK;
    channel.auto_clear_after_cb = true;
    esp_err_t result = i2s_new_channel(&channel, &tx, NULL);
    if (result != ESP_OK) return result;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PASSPORT_SPEAKER_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_BCLK, .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    result = i2s_channel_init_std_mode(tx, &standard);
    if (result != ESP_OK) return result;
    result = i2s_channel_enable(tx);
    tx_running = result == ESP_OK;
    return result;
}

esp_err_t passport_speaker_open(void)
{
    if (cleanup_failed) return ESP_ERR_INVALID_STATE;
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) return result;
    i2c_master_bus_handle_t bus = bsp_i2c_bus();
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BSP_I2C_ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    result = i2c_master_bus_add_device(bus, &config, &device);
    if (result != ESP_OK) return result;

    io_error = ESP_OK;
    uint8_t high = 0, low = 0;
    ctrl_read(&control, 0xFD, 1, &high, 1);
    ctrl_read(&control, 0xFE, 1, &low, 1);
    if (io_error != ESP_OK) return io_error;
    if (high != 0x83 || low != 0x11) return ESP_ERR_NOT_FOUND;
    result = open_tx();
    if (result != ESP_OK) return result;

    _Static_assert(BSP_I2S_PA_CTRL == -1, "Recheck speaker PA wiring for a new BSP");
    codec_touched = true;
    codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = &control, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_I2S_PA_CTRL, .use_mclk = true, .no_dac_ref = true,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    });
    if (io_error != ESP_OK) return io_error;
    if (!codec) return ESP_FAIL;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = PASSPORT_SPEAKER_RATE, .bits_per_sample = 16, .channel = 2,
    };
    if (codec->set_fs(codec, &format) != ESP_CODEC_DEV_OK ||
        codec->set_vol(codec, -12.0f) != ESP_CODEC_DEV_OK ||
        codec->enable(codec, true) != ESP_CODEC_DEV_OK) {
        return io_error != ESP_OK ? io_error : ESP_FAIL;
    }
    return io_error;
}

esp_err_t passport_speaker_write(const int16_t *stereo, size_t frames)
{
    size_t sent = 0;
    size_t bytes = frames * 2 * sizeof(*stereo);
    esp_err_t result = i2s_channel_write(tx, stereo, bytes, &sent, TX_TIMEOUT_MS);
    return result == ESP_OK && sent != bytes ? ESP_ERR_INVALID_SIZE : result;
}

esp_err_t passport_speaker_close(void)
{
    if (cleanup_failed) return ESP_ERR_INVALID_STATE;
    esp_err_t release = ESP_OK, silence = ESP_OK;
    bool suspended = false;
    io_error = ESP_OK;
    if (codec) {
        // The last codec reference powers down the part even after partial enable.
        if (audio_codec_delete_codec_if(codec) != ESP_CODEC_DEV_OK) release = ESP_FAIL;
        suspended = release == ESP_OK && io_error == ESP_OK;
        codec = NULL;
    }
    if (codec_touched && !suspended) {
        // Cover partial construction and failed codec shutdown independently.
        keep_error(&silence, write_register(0x32, 0));    // DAC volume.
        keep_error(&silence, write_register(0x12, 0x02)); // DAC power down.
        keep_error(&silence, write_register(0x01, 0));    // Stop codec clocks.
    }
    codec_touched = false;
    if (tx_running) {
        keep_error(&release, i2s_channel_disable(tx));
        tx_running = false;
    }
    if (tx) {
        esp_err_t deleted = i2s_del_channel(tx);
        keep_error(&release, deleted);
        if (deleted == ESP_OK) tx = NULL;
    }
    if (device) {
        esp_err_t removed = i2c_master_bus_rm_device(device);
        keep_error(&release, removed);
        if (removed == ESP_OK) device = NULL;
    }
    cleanup_failed = release != ESP_OK;
    return cleanup_failed ? release : silence;
}
