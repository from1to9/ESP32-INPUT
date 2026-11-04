// SPDX-License-Identifier: Apache-2.0
#include "app_control_trust.h"
#include "control_json.h"
#include "control_registry.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BANK_HEADER 48U
#define BANK_BYTES (BANK_HEADER + APP_CONTROL_TRUST_PACKAGE_BYTES)
typedef struct {
    char host[129], certificate[APP_CONTROL_TRUST_CERT_BYTES + 1],
        key[APP_CONTROL_TRUST_KEY_BYTES + 1];
    uint16_t port;
    cp_principal_t principals[CP_MAX_PRINCIPALS];
    size_t principal_count, grant_count, clock_count;
    cp_capture_grant_t grants[CP_MAX_CAPTURE_GRANTS];
    char clocks[2][129];
    bool update_present, replace_unknown_store;
    app_update_provisioned_config_t update;
    bool policy_present;
    app_update_policy_t policy;
} package_t;
typedef struct {
    package_t current, candidate;
    char candidate_json[APP_CONTROL_TRUST_PACKAGE_BYTES + 1];
    size_t candidate_length;
    uint8_t token[32];
    uint64_t until;
    bool approved;
    app_control_trust_snapshot_t view;
    app_control_trust_review_t review;
} trust_t;
static portMUX_TYPE trust_lock = portMUX_INITIALIZER_UNLOCKED;
static trust_t *trust;
static SemaphoreHandle_t storage_mutex;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}
static void *allocate(size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
}
static void *allocate_aligned(size_t alignment, size_t bytes)
{
    void *p = heap_caps_aligned_calloc(alignment, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_calloc(alignment, 1, bytes, MALLOC_CAP_8BIT);
}
static bool equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    unsigned diff = 0;
    for (size_t i = 0; i < n; ++i) {
        diff |= x[i] ^ y[i];
    }
    return diff == 0;
}
static bool decode(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            value = value * 16U + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)value;
    }
    uint8_t bits = 0;
    for (size_t i = 0; i < n; ++i) {
        bits |= out[i];
    }
    return bits != 0;
}
static void encode(const uint8_t *p, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = hex[p[i] >> 4];
        out[2 * i + 1] = hex[p[i] & 15];
    }
    out[2 * n] = 0;
}
static bool exact_keys(const cj_document_t *d, uint16_t object, const char *const *keys, size_t n)
{
    if (object == CJ_NONE || d->nodes[object].type != CJ_OBJECT) {
        return false;
    }
    for (uint16_t t = d->nodes[object].child; t != CJ_NONE; t = d->nodes[t].next) {
        bool found = false;
        for (size_t i = 0; i < n; ++i) {
            if (strcmp(d->text + d->nodes[t].key, keys[i]) == 0) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
static const char *text(const cj_document_t *d, uint16_t o, const char *key)
{
    return cj_string(d, cj_get(d, o, key));
}
static bool copy_string(char *out, size_t n, const char *s)
{
    if (!s || !*s || strlen(s) >= n) {
        return false;
    }
    memcpy(out, s, strlen(s) + 1);
    return true;
}
static bool boolean(const cj_document_t *d, uint16_t o, const char *key, bool *out)
{
    uint16_t t = cj_get(d, o, key);
    if (t == CJ_NONE || d->nodes[t].type != CJ_BOOL) {
        return false;
    }
    *out = d->nodes[t].value.boolean;
    return true;
}
static bool integer(const cj_document_t *d, uint16_t o, const char *key, uint64_t *out)
{
    uint16_t t = cj_get(d, o, key);
    if (cj_u64(d, t, out)) {
        return true;
    }
    int64_t v;
    if (!cj_integer(d, t, &v) || v < 0) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}
static bool parse_grant(const cj_document_t *d, uint16_t o, package_t *p)
{
    static const char *const keys[] = {"id",
                                       "principal_id",
                                       "revision",
                                       "expires_unix_seconds",
                                       "source",
                                       "purpose",
                                       "maximum_bytes",
                                       "maximum_duration_seconds",
                                       "maximum_retention_seconds",
                                       "live_destination",
                                       "file_destination",
                                       "owner_download"};
    if (p->grant_count == CP_MAX_CAPTURE_GRANTS ||
        !exact_keys(d, o, keys, sizeof(keys) / sizeof(*keys))) {
        return false;
    }
    cp_capture_grant_t *g = &p->grants[p->grant_count];
    const char *source = text(d, o, "source"), *purpose = text(d, o, "purpose");
    uint64_t duration, retention;
    if (!decode(text(d, o, "id"), g->id, 16) ||
        !decode(text(d, o, "principal_id"), g->principal_id, 16) ||
        !integer(d, o, "revision", &g->revision) || !g->revision ||
        !integer(d, o, "expires_unix_seconds", &g->expires_unix_seconds) ||
        !g->expires_unix_seconds || !integer(d, o, "maximum_bytes", &g->maximum_bytes) ||
        !g->maximum_bytes || g->maximum_bytes > UINT64_C(8589934592) ||
        !integer(d, o, "maximum_duration_seconds", &duration) || !duration || duration > 28800 ||
        !integer(d, o, "maximum_retention_seconds", &retention) || retention > 31536000 ||
        !source || strcmp(source, "local_microphone") || !purpose ||
        !boolean(d, o, "live_destination", &g->live_destination) ||
        !boolean(d, o, "file_destination", &g->file_destination) ||
        !boolean(d, o, "owner_download", &g->owner_download)) {
        return false;
    }
    if (!strcmp(purpose, "speech")) {
        g->purpose = CP_CAPTURE_SPEECH;
    } else if (!strcmp(purpose, "live_capture")) {
        g->purpose = CP_CAPTURE_LIVE;
    } else if (!strcmp(purpose, "recording")) {
        g->purpose = CP_CAPTURE_RECORDING;
    } else if (!strcmp(purpose, "background_recording")) {
        g->purpose = CP_CAPTURE_BACKGROUND;
    } else {
        return false;
    }
    if ((g->purpose == CP_CAPTURE_SPEECH && (duration > 8 || g->file_destination)) ||
        (g->purpose == CP_CAPTURE_BACKGROUND && (g->live_destination || !g->file_destination)) ||
        (!g->live_destination && !g->file_destination) ||
        (g->owner_download && !g->file_destination)) {
        return false;
    }
    bool principal = false;
    for (size_t i = 0; i < p->principal_count; ++i) {
        if (equal(g->principal_id, p->principals[i].id, 16) && p->principals[i].enabled) {
            cp_scope_mask_t needed =
                g->purpose == CP_CAPTURE_RECORDING || g->purpose == CP_CAPTURE_BACKGROUND
                    ? CP_SCOPE_RECORDING_CONTROL
                    : CP_SCOPE_AUDIO_CAPTURE;
            if ((p->principals[i].scopes & needed) == needed) {
                principal = true;
            }
        }
    }
    for (size_t i = 0; i < p->grant_count; ++i) {
        if (equal(g->id, p->grants[i].id, 16)) {
            return false;
        }
    }
    if (!principal) {
        return false;
    }
    g->maximum_duration_seconds = (uint32_t)duration;
    g->maximum_retention_seconds = (uint32_t)retention;
    ++p->grant_count;
    return true;
}
static esp_err_t parse_package(const char *json, size_t length, package_t *p)
{
    static const char *const keys[] = {"v",
                                       "host",
                                       "port",
                                       "certificate_pem",
                                       "private_key_pem",
                                       "principals",
                                       "capture_grants",
                                       "time_servers",
                                       "update_source",
                                       "replace_unknown_store",
                                       "update_policy"};
    cj_document_t *d = allocate_aligned(_Alignof(cj_document_t), sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    memset(p, 0, sizeof(*p));
    bool valid = cj_parse(d, json, length) && exact_keys(d, 0, keys, sizeof(keys) / sizeof(*keys));
    uint64_t version = 0, port = 0;
    valid = valid && integer(d, 0, "v", &version) && version == 1 && integer(d, 0, "port", &port) &&
            port > 0 && port <= 65535 &&
            copy_string(p->host, sizeof(p->host), text(d, 0, "host")) &&
            copy_string(p->certificate, sizeof(p->certificate), text(d, 0, "certificate_pem")) &&
            copy_string(p->key, sizeof(p->key), text(d, 0, "private_key_pem"));
    p->port = (uint16_t)port;
    for (size_t i = 0; valid && p->host[i]; ++i) {
        unsigned char c = (unsigned char)p->host[i];
        valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '.';
    }
    uint16_t array = cj_get(d, 0, "principals");
    valid = valid && array != CJ_NONE && d->nodes[array].type == CJ_ARRAY;
    for (uint16_t t = valid ? d->nodes[array].child : CJ_NONE; t != CJ_NONE && valid;
         t = d->nodes[t].next) {
        static const char *const pk[] = {"id", "credential_sha256", "scopes", "native_only",
                                         "enabled"};
        if (p->principal_count == CP_MAX_PRINCIPALS ||
            !exact_keys(d, t, pk, sizeof(pk) / sizeof(*pk))) {
            valid = false;
            break;
        }
        cp_principal_t *v = &p->principals[p->principal_count];
        valid = decode(text(d, t, "id"), v->id, 16) &&
                decode(text(d, t, "credential_sha256"), v->credential_sha256, 32) &&
                boolean(d, t, "native_only", &v->native_only) &&
                boolean(d, t, "enabled", &v->enabled);
        uint16_t scopes = cj_get(d, t, "scopes");
        valid = valid && scopes != CJ_NONE && d->nodes[scopes].type == CJ_ARRAY;
        for (uint16_t s = valid ? d->nodes[scopes].child : CJ_NONE; s != CJ_NONE && valid;
             s = d->nodes[s].next) {
            const char *name = cj_string(d, s);
            cp_scope_mask_t bit = name ? cp_scope_from_name(name) : 0;
            if (!bit || (v->scopes & bit)) {
                valid = false;
            }
            v->scopes |= bit;
        }
        for (size_t i = 0; i < p->principal_count; ++i) {
            if (equal(v->id, p->principals[i].id, 16)) {
                valid = false;
            }
        }
        if (!(v->scopes & CP_SCOPE_VIEW)) {
            valid = false;
        }
        ++p->principal_count;
    }
    valid = valid && p->principal_count > 0;
    array = cj_get(d, 0, "capture_grants");
    if (array != CJ_NONE) {
        valid = valid && d->nodes[array].type == CJ_ARRAY;
        for (uint16_t t = valid ? d->nodes[array].child : CJ_NONE; t != CJ_NONE && valid;
             t = d->nodes[t].next) {
            valid = parse_grant(d, t, p);
        }
    }
    array = cj_get(d, 0, "time_servers");
    if (array != CJ_NONE) {
        valid = valid && d->nodes[array].type == CJ_ARRAY;
        for (uint16_t t = valid ? d->nodes[array].child : CJ_NONE; t != CJ_NONE && valid;
             t = d->nodes[t].next) {
            if (p->clock_count == 2 ||
                !copy_string(p->clocks[p->clock_count], 129, cj_string(d, t))) {
                valid = false;
                break;
            }
            const char *s = p->clocks[p->clock_count];
            for (size_t i = 0; s[i]; ++i) {
                unsigned char c = (unsigned char)s[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.')) {
                    valid = false;
                }
            }
            ++p->clock_count;
        }
    }
    uint16_t recovery = cj_get(d, 0, "replace_unknown_store");
    if (recovery != CJ_NONE) {
        valid = valid && boolean(d, 0, "replace_unknown_store", &p->replace_unknown_store);
    }
    uint16_t update = cj_get(d, 0, "update_source");
    if (update != CJ_NONE) {
        static const char *const uk[] = {
            "catalog_url", "tls_ca_pem", "signing_key_id",        "signing_public_key",  "product",
            "board",       "layout",     "minimum_release_index", "allow_private_source"};
        uint64_t minimum = 0;
        valid =
            valid && exact_keys(d, update, uk, sizeof(uk) / sizeof(*uk)) &&
            copy_string(p->update.catalog_url, sizeof(p->update.catalog_url),
                        text(d, update, "catalog_url")) &&
            !strncmp(p->update.catalog_url, "https://", 8) &&
            copy_string(p->update.tls_ca_pem, sizeof(p->update.tls_ca_pem),
                        text(d, update, "tls_ca_pem")) &&
            copy_string(p->update.signing_key_id, sizeof(p->update.signing_key_id),
                        text(d, update, "signing_key_id")) &&
            decode(text(d, update, "signing_public_key"), p->update.signing_public_key, 65) &&
            p->update.signing_public_key[0] == 4 &&
            copy_string(p->update.product, sizeof(p->update.product), text(d, update, "product")) &&
            copy_string(p->update.board, sizeof(p->update.board), text(d, update, "board")) &&
            copy_string(p->update.layout, sizeof(p->update.layout), text(d, update, "layout")) &&
            !strcmp(p->update.layout, APP_UPDATE_LAYOUT) &&
            integer(d, update, "minimum_release_index", &minimum) && minimum <= UINT32_MAX &&
            boolean(d, update, "allow_private_source", &p->update.allow_private_source);
        p->update.minimum_release_index = (uint32_t)minimum;
        p->update_present = valid;
    }
    uint16_t policy = cj_get(d, 0, "update_policy");
    if (policy != CJ_NONE) {
        static const char *const pk[] = {"id",
                                         "principal_id",
                                         "revision",
                                         "expires_unix_seconds",
                                         "maximum_job_ms",
                                         "maximum_bytes",
                                         "allow_download",
                                         "allow_activate",
                                         "allow_restart",
                                         "continue_after_owner_loss",
                                         "allow_signed_releases",
                                         "allowed_digest"};
        app_update_policy_t *v = &p->policy;
        uint64_t job = 0, bytes = 0;
        valid = valid && p->update_present && exact_keys(d, policy, pk, sizeof(pk) / sizeof(*pk)) &&
                decode(text(d, policy, "id"), v->id, 16) &&
                decode(text(d, policy, "principal_id"), v->principal_id, 16) &&
                integer(d, policy, "revision", &v->revision) && v->revision &&
                integer(d, policy, "expires_unix_seconds", &v->expires_unix_seconds) &&
                v->expires_unix_seconds && integer(d, policy, "maximum_job_ms", &job) &&
                job >= 1000 && job <= 600000 && integer(d, policy, "maximum_bytes", &bytes) &&
                bytes > 0 && bytes <= APP_UPDATE_SLOT_BYTES &&
                boolean(d, policy, "allow_download", &v->allow_download) &&
                boolean(d, policy, "allow_activate", &v->allow_activate) &&
                boolean(d, policy, "allow_restart", &v->allow_restart) &&
                boolean(d, policy, "continue_after_owner_loss", &v->continue_after_owner_loss) &&
                boolean(d, policy, "allow_signed_releases", &v->allow_signed_releases);
        v->maximum_job_ms = (uint32_t)job;
        v->maximum_bytes = (uint32_t)bytes;
        if (valid && !v->allow_signed_releases) {
            uint8_t hash[32];
            valid = copy_string(v->allowed_digest, sizeof(v->allowed_digest),
                                text(d, policy, "allowed_digest")) &&
                    decode(v->allowed_digest, hash, 32);
        }
        bool enrolled = false;
        for (size_t i = 0; i < p->principal_count; ++i) {
            if (equal(v->principal_id, p->principals[i].id, 16) && p->principals[i].enabled) {
                cp_scope_mask_t scopes = p->principals[i].scopes;
                enrolled = (!v->allow_download || (scopes & CP_SCOPE_UPDATE_STAGE)) &&
                           (!v->allow_activate || (scopes & CP_SCOPE_UPDATE_ACTIVATE));
            }
        }
        valid = valid && enrolled && (!v->allow_restart || v->allow_activate);
        if (v->allow_restart) {
            bool scope = false;
            for (size_t i = 0; i < p->principal_count; ++i) {
                if (equal(v->principal_id, p->principals[i].id, 16) &&
                    (p->principals[i].scopes & CP_SCOPE_SYSTEM_RESTART)) {
                    scope = true;
                }
            }
            valid = valid && scope;
        }
        p->policy_present = valid;
    }
    if (valid) {
        mbedtls_x509_crt certificate;
        mbedtls_pk_context key;
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
        valid = mbedtls_x509_crt_parse(&certificate, (const unsigned char *)p->certificate,
                                       strlen(p->certificate) + 1) == 0 &&
                mbedtls_pk_parse_key(&key, (const unsigned char *)p->key, strlen(p->key) + 1, NULL,
                                     0) == 0 &&
                mbedtls_pk_check_pair(&certificate.pk, &key) == 0;
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&certificate);
    }
    wipe(d, sizeof(*d));
    heap_caps_free(d);
    if (!valid) {
        wipe(p, sizeof(*p));
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
static uint64_t be64(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) {
        n = (n << 8) | p[i];
    }
    return n;
}
static void put64(uint8_t *p, uint64_t n)
{
    for (int i = 7; i >= 0; --i) {
        p[i] = (uint8_t)n;
        n >>= 8;
    }
}
/* Header: magic[4], generation[8], length[4], digest[32]. Hash includes
 * generation/length, so a torn record cannot manufacture a newer revision. */
static esp_err_t read_bank(nvs_handle_t nvs, const char *key, uint8_t *bank, size_t *length,
                           uint64_t *generation)
{
    size_t size = BANK_BYTES;
    esp_err_t e = nvs_get_blob(nvs, key, bank, &size);
    if (e != ESP_OK) {
        return e;
    }
    if (size < BANK_HEADER || memcmp(bank, "CTB1", 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    uint32_t n = ((uint32_t)bank[12] << 24) | ((uint32_t)bank[13] << 16) |
                 ((uint32_t)bank[14] << 8) | bank[15];
    if (n > APP_CONTROL_TRUST_PACKAGE_BYTES || n + BANK_HEADER != size || !be64(bank + 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    uint8_t digest[32], stored[32];
    memcpy(stored, bank + 16, 32);
    memset(bank + 16, 0, 32);
    cp_sha256(bank, size, digest);
    memcpy(bank + 16, stored, 32);
    if (!equal(stored, digest, 32)) {
        return ESP_ERR_INVALID_CRC;
    }
    *length = n;
    *generation = be64(bank + 4);
    return ESP_OK;
}
esp_err_t app_control_trust_verify(void)
{
    if (!trust || !storage_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&trust_lock);
    trust->view.known = false;
    trust->view.present = false;
    wipe(trust->view.active_digest, sizeof(trust->view.active_digest));
    trust->view.uncertain = true;
    portEXIT_CRITICAL(&trust_lock);
    uint8_t *bank = allocate(BANK_BYTES);
    package_t *loaded = allocate_aligned(_Alignof(package_t), sizeof(*loaded));
    esp_err_t error = (!bank || !loaded) ? ESP_ERR_NO_MEM : ESP_OK;
    nvs_handle_t nvs = 0;
    uint64_t best = 0;
    uint8_t best_digest[32] = {0};
    bool found = false, ambiguous = false;
    if (error == ESP_OK) {
        error = nvs_open_from_partition(APP_CONTROL_TRUST_PARTITION, APP_CONTROL_TRUST_NAMESPACE,
                                        NVS_READONLY, &nvs);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (nvs) {
        for (unsigned i = 0; i < 2; ++i) {
            size_t length = 0;
            uint64_t generation = 0;
            esp_err_t e =
                read_bank(nvs, i ? "identity_b" : "identity_a", bank, &length, &generation);
            if (e == ESP_ERR_NVS_NOT_FOUND) {
                continue;
            }
            if (e != ESP_OK) {
                ambiguous = true;
                error = e;
                continue;
            }
            if (generation > best) {
                esp_err_t parsed = parse_package((const char *)bank + BANK_HEADER, length, loaded);
                if (parsed != ESP_OK) {
                    ambiguous = true;
                    error = parsed;
                    continue;
                }
                best = generation;
                cp_sha256(bank + BANK_HEADER, length, best_digest);
                found = true;
                portENTER_CRITICAL(&trust_lock);
                trust->current = *loaded;
                portEXIT_CRITICAL(&trust_lock);
            } else if (generation == best) {
                ambiguous = true;
                error = ESP_ERR_INVALID_CRC;
            }
        }
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&trust_lock);
    trust->view.known = error == ESP_OK && !ambiguous;
    trust->view.uncertain = !trust->view.known;
    trust->view.present = found && trust->view.known;
    trust->view.revision = best;
    trust->view.error = error;
    if (trust->view.present) {
        encode(best_digest, sizeof(best_digest), trust->view.active_digest);
    } else {
        wipe(&trust->current, sizeof(trust->current));
        wipe(trust->view.active_digest, sizeof(trust->view.active_digest));
    }
    portEXIT_CRITICAL(&trust_lock);
    if (bank) {
        wipe(bank, BANK_BYTES);
        heap_caps_free(bank);
    }
    if (loaded) {
        wipe(loaded, sizeof(*loaded));
        heap_caps_free(loaded);
    }
    xSemaphoreGive(storage_mutex);
    return error;
}
esp_err_t app_control_trust_init(void)
{
    if (trust) {
        return ESP_ERR_INVALID_STATE;
    }
    trust_t *created = allocate_aligned(_Alignof(trust_t), sizeof(*created));
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!created || !mutex) {
        if (created) {
            heap_caps_free(created);
        }
        if (mutex) {
            vSemaphoreDelete(mutex);
        }
        return ESP_ERR_NO_MEM;
    }
    trust = created;
    storage_mutex = mutex;
    esp_err_t e = nvs_flash_init_partition(APP_CONTROL_TRUST_PARTITION);
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, APP_CONTROL_TRUST_PARTITION);
    trust->view.initialized = e == ESP_OK;
    trust->view.at_rest_protected = partition && partition->encrypted;
    trust->view.error = e;
    trust->view.uncertain = e != ESP_OK;
    return e == ESP_OK ? app_control_trust_verify() : e;
}
static void close_locked(void)
{
    trust->view.import_window = false;
    trust->view.review_pending = false;
    trust->view.candidate_digest[0] = 0;
    trust->approved = false;
    trust->until = 0;
    trust->candidate_length = 0;
    wipe(trust->token, sizeof(trust->token));
    wipe(&trust->candidate, sizeof(trust->candidate));
    wipe(trust->candidate_json, sizeof(trust->candidate_json));
    wipe(&trust->review, sizeof(trust->review));
}
esp_err_t app_control_trust_open_local_window(uint8_t token[32])
{
    if (!trust || !trust->view.initialized || !token) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t random[32];
    esp_fill_random(random, sizeof(random));
    uint64_t until = now_ms() + APP_CONTROL_TRUST_WINDOW_MS;
    portENTER_CRITICAL(&trust_lock);
    close_locked();
    memcpy(trust->token, random, 32);
    memcpy(token, random, 32);
    trust->until = until;
    trust->view.import_window = true;
    portEXIT_CRITICAL(&trust_lock);
    wipe(random, sizeof(random));
    return ESP_OK;
}
void app_control_trust_close_local_window(void)
{
    if (!trust) {
        return;
    }
    portENTER_CRITICAL(&trust_lock);
    close_locked();
    portEXIT_CRITICAL(&trust_lock);
}
esp_err_t app_control_trust_stage(const uint8_t token[32], const char *json, size_t length)
{
    if (!trust || !token || !json || !length || length > APP_CONTROL_TRUST_PACKAGE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    package_t *candidate = allocate_aligned(_Alignof(package_t), sizeof(*candidate));
    if (!candidate) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = parse_package(json, length, candidate);
    uint8_t digest[32];
    cp_sha256(json, length, digest);
    uint64_t now = now_ms();
    portENTER_CRITICAL(&trust_lock);
    if (!trust->view.import_window || now >= trust->until || !equal(token, trust->token, 32)) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        trust->candidate = *candidate;
        memcpy(trust->candidate_json, json, length);
        trust->candidate_json[length] = 0;
        trust->candidate_length = length;
        trust->approved = false;
        trust->view.review_pending = true;
        encode(digest, 32, trust->view.candidate_digest);
        trust->review =
            (app_control_trust_review_t){.principal_count = (uint8_t)candidate->principal_count,
                                         .capture_grant_count = (uint8_t)candidate->grant_count};
        strcpy(trust->review.digest, trust->view.candidate_digest);
        strcpy(trust->review.host, candidate->host);
        trust->review.port = candidate->port;
        trust->review.clock_server_count = (uint8_t)candidate->clock_count;
        memcpy(trust->review.clock_servers, candidate->clocks, sizeof(candidate->clocks));
        for (size_t i = 0; i < candidate->principal_count; ++i) {
            trust->review.principal_scopes[i] = candidate->principals[i].scopes;
            memcpy(trust->review.principals[i].id, candidate->principals[i].id, 16);
            trust->review.principals[i].scopes = candidate->principals[i].scopes;
            trust->review.principals[i].native_only = candidate->principals[i].native_only;
            trust->review.principals[i].enabled = candidate->principals[i].enabled;
        }
        memcpy(trust->review.grants, candidate->grants, sizeof(candidate->grants));
        uint8_t fingerprint[32];
        cp_sha256(candidate->certificate, strlen(candidate->certificate), fingerprint);
        encode(fingerprint, 32, trust->review.certificate_pem_sha256);
        trust->review.update_policy_present = candidate->policy_present;
        trust->review.update_policy = candidate->policy;
        cp_sha256(candidate->update.signing_public_key, 65, fingerprint);
        encode(fingerprint, 32, trust->review.update_public_key_sha256);
        trust->review.update_source_present = candidate->update_present;
        trust->review.replace_unknown_store = candidate->replace_unknown_store;
        if (candidate->update_present) {
            strcpy(trust->review.update_catalog_url, candidate->update.catalog_url);
            strcpy(trust->review.update_signing_key_id, candidate->update.signing_key_id);
            strcpy(trust->review.update_product, candidate->update.product);
            strcpy(trust->review.update_board, candidate->update.board);
            strcpy(trust->review.update_layout, candidate->update.layout);
            trust->review.update_minimum_release_index = candidate->update.minimum_release_index;
            trust->review.update_allow_private_source = candidate->update.allow_private_source;
            cp_sha256(candidate->update.tls_ca_pem, strlen(candidate->update.tls_ca_pem),
                      fingerprint);
            encode(fingerprint, 32, trust->review.update_tls_ca_sha256);
        }
    }
    portEXIT_CRITICAL(&trust_lock);
    wipe(candidate, sizeof(*candidate));
    heap_caps_free(candidate);
    return error;
}
bool app_control_trust_review(app_control_trust_review_t *out)
{
    if (!trust || !out) {
        return false;
    }
    uint64_t now = now_ms();
    portENTER_CRITICAL(&trust_lock);
    bool ready = trust->view.review_pending && now < trust->until;
    if (ready) {
        *out = trust->review;
    }
    portEXIT_CRITICAL(&trust_lock);
    return ready;
}
esp_err_t app_control_trust_approve_local(const char digest[65])
{
    if (!trust || !digest || strlen(digest) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t now = now_ms();
    portENTER_CRITICAL(&trust_lock);
    bool valid = trust->view.review_pending && trust->view.import_window && now < trust->until &&
                 equal(digest, trust->view.candidate_digest, 64);
    if (valid) {
        trust->approved = true;
    }
    portEXIT_CRITICAL(&trust_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t app_control_trust_import(const uint8_t token[32], const char *json, size_t length)
{
    if (!trust || !token || !json || !length || length > APP_CONTROL_TRUST_PACKAGE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    app_control_server_snapshot_t server;
    app_control_server_snapshot(&server);
    if (server.state != APP_CONTROL_STOPPED && server.state != APP_CONTROL_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t *bank = allocate(BANK_BYTES);
    esp_err_t error = bank ? ESP_OK : ESP_ERR_NO_MEM;
    uint8_t digest[32];
    cp_sha256(json, length, digest);
    char hex[65];
    encode(digest, 32, hex);
    uint64_t now = now_ms(), generation = 0;
    portENTER_CRITICAL(&trust_lock);
    bool recovering = !trust->view.known || trust->view.uncertain;
    bool valid = (trust->view.known || trust->candidate.replace_unknown_store) &&
                 trust->view.import_window && trust->approved && now < trust->until &&
                 equal(token, trust->token, 32) && equal(hex, trust->view.candidate_digest, 64) &&
                 length == trust->candidate_length && equal(json, trust->candidate_json, length) &&
                 trust->view.revision < (recovering ? UINT64_MAX - 1 : UINT64_MAX);
    generation = trust->view.revision + 1;
    trust->approved = false;
    portEXIT_CRITICAL(&trust_lock);
    if (!valid) {
        error = ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t nvs = 0;
    if (error == ESP_OK) {
        memcpy(bank, "CTB1", 4);
        put64(bank + 4, generation);
        bank[12] = (uint8_t)(length >> 24);
        bank[13] = (uint8_t)(length >> 16);
        bank[14] = (uint8_t)(length >> 8);
        bank[15] = (uint8_t)length;
        memcpy(bank + BANK_HEADER, json, length);
        cp_sha256(bank, BANK_HEADER + length, digest);
        memcpy(bank + 16, digest, 32);
        error = nvs_open_from_partition(APP_CONTROL_TRUST_PARTITION, APP_CONTROL_TRUST_NAMESPACE,
                                        NVS_READWRITE, &nvs);
        if (error == ESP_OK) {
            error = nvs_set_blob(nvs, (generation & 1) ? "identity_a" : "identity_b", bank,
                                 BANK_HEADER + length);
        }
        if (error == ESP_OK) {
            error = nvs_commit(nvs);
        }
        if (error == ESP_OK && recovering && generation < UINT64_MAX) {
            ++generation;
            put64(bank + 4, generation);
            memset(bank + 16, 0, 32);
            cp_sha256(bank, BANK_HEADER + length, digest);
            memcpy(bank + 16, digest, 32);
            error = nvs_set_blob(nvs, (generation & 1) ? "identity_a" : "identity_b", bank,
                                 BANK_HEADER + length);
            if (error == ESP_OK) {
                error = nvs_commit(nvs);
            }
        }
        /* A write/commit error may already have changed flash. Never describe
         * it as rollback; explicit verification is required before reuse. */
        portENTER_CRITICAL(&trust_lock);
        trust->view.known = false;
        trust->view.present = false;
        wipe(trust->view.active_digest, sizeof(trust->view.active_digest));
        trust->view.uncertain = true;
        trust->view.error = error;
        close_locked();
        portEXIT_CRITICAL(&trust_lock);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    if (bank) {
        wipe(bank, BANK_BYTES);
        heap_caps_free(bank);
    }
    xSemaphoreGive(storage_mutex);
    return error == ESP_OK ? app_control_trust_verify() : error;
}
void app_control_trust_snapshot(app_control_trust_snapshot_t *out)
{
    if (!out) {
        return;
    }
    if (!trust) {
        *out = (app_control_trust_snapshot_t){.error = ESP_ERR_INVALID_STATE};
        return;
    }
    uint64_t now = now_ms();
    portENTER_CRITICAL(&trust_lock);
    if (trust->view.import_window && now >= trust->until) {
        close_locked();
    }
    *out = trust->view;
    out->window_remaining_ms = out->import_window ? (uint32_t)(trust->until - now) : 0;
    portEXIT_CRITICAL(&trust_lock);
}
esp_err_t app_control_trust_start_server(const cp_callbacks_t *callbacks)
{
    if (!trust || !callbacks) {
        return ESP_ERR_INVALID_STATE;
    }
    package_t *p = allocate_aligned(_Alignof(package_t), sizeof(*p));
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&trust_lock);
    bool valid = trust->view.known && trust->view.present && !trust->view.uncertain &&
                 !trust->view.import_window;
    if (valid) {
        *p = trust->current;
    }
    portEXIT_CRITICAL(&trust_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (valid) {
        char host[140], origin[150];
        if (p->port == 443) {
            snprintf(host, sizeof(host), "%s", p->host);
        } else {
            snprintf(host, sizeof(host), "%s:%u", p->host, p->port);
        }
        snprintf(origin, sizeof(origin), "https://%s", host);
        app_control_server_config_t config = {.abi_version = APP_CONTROL_SERVER_ABI,
                                              .station_ifkey = "WIFI_STA_DEF",
                                              .allowed_host = host,
                                              .browser_origin = origin,
                                              .certificate_pem = (const uint8_t *)p->certificate,
                                              .certificate_length = strlen(p->certificate) + 1,
                                              .private_key_pem = (const uint8_t *)p->key,
                                              .private_key_length = strlen(p->key) + 1,
                                              .port = p->port,
                                              .coordinator = *callbacks,
                                              .principals = p->principals,
                                              .principal_count = p->principal_count};
        error = app_control_server_start(&config);
    }
    wipe(p, sizeof(*p));
    heap_caps_free(p);
    return error;
}
size_t app_control_trust_principals(cp_principal_t out[CP_MAX_PRINCIPALS], char digest[65])
{
    size_t count = 0;
    if (digest) digest[0] = 0;
    if (!trust || !out || !digest) return 0;
    portENTER_CRITICAL(&trust_lock);
    if (trust->view.known && trust->view.present && !trust->view.uncertain) {
        memcpy(digest, trust->view.active_digest, 65);
        count = trust->current.principal_count;
        memcpy(out, trust->current.principals, count * sizeof(*out));
    }
    portEXIT_CRITICAL(&trust_lock);
    return count;
}
size_t app_control_trust_grants(const uint8_t principal[16],
                                cp_capture_grant_t out[CP_MAX_CAPTURE_GRANTS])
{
    if (!trust || !principal || !out) {
        return 0;
    }
    size_t count = 0;
    portENTER_CRITICAL(&trust_lock);
    if (trust->view.known && trust->view.present && !trust->view.uncertain) {
        for (size_t i = 0; i < trust->current.grant_count; ++i) {
            if (equal(principal, trust->current.grants[i].principal_id, 16)) {
                out[count++] = trust->current.grants[i];
            }
        }
    }
    portEXIT_CRITICAL(&trust_lock);
    return count;
}
bool app_control_trust_grant(const uint8_t id[16], cp_capture_grant_t *out)
{
    if (!trust || !id || !out) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&trust_lock);
    if (trust->view.known && trust->view.present && !trust->view.uncertain) {
        for (size_t i = 0; i < trust->current.grant_count; ++i) {
            if (equal(id, trust->current.grants[i].id, 16)) {
                *out = trust->current.grants[i];
                found = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&trust_lock);
    return found;
}
size_t app_control_trust_clock_servers(char out[2][129])
{
    if (!trust || !out) {
        return 0;
    }
    size_t n = 0;
    portENTER_CRITICAL(&trust_lock);
    if (trust->view.known && trust->view.present) {
        n = trust->current.clock_count;
        memcpy(out, trust->current.clocks, sizeof(trust->current.clocks));
    }
    portEXIT_CRITICAL(&trust_lock);
    return n;
}

bool app_control_trust_update_config(app_update_provisioned_config_t *out)
{
    if (!trust || !out) {
        return false;
    }
    portENTER_CRITICAL(&trust_lock);
    bool ready = trust->view.known && trust->view.present && !trust->view.uncertain &&
                 trust->current.update_present;
    if (ready) {
        *out = trust->current.update;
    }
    portEXIT_CRITICAL(&trust_lock);
    return ready;
}

bool app_control_trust_update_policy(const uint8_t principal[16], app_update_policy_t *out)
{
    if (!trust || !principal || !out) {
        return false;
    }
    portENTER_CRITICAL(&trust_lock);
    bool ready = trust->view.known && trust->view.present && !trust->view.uncertain &&
                 trust->current.update_present && trust->current.policy_present &&
                 equal(principal, trust->current.policy.principal_id, 16);
    if (ready) {
        *out = trust->current.policy;
    }
    portEXIT_CRITICAL(&trust_lock);
    return ready;
}
