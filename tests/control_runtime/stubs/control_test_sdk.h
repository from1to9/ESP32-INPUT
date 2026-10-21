// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10a
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
#define CONFIG_LWIP_SNTP_MAX_SERVERS 2
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_open_from_partition(const char *,const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter(void);
void test_leave(void);
#define portENTER_CRITICAL(p) ((void)(p),test_enter())
#define portEXIT_CRITICAL(p) ((void)(p),test_leave())
typedef void *SemaphoreHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
void vTaskDelay(TickType_t);
int64_t esp_timer_get_time(void);
void esp_fill_random(void *,size_t);
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_calloc(size_t,size_t,unsigned);
void *heap_caps_malloc(size_t,unsigned);
void heap_caps_free(void *);
typedef struct {unsigned type,subtype;uint32_t address,size;bool encrypted;} esp_partition_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_PARTITION_SUBTYPE_DATA_PHY 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82
const esp_partition_t *esp_partition_find_first(unsigned,unsigned,const char *);
esp_err_t esp_partition_read(const esp_partition_t *,size_t,void *,size_t);
typedef struct {unsigned marker;} mbedtls_pk_context;
typedef struct {mbedtls_pk_context pk;} mbedtls_x509_crt;
void mbedtls_x509_crt_init(mbedtls_x509_crt *);
void mbedtls_x509_crt_free(mbedtls_x509_crt *);
int mbedtls_x509_crt_parse(mbedtls_x509_crt *,const unsigned char *,size_t);
void mbedtls_pk_init(mbedtls_pk_context *);
void mbedtls_pk_free(mbedtls_pk_context *);
int mbedtls_pk_parse_key(mbedtls_pk_context *,const unsigned char *,size_t,const unsigned char *,size_t);
int mbedtls_pk_check_pair(const mbedtls_pk_context *,const mbedtls_pk_context *);
typedef struct {char project_name[32],version[32];} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
typedef unsigned esp_ota_handle_t;
typedef enum {ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID} esp_ota_img_states_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeU
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_begin(const esp_partition_t *,size_t,esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t,const void *,size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *,esp_app_desc_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
void esp_restart(void);
typedef struct esp_http_client *esp_http_client_handle_t;
typedef struct {int event_id;char *header_key,*header_value;void *user_data;} esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_TRANSPORT_OVER_SSL 1
typedef struct {const char *host,*path,*cert_pem,*common_name;int port,transport_type,timeout_ms,buffer_size,buffer_size_tx;bool disable_auto_redirect;esp_err_t (*event_handler)(esp_http_client_event_t *);} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char *,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
typedef int psa_status_t;
typedef unsigned mbedtls_svc_key_id_t;
typedef struct {unsigned type,bits,usage,algorithm;} psa_key_attributes_t;
typedef struct {uint8_t bytes[2048];size_t count;} psa_hash_operation_t;
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ECC_FAMILY_SECP_R1 1U
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(x) (x)
#define PSA_KEY_USAGE_VERIFY_MESSAGE 2U
#define PSA_ALG_SHA_256 3U
#define PSA_ALG_ECDSA(x) (x)
void psa_set_key_type(psa_key_attributes_t *,unsigned);
void psa_set_key_bits(psa_key_attributes_t *,unsigned);
void psa_set_key_usage_flags(psa_key_attributes_t *,unsigned);
void psa_set_key_algorithm(psa_key_attributes_t *,unsigned);
void psa_reset_key_attributes(psa_key_attributes_t *);
psa_status_t psa_crypto_init(void);
psa_status_t psa_import_key(const psa_key_attributes_t *,const uint8_t *,size_t,mbedtls_svc_key_id_t *);
psa_status_t psa_verify_message(mbedtls_svc_key_id_t,unsigned,const uint8_t *,size_t,const uint8_t *,size_t);
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t);
psa_status_t psa_hash_setup(psa_hash_operation_t *,unsigned);
psa_status_t psa_hash_update(psa_hash_operation_t *,const uint8_t *,size_t);
psa_status_t psa_hash_finish(psa_hash_operation_t *,uint8_t *,size_t,size_t *);
psa_status_t psa_hash_abort(psa_hash_operation_t *);
typedef struct {bool smooth_sync,server_from_dhcp,wait_for_sync,start,renew_servers_after_new_IP;void (*sync_cb)(struct timeval *);size_t num_of_servers;const char *servers[2];} esp_sntp_config_t;
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
