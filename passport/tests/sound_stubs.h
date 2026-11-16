#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
enum {
    ESP_OK, ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE,
    ESP_ERR_INVALID_SIZE, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_TIMEOUT,
    ESP_ERR_INVALID_ARG, ESP_ERR_NOT_SUPPORTED, ESP_ERR_NO_MEM,
};
const char *esp_err_to_name(esp_err_t err);
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGD ESP_LOGW
#define ESP_LOGI ESP_LOGW
#define ESP_LOGE ESP_LOGW

enum {
    BSP_I2C_PORT = 0, BSP_I2C_ES8311_ADDR = 0x18, BSP_I2S_PORT = 0,
    BSP_I2S_MCLK = 6, BSP_I2S_BCLK = 5, BSP_I2S_WS = 3, BSP_I2S_DOUT = 2,
    BSP_I2S_PA_CTRL = -1, I2S_GPIO_UNUSED = -1,
};
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7 = 7 };
typedef struct { unsigned dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_bus(void);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *address, size_t address_len,
                                      uint8_t *data, size_t len, int timeout);

typedef void *i2s_chan_handle_t;
enum { I2S_ROLE_MASTER = 1, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_STEREO = 2 };
typedef struct { int port, role, dma_frame_num, dma_desc_num; bool auto_clear_after_cb; } i2s_chan_config_t;
typedef struct {
    struct { unsigned rate; } clk_cfg;
    struct { int bits, mode; } slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) {port, role, 0, 0, false}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {rate}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) {bits, mode}
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx, const i2s_std_config_t *cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx);
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx);
esp_err_t i2s_del_channel(i2s_chan_handle_t tx);
esp_err_t i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t size, size_t *written, uint32_t wait);

typedef unsigned nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

typedef void *TaskHandle_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
enum { pdPASS = 1, pdTRUE = 1, portMAX_DELAY = 10000, tskIDLE_PRIORITY = 0 };
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, int prio, TaskHandle_t *handle);
void xTaskNotifyGive(TaskHandle_t handle);
unsigned ulTaskNotifyTake(bool clear, unsigned delay);
void vTaskDelete(TaskHandle_t handle);
