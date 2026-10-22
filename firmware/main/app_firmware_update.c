// SPDX-License-Identifier: Apache-2.0
#include "app_firmware_update.h"
#include "app_clock.h"
#include "control_json.h"
#include "device_settings.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define UPDATE_BUFFER 4096U
#define UPDATE_CA_BYTES 2048U
#define UPDATE_SOURCE_BYTES 512U
#define UPDATE_JOB_MS 600000U
#define UPDATE_PROBATION_MS 60000U
#define UPDATE_MINIMUM_HEALTH_MS 45000U
typedef struct {
    char host[129], path[384];
    uint16_t port;
} source_t;
typedef struct {
    char release_id[65], version[32], digest[65], url[UPDATE_SOURCE_BYTES];
    uint32_t release_index, size;
    uint64_t expires;
} release_t;
typedef enum { WORK_NONE, WORK_CHECK, WORK_DOWNLOAD, WORK_ACTIVATE, WORK_DISCARD } work_kind_t;
typedef struct {
    char catalog[UPDATE_SOURCE_BYTES], ca[UPDATE_CA_BYTES + 1], key_id[65], product[65], board[65],
        layout[65];
    uint8_t public_key[65];
    bool allow_private, configured, cancel, health_requested, invalid_encoding, configuring;
    bool probation, probation_terminal, health_attempted;
    esp_err_t configuration_error;
    uint32_t minimum_index, floor;
    app_update_snapshot_t view;
    app_update_authority_t authority;
    work_kind_t work, active_work;
    release_t release;
    const esp_partition_t *staged;
    uint64_t started, boot_ms;
} update_t;
static update_t *updater;
static portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}
static void publish_locked(app_update_state_t state, esp_err_t error, const char *reason)
{
    updater->view.state = state;
    updater->view.error = error;
    updater->view.cleanup_pending = false;
    snprintf(updater->view.reason, sizeof(updater->view.reason), "%s", reason ? reason : "");
    ++updater->view.revision;
}
static void publish(app_update_state_t state, esp_err_t error, const char *reason)
{
    portENTER_CRITICAL(&update_lock);
    publish_locked(state, error, reason);
    portEXIT_CRITICAL(&update_lock);
}
static bool decode(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[2 * i + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
    return true;
}
static bool text_field(const cj_document_t *d, const char *key, char *out, size_t capacity)
{
    const char *s = cj_string(d, cj_get(d, 0, key));
    if (!s || !*s || strlen(s) >= capacity) {
        return false;
    }
    memcpy(out, s, strlen(s) + 1);
    return true;
}
static bool exact_keys(const cj_document_t *d, const char *const *keys, size_t count)
{
    if (d->nodes[0].type != CJ_OBJECT) {
        return false;
    }
    for (uint16_t t = d->nodes[0].child; t != CJ_NONE; t = d->nodes[t].next) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (!strcmp(d->text + d->nodes[t].key, keys[i])) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
static bool source_parse(const char *url, source_t *out)
{
    if (!url || strlen(url) >= UPDATE_SOURCE_BYTES || strncmp(url, "https://", 8)) {
        return false;
    }
    const char *host = url + 8, *slash = strchr(host, '/');
    if (!slash || slash == host || strchr(url, '@') || strchr(url, '?') || strchr(url, '#') ||
        strchr(url, '\\')) {
        return false;
    }
    const char *colon = memchr(host, ':', (size_t)(slash - host));
    size_t n = (size_t)((colon ? colon : slash) - host);
    if (!n || n >= sizeof(out->host) || strlen(slash) >= sizeof(out->path)) {
        return false;
    }
    memcpy(out->host, host, n);
    out->host[n] = 0;
    out->port = 443;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)out->host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.')) {
            return false;
        }
    }
    if (colon) {
        char *end = NULL;
        unsigned long port = strtoul(colon + 1, &end, 10);
        if (end != slash || port < 1 || port > 65535) {
            return false;
        }
        out->port = (uint16_t)port;
    }
    strcpy(out->path, slash);
    for (const unsigned char *p = (const unsigned char *)slash; *p; ++p) {
        if (*p < 33 || *p > 126) {
            return false;
        }
    }
    return true;
}
static bool allowed_ip(uint32_t network_order, bool allow_private)
{
    uint32_t ip = ntohl(network_order);
    unsigned first = ip >> 24, second = (ip >> 16) & 255;
    if (!first || first == 127 || first >= 224 || (first == 169 && second == 254)) {
        return false;
    }
    bool private_ip = first == 10 || (first == 172 && second >= 16 && second <= 31) ||
                      (first == 192 && second == 168) ||
                      (first == 100 && second >= 64 && second <= 127) ||
                      (first == 198 && (second == 18 || second == 19));
    return allow_private || !private_ip;
}
static esp_err_t source_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Content-Encoding") &&
        strcasecmp(event->header_value, "identity")) {
        updater->invalid_encoding = true;
    }
    return ESP_OK;
}
static esp_http_client_handle_t open_source(const char *url)
{
    source_t source = {0}, approved = {0};
    if (!source_parse(url, &source) || !source_parse(updater->catalog, &approved) ||
        strcmp(source.host, approved.host) || source.port != approved.port) {
        return NULL;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *answers = NULL;
    if (getaddrinfo(source.host, NULL, &hints, &answers) != 0) {
        return NULL;
    }
    char address[INET_ADDRSTRLEN] = {0};
    bool valid = answers != NULL;
    for (struct addrinfo *a = answers; a; a = a->ai_next) {
        const struct sockaddr_in *ip = (const struct sockaddr_in *)a->ai_addr;
        if (!allowed_ip(ip->sin_addr.s_addr, updater->allow_private)) {
            valid = false;
            break;
        }
        if (!address[0]) {
            inet_ntop(AF_INET, &ip->sin_addr, address, sizeof(address));
        }
    }
    freeaddrinfo(answers);
    if (!valid || !address[0]) {
        return NULL;
    }
    /* Connect to the validated address, retaining approved DNS name for SNI,
     * certificate verification and HTTP Host. No second DNS lookup or redirect. */
    updater->invalid_encoding = false;
    esp_http_client_config_t config = {.event_handler = source_event,
                                       .host = address,
                                       .port = source.port,
                                       .path = source.path,
                                       .transport_type = HTTP_TRANSPORT_OVER_SSL,
                                       .cert_pem = updater->ca,
                                       .common_name = source.host,
                                       .timeout_ms = 2000,
                                       .disable_auto_redirect = true,
                                       .buffer_size = UPDATE_BUFFER,
                                       .buffer_size_tx = 1024};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }
    char host[140];
    if (source.port == 443) {
        snprintf(host, sizeof(host), "%s", source.host);
    } else {
        snprintf(host, sizeof(host), "%s:%u", source.host, source.port);
    }
    if (esp_http_client_set_header(client, "Host", host) != ESP_OK ||
        esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK ||
        esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) != 200) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    if (updater->invalid_encoding) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}
static bool authorized(bool activation)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&update_lock);
    app_update_authority_t a = updater->authority;
    bool cancel = updater->cancel;
    uint64_t started = updater->started;
    portEXIT_CRITICAL(&update_lock);
    return !cancel && a.authorized && a.unlocked && a.maintenance_ready &&
           now < a.authority_expires_ms && now >= started && now - started < UPDATE_JOB_MS &&
           a.policy_revision == updater->view.policy_revision &&
           (!activation || a.restart_authorized) &&
           !strcmp(a.expected_digest, updater->release.digest);
}
static bool layout_ready(void)
{
    const esp_partition_t *a =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const esp_partition_t *b =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
    const esp_partition_t *control = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "control");
    const esp_partition_t *records =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "recordings");
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    const esp_partition_t *model = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "model");
    const esp_partition_t *nvs =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    const esp_partition_t *phy = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, "phy_init");
    const esp_partition_t *running = esp_ota_get_running_partition();
    return a && b && !factory && control && records && ota && model && nvs && phy &&
           (running == a || running == b) && a->address == 0x10000 && b->address == 0x610000 &&
           a->size == APP_UPDATE_SLOT_BYTES && b->size == APP_UPDATE_SLOT_BYTES &&
           ota->address == 0xc10000 && ota->size == 0x2000 && model->address == 0xc20000 &&
           model->size == 0x300000 && nvs->address == 0x9000 && nvs->size == 0x6000 &&
           phy->address == 0xf000 && phy->size == 0x1000 && control->address == 0xf20000 &&
           control->size == 0x10000 && records->address == 0xf30000 && records->size == 0xd0000;
}
static bool configuration_ready(void)
{
    bool rollback = false;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    rollback = true;
#endif
    return updater->configured && updater->configuration_error == ESP_OK && rollback && layout_ready();
}
/* Worker/configuration task only. Never turn an unavailable observation into
 * permission to overwrite a possible rollback slot. */
static esp_err_t running_state_read(esp_ota_img_states_t *state)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t error = running ? esp_ota_get_state_partition(running, state) : ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&update_lock);
    updater->view.running_state_known = error == ESP_OK;
    updater->view.running_state_error = error;
    updater->view.running_confirmed = error == ESP_OK && *state == ESP_OTA_IMG_VALID;
    if (error == ESP_OK && *state == ESP_OTA_IMG_PENDING_VERIFY && !updater->probation_terminal) updater->probation = true;
    if (updater->view.running_confirmed) updater->probation = false;
    updater->view.probation_active = updater->probation && !updater->probation_terminal;
    if (!updater->view.running_confirmed) updater->view.available = false;
    portEXIT_CRITICAL(&update_lock);
    return error;
}
static esp_err_t require_confirmed_running_image(void)
{
    esp_ota_img_states_t state = ESP_OTA_IMG_NEW;
    esp_err_t error = running_state_read(&state);
    if (error == ESP_OK && state == ESP_OTA_IMG_VALID) return ESP_OK;
    if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
    publish(APP_UPDATE_UNAVAILABLE, error, "running_image_not_confirmed");
    return error;
}
static esp_err_t floor_read(uint32_t *floor)
{
    *floor = 0;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open_from_partition("control", "ctrl_ota", NVS_READONLY, &nvs);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (e != ESP_OK) {
        return e;
    }
    uint8_t bytes[8] = {0};
    size_t n = sizeof(bytes);
    e = nvs_get_blob(nvs, "floor", bytes, &n);
    nvs_close(nvs);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (e != ESP_OK) {
        return e;
    }
    if (n != 8 || memcmp(bytes, "OTF1", 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    *floor = ((uint32_t)bytes[4] << 24) | ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 8) |
             bytes[7];
    return ESP_OK;
}
static esp_err_t floor_write(uint32_t floor)
{
    uint8_t bytes[8] = {'O',
                        'T',
                        'F',
                        '1',
                        (uint8_t)(floor >> 24),
                        (uint8_t)(floor >> 16),
                        (uint8_t)(floor >> 8),
                        (uint8_t)floor};
    nvs_handle_t nvs;
    esp_err_t e = nvs_open_from_partition("control", "ctrl_ota", NVS_READWRITE, &nvs);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_set_blob(nvs, "floor", bytes, sizeof(bytes));
    if (e == ESP_OK) {
        e = nvs_commit(nvs);
    }
    nvs_close(nvs);
    uint32_t read = 0;
    if (e == ESP_OK) {
        e = floor_read(&read);
    }
    if (e == ESP_OK && read != floor) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (e == ESP_OK) {
        updater->floor = floor;
    }
    return e;
}
static esp_err_t verify_catalog(const char *json, size_t length, release_t *out)
{
    static const char *const envelope_keys[] = {"v", "alg", "key_id", "payload", "signature"};
    static const char *const payload_keys[] = {"v",
                                               "release_id",
                                               "release_index",
                                               "product",
                                               "board",
                                               "chip",
                                               "project",
                                               "layout",
                                               "settings_schema",
                                               "version",
                                               "size",
                                               "sha256",
                                               "url",
                                               "expires_unix_seconds"};
    cj_document_t *d =
        heap_caps_aligned_calloc(_Alignof(cj_document_t), 1, sizeof(*d), MALLOC_CAP_8BIT);
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    uint8_t signature[64];
    char payload[3073] = {0}, algorithm[16], key_id[65];
    int64_t version;
    bool valid = cj_parse(d, json, length) && exact_keys(d, envelope_keys, 5) &&
                 cj_integer(d, cj_get(d, 0, "v"), &version) && version == 1 &&
                 text_field(d, "alg", algorithm, sizeof(algorithm)) &&
                 !strcmp(algorithm, "ES256") && text_field(d, "key_id", key_id, sizeof(key_id)) &&
                 !strcmp(key_id, updater->key_id) &&
                 text_field(d, "payload", payload, sizeof(payload)) &&
                 decode(cj_string(d, cj_get(d, 0, "signature")), signature, 64);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key = 0;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    if (valid) {
        valid = psa_crypto_init() == PSA_SUCCESS &&
                psa_import_key(&attributes, updater->public_key, 65, &key) == PSA_SUCCESS &&
                psa_verify_message(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), (const uint8_t *)payload,
                                   strlen(payload), signature, 64) == PSA_SUCCESS;
    }
    if (key) {
        psa_destroy_key(key);
    }
    psa_reset_key_attributes(&attributes);
    uint64_t expiry = 0;
    int64_t index = 0, size = 0, schema = 0;
    char product[65], board[65], chip[32], project[33], layout[65];
    uint8_t image_hash[32];
    if (valid) {
        valid = cj_parse(d, payload, strlen(payload)) && exact_keys(d, payload_keys, 14) &&
                cj_integer(d, cj_get(d, 0, "v"), &version) && version == 1 &&
                text_field(d, "release_id", out->release_id, sizeof(out->release_id)) &&
                text_field(d, "version", out->version, sizeof(out->version)) &&
                text_field(d, "product", product, sizeof(product)) &&
                !strcmp(product, updater->product) &&
                text_field(d, "board", board, sizeof(board)) && !strcmp(board, updater->board) &&
                text_field(d, "chip", chip, sizeof(chip)) && !strcmp(chip, "esp32s31") &&
                text_field(d, "project", project, sizeof(project)) &&
                !strcmp(project, esp_app_get_description()->project_name) &&
                text_field(d, "layout", layout, sizeof(layout)) &&
                !strcmp(layout, APP_UPDATE_LAYOUT) && !strcmp(layout, updater->layout) &&
                cj_integer(d, cj_get(d, 0, "settings_schema"), &schema) &&
                schema == DEVICE_SETTINGS_SCHEMA_VERSION &&
                cj_integer(d, cj_get(d, 0, "release_index"), &index) &&
                index >= updater->minimum_index && index >= updater->floor && index <= UINT32_MAX &&
                cj_integer(d, cj_get(d, 0, "size"), &size) && size > 0 &&
                size <= APP_UPDATE_SLOT_BYTES &&
                text_field(d, "sha256", out->digest, sizeof(out->digest)) &&
                decode(out->digest, image_hash, 32) &&
                text_field(d, "url", out->url, sizeof(out->url)) &&
                cj_u64(d, cj_get(d, 0, "expires_unix_seconds"), &expiry);
    }
    uint64_t wall;
    source_t source = {0}, catalog = {0};
    if (valid) {
        valid = app_clock_wall_time(&wall) && expiry > wall && expiry - wall <= 604800 &&
                source_parse(out->url, &source) && source_parse(updater->catalog, &catalog) &&
                !strcmp(source.host, catalog.host) && source.port == catalog.port;
    }
    if (valid) {
        for (const unsigned char *p = (const unsigned char *)out->release_id; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
                valid = false;
            }
        }
    }
    if (valid) {
        out->release_index = (uint32_t)index;
        out->size = (uint32_t)size;
        out->expires = expiry;
        result = ESP_OK;
    }
    wipe(d, sizeof(*d));
    heap_caps_free(d);
    wipe(payload, sizeof(payload));
    return result;
}
static esp_err_t check_catalog(void)
{
    publish(APP_UPDATE_CHECKING, ESP_OK, "checking_signed_catalog");
    esp_http_client_handle_t client = open_source(updater->catalog);
    if (!client) {
        return ESP_FAIL;
    }
    char *body = heap_caps_calloc(1, CJ_CAPACITY + 1, MALLOC_CAP_8BIT);
    if (!body) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    esp_err_t error = ESP_OK;
    uint64_t until = now_ms() + 15000;
    while (used < CJ_CAPACITY && now_ms() < until) {
        int n = esp_http_client_read(client, body + used, (int)(CJ_CAPACITY - used));
        if (n < 0) {
            error = ESP_FAIL;
            break;
        }
        if (!n) {
            break;
        }
        used += (size_t)n;
    }
    if (used == CJ_CAPACITY || now_ms() >= until || updater->cancel ||
        !esp_http_client_is_complete_data_received(client)) {
        error = ESP_ERR_INVALID_SIZE;
    }
    release_t release = {0};
    if (error == ESP_OK) {
        error = verify_catalog(body, used, &release);
    }
    esp_http_client_cleanup(client);
    wipe(body, CJ_CAPACITY + 1);
    heap_caps_free(body);
    if (error == ESP_OK) {
        updater->release = release;
        portENTER_CRITICAL(&update_lock);
        strcpy(updater->view.release_id, release.release_id);
        strcpy(updater->view.digest, release.digest);
        updater->view.total_bytes = release.size;
        portEXIT_CRITICAL(&update_lock);
        publish(APP_UPDATE_AVAILABLE, ESP_OK, "verified_signed_release");
    }
    return error;
}
static esp_err_t partition_hash(const esp_partition_t *p, uint32_t length, uint8_t out[32])
{
    uint8_t *buffer = heap_caps_malloc(UPDATE_BUFFER, MALLOC_CAP_8BIT);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    esp_err_t error = ESP_OK;
    for (uint32_t offset = 0; offset < length && status == PSA_SUCCESS;) {
        size_t n = length - offset;
        if (n > UPDATE_BUFFER) {
            n = UPDATE_BUFFER;
        }
        error = esp_partition_read(p, offset, buffer, n);
        if (error != ESP_OK) {
            break;
        }
        status = psa_hash_update(&hash, buffer, n);
        offset += (uint32_t)n;
    }
    size_t n = 0;
    if (error == ESP_OK && status == PSA_SUCCESS) {
        status = psa_hash_finish(&hash, out, 32, &n);
    }
    psa_hash_abort(&hash);
    wipe(buffer, UPDATE_BUFFER);
    heap_caps_free(buffer);
    return error != ESP_OK ? error : status == PSA_SUCCESS && n == 32 ? ESP_OK : ESP_FAIL;
}
static esp_err_t download_image(void)
{
    esp_err_t running_error = require_confirmed_running_image();
    if (running_error != ESP_OK) return running_error;
    uint64_t wall;
    if (!app_clock_wall_time(&wall) || wall >= updater->release.expires) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *running = esp_ota_get_running_partition(),
                          *boot = esp_ota_get_boot_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!layout_ready() || !target || target == running || target == boot ||
        target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        updater->release.size > target->size) {
        return ESP_ERR_INVALID_STATE;
    }
    publish(APP_UPDATE_WAITING_MAINTENANCE, ESP_OK, "waiting_for_actual_maintenance");
    uint64_t until = now_ms() + 5000;
    while (!authorized(false) && now_ms() < until) {
        portENTER_CRITICAL(&update_lock);
        bool cancel = updater->cancel;
        portEXIT_CRITICAL(&update_lock);
        if (cancel) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!authorized(false)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_handle_t client = open_source(updater->release.url);
    if (!client) {
        return ESP_FAIL;
    }
    int64_t content = esp_http_client_get_content_length(client);
    if (content != (int64_t)updater->release.size) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buffer = heap_caps_malloc(UPDATE_BUFFER, MALLOC_CAP_8BIT);
    if (!buffer) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    esp_ota_handle_t ota = 0;
    esp_err_t error = authorized(false) ? require_confirmed_running_image() : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) error = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    uint32_t received = 0;
    if (error == ESP_OK) {
        publish(APP_UPDATE_DOWNLOADING, ESP_OK, "streaming_inactive_slot");
    }
    while (error == ESP_OK && received < updater->release.size) {
        if (!authorized(false)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t remaining = updater->release.size - received;
        if (remaining > UPDATE_BUFFER) {
            remaining = UPDATE_BUFFER;
        }
        int n = esp_http_client_read(client, (char *)buffer, (int)remaining);
        if (n <= 0) {
            error = ESP_FAIL;
            break;
        }
        received += (uint32_t)n;
        portENTER_CRITICAL(&update_lock);
        updater->view.received_bytes = received;
        portEXIT_CRITICAL(&update_lock);
        if (!authorized(false)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        status = psa_hash_update(&hash, buffer, (size_t)n);
        if (status != PSA_SUCCESS) {
            error = ESP_FAIL;
            break;
        }
        error = esp_ota_write(ota, buffer, (size_t)n);
        portENTER_CRITICAL(&update_lock);
        if (error == ESP_OK) {
            updater->view.written_bytes = received;
        }
        portEXIT_CRITICAL(&update_lock);
    }
    uint8_t digest[32], expected[32];
    size_t hash_size = 0;
    if (error == ESP_OK &&
        (received != updater->release.size || !esp_http_client_is_complete_data_received(client) ||
         !decode(updater->release.digest, expected, 32) ||
         psa_hash_finish(&hash, digest, sizeof(digest), &hash_size) != PSA_SUCCESS ||
         hash_size != 32 || memcmp(digest, expected, 32))) {
        error = ESP_ERR_INVALID_CRC;
    }
    psa_hash_abort(&hash);
    esp_http_client_cleanup(client);
    wipe(buffer, UPDATE_BUFFER);
    heap_caps_free(buffer);
    if (error == ESP_OK && authorized(false)) {
        publish(APP_UPDATE_VERIFYING, ESP_OK, "verifying_staged_image");
        error = esp_ota_end(ota);
        ota = 0;
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (ota) {
        (void)esp_ota_abort(ota);
    }
    if (error == ESP_OK) {
        error = partition_hash(target, updater->release.size, digest);
    }
    if (error == ESP_OK && memcmp(digest, expected, 32)) {
        error = ESP_ERR_INVALID_CRC;
    }
    esp_app_desc_t description;
    if (error == ESP_OK) {
        error = esp_ota_get_partition_description(target, &description);
    }
    if (error == ESP_OK &&
        (strcmp(description.project_name, esp_app_get_description()->project_name) ||
         strcmp(description.version, updater->release.version))) {
        error = ESP_ERR_INVALID_VERSION;
    }
    if (error == ESP_OK && !authorized(false)) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        updater->staged = target;
        publish(APP_UPDATE_STAGED, ESP_OK, "verified_not_selected_for_boot");
    }
    return error;
}
static esp_err_t activate(void)
{
    esp_err_t running_error = require_confirmed_running_image();
    if (running_error != ESP_OK) return running_error;
    if (!updater->staged || !authorized(true) || !layout_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t digest[32], expected[32];
    esp_err_t e = partition_hash(updater->staged, updater->release.size, digest);
    if (e != ESP_OK || !decode(updater->release.digest, expected, 32) ||
        memcmp(digest, expected, 32) || !authorized(true)) {
        return ESP_ERR_INVALID_CRC;
    }
    portENTER_CRITICAL(&update_lock);
    if (updater->cancel) { portEXIT_CRITICAL(&update_lock); return ESP_ERR_INVALID_STATE; }
    publish_locked(APP_UPDATE_SELECTING_BOOT, ESP_OK, "committing_boot_selection");
    portEXIT_CRITICAL(&update_lock);
    e = floor_write(updater->release.release_index);
    if (e != ESP_OK) {
        publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, e, "release_floor_uncertain_no_boot_write");
        return e;
    }
    if (!authorized(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    e = esp_ota_set_boot_partition(updater->staged);
    const esp_partition_t *selected = esp_ota_get_boot_partition();
    if (e != ESP_OK || selected != updater->staged) {
        publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, e != ESP_OK ? e : ESP_ERR_INVALID_STATE,
                "boot_selection_requires_reconciliation");
        return e != ESP_OK ? e : ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&update_lock);
    updater->view.boot_selected = true;
    portEXIT_CRITICAL(&update_lock);
    publish(APP_UPDATE_RESTART_PENDING, ESP_OK, "boot_selected_restart_pending");
    vTaskDelay(pdMS_TO_TICKS(500));
    /* No command is replayed after restart. SDK rollback owns unconfirmed
     * images; the application independently proves its local health gates. */
    esp_restart();
    return ESP_ERR_INVALID_STATE;
}
static void running_confirmed(const char *reason)
{
    bool available = configuration_ready();
    portENTER_CRITICAL(&update_lock);
    updater->view.available = available;
    publish_locked(APP_UPDATE_CONFIRMED, available ? ESP_OK : updater->configuration_error != ESP_OK ?
        updater->configuration_error : ESP_ERR_NOT_SUPPORTED,
        available ? reason : "health_confirmed_ota_configuration_unavailable");
    portEXIT_CRITICAL(&update_lock);
}
static void service_probation(void)
{
    portENTER_CRITICAL(&update_lock);
    bool active = updater->probation && !updater->probation_terminal;
    portEXIT_CRITICAL(&update_lock);
    if (!active) return;
    uint64_t now = now_ms();
    if (now < updater->boot_ms) {
        portENTER_CRITICAL(&update_lock);
        updater->probation_terminal = true; updater->view.probation_active = false;
        portEXIT_CRITICAL(&update_lock);
        publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, ESP_ERR_INVALID_STATE, "probation_clock_regressed");
        return;
    }
    /* This deadline outranks a failed confirmation. Read actual metadata before
     * invalidating an image: a failed mark-valid may already have committed. */
    if (now - updater->boot_ms >= UPDATE_PROBATION_MS) {
        portENTER_CRITICAL(&update_lock);
        updater->probation_terminal = true; updater->view.probation_active = false;
        portEXIT_CRITICAL(&update_lock);
        esp_ota_img_states_t state = ESP_OTA_IMG_NEW;
        esp_err_t error = running_state_read(&state);
        if (error == ESP_OK && state == ESP_OTA_IMG_VALID) {
            running_confirmed("valid_image_observed_at_probation_deadline");
        } else if (error == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
            publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, ESP_OK, "probation_expired_rollback_requested");
            error = esp_ota_mark_app_invalid_rollback_and_reboot();
            (void)running_state_read(&state);
            publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, error == ESP_OK ? ESP_ERR_INVALID_STATE : error,
                "rollback_returned_requires_recovery");
        } else {
            publish(APP_UPDATE_BOOT_SELECTION_UNCERTAIN, error == ESP_OK ? ESP_ERR_INVALID_STATE : error,
                "probation_expired_state_unknown_or_not_pending");
        }
        return;
    }
    portENTER_CRITICAL(&update_lock);
    bool confirm = updater->health_requested && !updater->health_attempted &&
        now - updater->boot_ms >= UPDATE_MINIMUM_HEALTH_MS;
    if (confirm) {
        updater->health_attempted = true; updater->health_requested = false;
        updater->view.health_confirmation_attempted = true;
    }
    portEXIT_CRITICAL(&update_lock);
    if (!confirm) return;
    esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    portENTER_CRITICAL(&update_lock);
    updater->view.health_confirmation_error = result;
    portEXIT_CRITICAL(&update_lock);
    esp_ota_img_states_t state = ESP_OTA_IMG_NEW;
    esp_err_t observed = running_state_read(&state);
    if (observed == ESP_OK && state == ESP_OTA_IMG_VALID) {
        running_confirmed(result == ESP_OK ? "local_health_confirmed" : "valid_image_observed_after_confirmation_error");
    } else {
        publish(APP_UPDATE_PENDING_HEALTH, observed != ESP_OK ? observed : result != ESP_OK ? result : ESP_ERR_INVALID_STATE,
            observed != ESP_OK ? "health_confirmation_state_unknown" : "health_confirmation_not_observed");
    }
}
static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        portENTER_CRITICAL(&update_lock);
        work_kind_t work = updater->work;
        updater->work = WORK_NONE;
        updater->active_work = work;
        portEXIT_CRITICAL(&update_lock);
        service_probation();
        esp_err_t error = ESP_OK;
        if (work == WORK_CHECK) error = check_catalog();
        else if (work == WORK_DOWNLOAD) error = download_image();
        else if (work == WORK_ACTIVATE) error = activate();
        else if (work == WORK_DISCARD) {
            updater->staged = NULL;
            publish(APP_UPDATE_IDLE, ESP_OK, "staged_reference_discarded");
        }
        portENTER_CRITICAL(&update_lock);
        updater->active_work = WORK_NONE;
        bool uncertain = updater->view.state == APP_UPDATE_BOOT_SELECTION_UNCERTAIN;
        if (work != WORK_NONE && updater->cancel && !uncertain && !updater->view.boot_selected &&
            updater->view.state != APP_UPDATE_SELECTING_BOOT) {
            updater->staged = NULL;
            wipe(&updater->authority, sizeof(updater->authority));
            publish_locked(APP_UPDATE_CANCELLED, error, "cancelled_owner_drained_stage_revoked");
        } else if (error != ESP_OK && !uncertain && updater->view.state != APP_UPDATE_UNAVAILABLE) {
            publish_locked(APP_UPDATE_ERROR, error, "update_failed_no_success_claim");
        }
        portEXIT_CRITICAL(&update_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_update_configure(const app_update_config_t *config)
{
    if (!updater) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&update_lock);
    bool busy = updater->work != WORK_NONE || updater->active_work != WORK_NONE || updater->view.state == APP_UPDATE_STAGED ||
                updater->view.state == APP_UPDATE_BOOT_SELECTION_UNCERTAIN ||
                updater->view.state == APP_UPDATE_CHECKING ||
                updater->view.state == APP_UPDATE_PENDING_HEALTH ||
                updater->view.state == APP_UPDATE_DOWNLOADING ||
                updater->view.state == APP_UPDATE_VERIFYING ||
                updater->view.state == APP_UPDATE_WAITING_MAINTENANCE ||
                updater->view.state == APP_UPDATE_SELECTING_BOOT || updater->view.boot_selected;
    if (!busy) {
        updater->configuring = true;
    }
    portEXIT_CRITICAL(&update_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ESP_OK;
    updater->configuration_error = ESP_ERR_INVALID_STATE;
    updater->configured = false;
    if (config) {
        if (config->abi_version != APP_UPDATE_ABI || !config->catalog_url || !config->tls_ca_pem ||
            !config->signing_key_id || !config->signing_public_key || !config->product ||
            !config->board || !config->layout ||
            strlen(config->catalog_url) >= sizeof(updater->catalog) ||
            strlen(config->tls_ca_pem) >= sizeof(updater->ca) ||
            strlen(config->signing_key_id) >= sizeof(updater->key_id) ||
            strlen(config->product) >= sizeof(updater->product) ||
            strlen(config->board) >= sizeof(updater->board) ||
            strcmp(config->layout, APP_UPDATE_LAYOUT)) {
            error = ESP_ERR_INVALID_ARG;
        }
        if (error == ESP_OK) {
            strcpy(updater->catalog, config->catalog_url);
            strcpy(updater->ca, config->tls_ca_pem);
            strcpy(updater->key_id, config->signing_key_id);
            strcpy(updater->product, config->product);
            strcpy(updater->board, config->board);
            strcpy(updater->layout, config->layout);
            memcpy(updater->public_key, config->signing_public_key, 65);
            updater->allow_private = config->allow_private_source;
            updater->minimum_index = config->minimum_release_index;
            source_t source;
            updater->configured =
                source_parse(updater->catalog, &source) && updater->public_key[0] == 4;
            if (!updater->configured) {
                error = ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (error == ESP_OK && config) {
        error = floor_read(&updater->floor);
    }
    updater->configuration_error = error != ESP_OK ? error : updater->configured ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    bool compatible = configuration_ready();
    esp_ota_img_states_t boot_state = ESP_OTA_IMG_NEW;
    esp_err_t running_error = running_state_read(&boot_state);
    bool probation = running_error == ESP_OK && boot_state == ESP_OTA_IMG_PENDING_VERIFY;
    portENTER_CRITICAL(&update_lock);
    updater->view.trust_revision = config ? config->trust_revision : 0;
    updater->view.available = error == ESP_OK && compatible && updater->view.running_confirmed;
    updater->view.state = probation ? APP_UPDATE_PENDING_HEALTH : updater->view.available ? APP_UPDATE_IDLE : APP_UPDATE_UNAVAILABLE;
    updater->view.running_expected = probation;
    updater->view.error = probation ? updater->configuration_error : running_error != ESP_OK ? running_error :
        error == ESP_OK && !updater->view.available ? ESP_ERR_NOT_SUPPORTED : error;
    strcpy(updater->view.reason, probation ? "awaiting_local_health_gates" :
        running_error != ESP_OK ? "running_image_state_unknown" : !updater->view.running_confirmed ? "running_image_not_confirmed" :
        updater->view.available ? "configured" : "trust_layout_or_rollback_unavailable");
    updater->configuring = false;
    bool available = updater->view.available;
    ++updater->view.revision;
    portEXIT_CRITICAL(&update_lock);
    return available ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_update_init(const app_update_config_t *config)
{
    if (updater) {
        return ESP_ERR_INVALID_STATE;
    }
    updater = heap_caps_aligned_calloc(_Alignof(update_t), 1, sizeof(*updater),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!updater) {
        updater = heap_caps_aligned_calloc(_Alignof(update_t), 1, sizeof(*updater), MALLOC_CAP_8BIT);
    }
    if (!updater) {
        return ESP_ERR_NO_MEM;
    }
    updater->boot_ms = now_ms();
    updater->view.state = APP_UPDATE_UNAVAILABLE;
    updater->view.error = ESP_ERR_INVALID_STATE;
    (void)app_update_configure(config);
    if (xTaskCreate(worker, "firmware_update", 16384, NULL, 1, NULL) != pdPASS) {
        wipe(updater, sizeof(*updater));
        heap_caps_free(updater);
        updater = NULL;
        return ESP_ERR_NO_MEM;
    }
    return updater->view.available ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t admit(work_kind_t work, const app_update_authority_t *authority, uint64_t job)
{
    if (!updater) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&update_lock);
    esp_err_t e = ESP_OK;
    app_update_state_t state = updater->view.state;
    if (updater->configuring || !updater->view.available || updater->work != WORK_NONE || updater->active_work != WORK_NONE ||
        (state != APP_UPDATE_IDLE && state != APP_UPDATE_AVAILABLE && state != APP_UPDATE_STAGED &&
         state != APP_UPDATE_ERROR && state != APP_UPDATE_CANCELLED &&
         state != APP_UPDATE_CONFIRMED)) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (job && job != updater->view.job_id) {
        e = ESP_ERR_INVALID_ARG;
    }
    if (work == WORK_CHECK && state == APP_UPDATE_STAGED) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (work == WORK_DOWNLOAD && state != APP_UPDATE_AVAILABLE) {
        e = ESP_ERR_INVALID_STATE;
    }
    if ((work == WORK_ACTIVATE || work == WORK_DISCARD) && state != APP_UPDATE_STAGED) {
        e = ESP_ERR_INVALID_STATE;
    }
    if ((work == WORK_DOWNLOAD || work == WORK_ACTIVATE) &&
        (!authority || !authority->authorized || !authority->unlocked ||
         !authority->policy_revision || authority->authority_expires_ms <= now_ms() ||
         strcmp(authority->expected_digest, updater->release.digest))) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (e == ESP_OK) {
        if (work == WORK_CHECK) {
            esp_fill_random(&updater->view.job_id, sizeof(updater->view.job_id));
            if (!updater->view.job_id) {
                updater->view.job_id = 1;
            }
            updater->view.received_bytes = updater->view.written_bytes = 0;
        }
        if (authority) {
            updater->authority = *authority;
            updater->view.policy_revision = authority->policy_revision;
        }
        updater->started = now_ms();
        updater->cancel = false;
        updater->work = work;
        ++updater->view.revision;
    }
    portEXIT_CRITICAL(&update_lock);
    return e;
}
esp_err_t app_update_check(void) { return admit(WORK_CHECK, NULL, 0); }
esp_err_t app_update_download(const char *release, const app_update_authority_t *a)
{
    if (!updater || !release || strcmp(release, updater->release.release_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return admit(WORK_DOWNLOAD, a, 0);
}
esp_err_t app_update_activate(uint64_t job, const app_update_authority_t *a)
{
    return admit(WORK_ACTIVATE, a, job);
}
esp_err_t app_update_discard(uint64_t job) { return admit(WORK_DISCARD, NULL, job); }
esp_err_t app_update_cancel(uint64_t job)
{
    if (!updater) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&update_lock);
    esp_err_t error = job == updater->view.job_id ? ESP_OK : ESP_ERR_INVALID_ARG;
    app_update_state_t state = updater->view.state;
    if (state == APP_UPDATE_SELECTING_BOOT || state == APP_UPDATE_BOOT_SELECTION_UNCERTAIN || updater->view.boot_selected)
        error = ESP_ERR_INVALID_STATE;
    bool active = updater->active_work != WORK_NONE;
    bool queued = updater->work != WORK_NONE;
    if (error == ESP_OK && !active && !queued && state != APP_UPDATE_STAGED && !updater->staged)
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        updater->cancel = true;
        if (!active) {
            updater->work = WORK_NONE; updater->staged = NULL;
            wipe(&updater->authority, sizeof(updater->authority));
            publish_locked(APP_UPDATE_CANCELLED, ESP_OK, "cancelled_queued_or_staged_reference_revoked");
        } else {
            publish_locked(APP_UPDATE_CANCELLING, ESP_OK, "waiting_for_actual_owner_cleanup");
            updater->view.cleanup_pending = true;
        }
    }
    portEXIT_CRITICAL(&update_lock);
    return error;
}

void app_update_publish_authority(const app_update_authority_t *a)
{
    if (!updater || !a) {
        return;
    }
    portENTER_CRITICAL(&update_lock);
    if (a->policy_revision == updater->view.policy_revision) {
        updater->authority = *a;
    }
    portEXIT_CRITICAL(&update_lock);
}
void app_update_snapshot(app_update_snapshot_t *out)
{
    if (!out) {
        return;
    }
    if (!updater) {
        *out = (app_update_snapshot_t){.state = APP_UPDATE_UNAVAILABLE,
                                       .error = ESP_ERR_INVALID_STATE};
        return;
    }
    portENTER_CRITICAL(&update_lock);
    *out = updater->view;
    portEXIT_CRITICAL(&update_lock);
}
esp_err_t app_update_confirm_health(bool gates)
{
    if (!updater || !gates || now_ms() < updater->boot_ms ||
        now_ms() - updater->boot_ms < UPDATE_MINIMUM_HEALTH_MS || now_ms() - updater->boot_ms >= UPDATE_PROBATION_MS) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&update_lock);
    esp_err_t e = updater->view.state == APP_UPDATE_PENDING_HEALTH && updater->probation &&
        !updater->probation_terminal && !updater->health_attempted ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) {
        updater->health_requested = true;
    }
    portEXIT_CRITICAL(&update_lock);
    return e;
}
bool app_update_flash_busy(void)
{
    if (!updater) {
        return false;
    }
    portENTER_CRITICAL(&update_lock);
    app_update_state_t s = updater->view.state;
    bool busy = updater->work == WORK_DOWNLOAD || updater->work == WORK_ACTIVATE ||
                updater->active_work == WORK_DOWNLOAD || updater->active_work == WORK_ACTIVATE ||
                s == APP_UPDATE_DOWNLOADING || s == APP_UPDATE_VERIFYING ||
                s == APP_UPDATE_SELECTING_BOOT || s == APP_UPDATE_WAITING_MAINTENANCE ||
                updater->view.cleanup_pending;
    portEXIT_CRITICAL(&update_lock);
    return busy;
}
