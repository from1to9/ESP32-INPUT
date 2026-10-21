// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_control_trust.c"

static uint64_t clock_ms = 100;
static uint8_t banks[2][BANK_BYTES];
static size_t lengths[2];
static esp_err_t write_error, commit_error, read_error;
static bool apply_failed_write;
static unsigned writes, starts, critical;
void test_enter(void) { ++critical; }
void test_leave(void)
{
    assert(critical);
    --critical;
}
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
void esp_fill_random(void *p, size_t n) { memset(p, 0x35, n); }
void *heap_caps_calloc(size_t n, size_t s, unsigned caps)
{
    (void)caps;
    return calloc(n, s);
}
void heap_caps_free(void *p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t)
{
    (void)m;
    (void)t;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t m) { (void)m; }
void vSemaphoreDelete(SemaphoreHandle_t m) { (void)m; }
esp_err_t nvs_flash_init_partition(const char *p)
{
    assert(!strcmp(p, "control"));
    return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *p, const char *ns, int mode, nvs_handle_t *out)
{
    assert(!strcmp(p, "control") && !strcmp(ns, "ctrl_v1"));
    (void)mode;
    *out = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t n, const char *key, void *out, size_t *length)
{
    assert(n == 1);
    if (read_error) {
        return read_error;
    }
    unsigned i = !strcmp(key, "identity_b");
    assert(i || !strcmp(key, "identity_a"));
    if (!lengths[i]) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < lengths[i]) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, banks[i], lengths[i]);
    *length = lengths[i];
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t n, const char *key, const void *data, size_t length)
{
    assert(n == 1 && length <= BANK_BYTES);
    unsigned i = !strcmp(key, "identity_b");
    assert(i || !strcmp(key, "identity_a"));
    ++writes;
    if (!write_error || apply_failed_write) {
        memcpy(banks[i], data, length);
        lengths[i] = length;
    }
    return write_error;
}
esp_err_t nvs_commit(nvs_handle_t n)
{
    assert(n == 1);
    return commit_error;
}
void nvs_close(nvs_handle_t n) { assert(n == 1); }
const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype, const char *label)
{
    static const esp_partition_t p = {.encrypted = false};
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS &&
           !strcmp(label, "control"));
    return &p;
}
void mbedtls_x509_crt_init(mbedtls_x509_crt *p) { memset(p, 0, sizeof(*p)); }
void mbedtls_x509_crt_free(mbedtls_x509_crt *p) { (void)p; }
int mbedtls_x509_crt_parse(mbedtls_x509_crt *p, const unsigned char *s, size_t n)
{
    (void)p;
    return n == 5 && !memcmp(s, "CERT", 5) ? 0 : -1;
}
void mbedtls_pk_init(mbedtls_pk_context *p) { memset(p, 0, sizeof(*p)); }
void mbedtls_pk_free(mbedtls_pk_context *p) { (void)p; }
int mbedtls_pk_parse_key(mbedtls_pk_context *p, const unsigned char *s, size_t n,
                         const unsigned char *pwd, size_t pwd_length)
{
    (void)p;
    assert(!pwd && !pwd_length);
    return n == 4 && !memcmp(s, "KEY", 4) ? 0 : -1;
}
int mbedtls_pk_check_pair(const mbedtls_pk_context *a, const mbedtls_pk_context *b)
{
    (void)a;
    (void)b;
    return 0;
}
void app_control_server_snapshot(app_control_server_snapshot_t *out)
{
    *out = (app_control_server_snapshot_t){.state = APP_CONTROL_STOPPED};
}
esp_err_t app_control_server_start(const app_control_server_config_t *config)
{
    assert(config->port == 443 && !strcmp(config->allowed_host, "device.test") &&
           config->principal_count == 1);
    ++starts;
    return ESP_OK;
}

static char package[4097];
static void make_package(bool recovery)
{
    int n = snprintf(
        package, sizeof(package),
        "{\"v\":1,\"host\":\"device.test\",\"port\":443,\"certificate_pem\":\"CERT\",\"private_key_"
        "pem\":\"KEY\",\"replace_unknown_store\":%s,\"principals\":[{\"id\":"
        "\"11111111111111111111111111111111\",\"credential_sha256\":"
        "\"2222222222222222222222222222222222222222222222222222222222222222\",\"scopes\":[\"view\","
        "\"audio.capture\"],\"native_only\":true,\"enabled\":true}],\"time_servers\":[\"clock."
        "example\"],\"capture_grants\":[{\"id\":\"33333333333333333333333333333333\",\"principal_"
        "id\":\"11111111111111111111111111111111\",\"revision\":\"1\",\"expires_unix_seconds\":"
        "\"2000000000\",\"source\":\"local_microphone\",\"purpose\":\"live_capture\",\"maximum_"
        "bytes\":\"8589934592\",\"maximum_duration_seconds\":28800,\"maximum_retention_seconds\":0,"
        "\"live_destination\":true,\"file_destination\":false,\"owner_download\":false}]}",
        recovery ? "true" : "false");
    assert(n > 0 && (size_t)n < sizeof(package));
}
static void stage_and_approve(uint8_t token[32])
{
    assert(app_control_trust_open_local_window(token) == ESP_OK);
    assert(app_control_trust_stage(token, package, strlen(package)) == ESP_OK);
    app_control_trust_review_t review;
    assert(app_control_trust_review(&review));
    assert(review.capture_grant_count == 1 && review.principal_count == 1 &&
           review.grants[0].maximum_duration_seconds == 28800);
    assert(app_control_trust_approve_local(review.digest) == ESP_OK);
}
static void test_review_authority_disclosure(void)
{
    app_control_trust_review_t baseline = {0};
    uint8_t token[32];
    for (unsigned variant = 0; variant < 9; ++variant) {
        make_package(false);
        size_t used = strlen(package) - 1;
        int n =
            snprintf(package + used, sizeof(package) - used,
                     ",\"update_source\":{\"catalog_url\":\"https://updates.example/catalog\","
                     "\"tls_ca_pem\":\"CA%s\",\"signing_key_id\":\"key1\",\"signing_public_key\":"
                     "\"04%0128d\","
                     "\"product\":\"%s\",\"board\":\"%s\",\"layout\":\"%s\","
                     "\"minimum_release_index\":%u,\"allow_private_source\":%s}}",
                     variant == 2 ? "-changed" : "", 0, variant == 4 ? "product-other" : "product",
                     variant == 5 ? "board-other" : "board", APP_UPDATE_LAYOUT,
                     variant == 3 ? 0 : 1, variant == 1 ? "true" : "false");
        assert(n > 0 && (size_t)n < sizeof(package) - used);
        if (variant == 6) {
            char *clock = strstr(package, "clock.example");
            assert(clock);
            memcpy(clock, "other.example", 13);
        }
        if (variant == 7) {
            char *port = strstr(package, "443");
            assert(port);
            memcpy(port, "844", 3);
        }
        /* A changed valid source URL is already disclosed by the old ABI. */
        if (variant == 8) {
            char *url = strstr(package, "updates.example");
            assert(url);
            memcpy(url, "changed.example", 15);
        }
        assert(app_control_trust_open_local_window(token) == ESP_OK);
        assert(app_control_trust_stage(token, package, strlen(package)) == ESP_OK);
        app_control_trust_review_t current;
        assert(app_control_trust_review(&current));
        assert(current.port && current.clock_server_count == 1 && current.update_source_present);
        assert(!strcmp(current.update_layout, APP_UPDATE_LAYOUT));
        assert(strlen(current.update_tls_ca_sha256) == 64 && current.clock_servers[0][0]);
        memset(current.digest, 0, sizeof(current.digest));
        if (!variant) {
            baseline = current;
        } else {
            assert(memcmp(&baseline, &current,
                          sizeof(current))); /* Authority cannot hide behind digest. */
        }
        app_control_trust_close_local_window();
    }
}
int main(void)
{
    assert(app_control_trust_init() == ESP_OK);
    app_control_trust_snapshot_t view;
    app_control_trust_snapshot(&view);
    assert(view.known && !view.present && !view.at_rest_protected);
    test_review_authority_disclosure();
    make_package(false);
    uint8_t token[32];
    assert(app_control_trust_open_local_window(token) == ESP_OK);
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_ERR_INVALID_STATE &&
           writes == 0);
    assert(app_control_trust_stage(token, package, strlen(package)) == ESP_OK);
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_ERR_INVALID_STATE &&
           writes == 0);
    app_control_trust_review_t review;
    assert(app_control_trust_review(&review));
    char wrong[65];
    memset(wrong, 'a', 64);
    wrong[64] = 0;
    assert(app_control_trust_approve_local(wrong) == ESP_ERR_INVALID_STATE);
    assert(app_control_trust_approve_local(review.digest) == ESP_OK);
    package[strlen(package) - 1] = ' ';
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_ERR_INVALID_STATE &&
           writes == 0);
    make_package(false);
    stage_and_approve(token);
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_OK);
    app_control_trust_snapshot(&view);
    assert(view.present && view.known && view.revision == 1 && !view.import_window);
    assert(!strcmp(view.active_digest, review.digest));
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_ERR_INVALID_STATE &&
           writes == 1);
    cp_callbacks_t callbacks = {0};
    assert(app_control_trust_start_server(&callbacks) == ESP_OK && starts == 1);
    uint8_t principal[16];
    memset(principal, 0x11, 16);
    cp_capture_grant_t grants[8];
    assert(app_control_trust_grants(principal, grants) == 1);
    memset(principal, 0x44, 16);
    assert(app_control_trust_grants(principal, grants) == 0);
    stage_and_approve(token);
    clock_ms += APP_CONTROL_TRUST_WINDOW_MS;
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_ERR_INVALID_STATE &&
           writes == 1);
    stage_and_approve(token);
    write_error = ESP_FAIL;
    apply_failed_write = true;
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_FAIL);
    app_control_trust_snapshot(&view);
    assert(view.uncertain && !view.present && !view.known && !view.active_digest[0]);
    assert(app_control_trust_grants(grants[0].principal_id, grants) == 0);
    write_error = ESP_OK;
    assert(app_control_trust_verify() == ESP_OK);
    app_control_trust_snapshot(&view);
    assert(view.known && view.present && view.revision == 2);
    banks[1][0] = 0;
    assert(app_control_trust_verify() != ESP_OK);
    app_control_trust_snapshot(&view);
    assert(!view.known && view.uncertain);
    make_package(true);
    stage_and_approve(token);
    assert(app_control_trust_import(token, package, strlen(package)) == ESP_OK);
    app_control_trust_snapshot(&view);
    assert(view.known && view.present && !view.uncertain);
    read_error = ESP_FAIL;
    assert(app_control_trust_verify() == ESP_FAIL);
    app_control_trust_snapshot(&view);
    assert(!view.known && view.uncertain);
    read_error = 0;
    assert(app_control_trust_verify() == ESP_OK);
    assert(critical == 0);
    wipe(trust, sizeof(*trust));
    free(trust);
    trust = NULL;
    puts("PASS: protected trust local Review, exact digest/single use/expiry, grants, bank "
         "failures and explicit recovery");
    return 0;
}
