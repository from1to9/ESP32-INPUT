// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_TIMEOUT 0x107
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
void speech_test_enter(void);
void speech_test_exit(void);
#define portENTER_CRITICAL(lock) ((void)(lock), speech_test_enter())
#define portEXIT_CRITICAL(lock) ((void)(lock), speech_test_exit())
int xTaskCreate(void (*)(void *), const char *, uint32_t, void *, unsigned, void *);
void vTaskDelay(TickType_t);
int64_t esp_timer_get_time(void);
void *heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
typedef struct { int unused; } srmodel_list_t;
typedef struct { int unused; } model_iface_data_t;
typedef struct { unsigned generation; } esp_afe_sr_data_t;
typedef enum { ESP_MN_STATE_DETECTING, ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT } esp_mn_state_t;
typedef struct { int num; int command_id[5]; float prob[5]; } esp_mn_results_t;
typedef struct {
    model_iface_data_t *(*create)(const char *, int);
    int (*get_samp_rate)(model_iface_data_t *);
    int (*get_samp_chunksize)(model_iface_data_t *);
    void (*clean)(model_iface_data_t *);
    void (*destroy)(model_iface_data_t *);
    esp_mn_state_t (*detect)(model_iface_data_t *, int16_t *);
    esp_mn_results_t *(*get_results)(model_iface_data_t *);
} esp_mn_iface_t;
typedef struct { int ret_value, data_size; int16_t *data; } afe_fetch_result_t;
typedef struct {
    bool wakenet_init, aec_init, se_init, ns_init, vad_init;
    char *ns_model_name, *vad_model_name;
    int afe_ns_mode, afe_ringbuf_size;
} afe_config_t;
#define AFE_TYPE_SR 0
#define AFE_MODE_LOW_COST 0
#define AFE_NS_MODE_WEBRTC 0
typedef struct {
    esp_afe_sr_data_t *(*create_from_config)(afe_config_t *);
    int (*get_feed_chunksize)(esp_afe_sr_data_t *);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t *);
    int (*get_feed_channel_num)(esp_afe_sr_data_t *);
    int (*get_samp_rate)(esp_afe_sr_data_t *);
    int (*feed)(esp_afe_sr_data_t *, const int16_t *);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *, TickType_t);
    void (*destroy)(esp_afe_sr_data_t *);
} esp_afe_sr_iface_t;
#define ESP_MN_PREFIX "mn"
srmodel_list_t *esp_srmodel_init(const char *);
void esp_srmodel_deinit(srmodel_list_t *);
char *esp_srmodel_filter(srmodel_list_t *, const char *, const char *);
esp_mn_iface_t *esp_mn_handle_from_name(const char *);
esp_err_t esp_mn_commands_alloc(const esp_mn_iface_t *, model_iface_data_t *);
esp_err_t esp_mn_commands_free(void);
void esp_mn_commands_clear(void);
esp_err_t esp_mn_commands_add(int, const char *);
void *esp_mn_commands_update(void);
afe_config_t *afe_config_init(const char *, srmodel_list_t *, int, int);
void afe_config_free(afe_config_t *);
const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *);
