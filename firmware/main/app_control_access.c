// SPDX-License-Identifier: Apache-2.0
#include "app_control_access.h"
#include "app_control_trust.h"
#include "app_clock.h"
#include "app_control_server.h"
#include "control_registry.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RECORD_BYTES 104U
#define BANK_BYTES (113U + CP_MAX_PRINCIPALS * RECORD_BYTES)
typedef struct {
    cp_principal_t principal;
    char label[APP_CONTROL_ACCESS_LABEL];
    uint8_t permissions;
    app_control_access_duration_t duration;
    uint64_t expires, revision;
    bool legacy, suspended, expired;
} access_record_t;
typedef struct {
    bool known, uncertain, queued, processing, expiry_pending;
    esp_err_t error;
    char trust_digest[65];
    uint64_t revision, storage_generation, serial, pending_id, pending_until;
    access_record_t saved[CP_MAX_PRINCIPALS], candidate[CP_MAX_PRINCIPALS], proposal;
    unsigned count;
    app_control_access_request_t request;
    uint64_t receipt;
    cp_result_state_t receipt_state;
    cp_error_t receipt_error;
    uint8_t bank[BANK_BYTES], verify[BANK_BYTES]; /* Management worker only. */
} access_t;
static access_t *access_state;
static portMUX_TYPE access_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t ticks(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void zero(void *v, size_t n) { volatile uint8_t *p = v; while (n--) *p++ = 0; }
static bool same(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b; unsigned difference = 0;
    for (size_t i = 0; i < n; ++i) difference |= x[i] ^ y[i];
    return difference == 0;
}
static bool unhex(const char *text, uint8_t *bytes, size_t count)
{
    if (!text || strlen(text) != count * 2) return false;
    unsigned nonzero = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            value = value * 16U + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        bytes[i] = (uint8_t)value; nonzero |= value;
    }
    return nonzero != 0;
}
static void hex(const uint8_t *v, size_t n, char *out)
{
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[i*2] = digits[v[i] >> 4]; out[i*2+1] = digits[v[i] & 15]; }
    out[n*2] = 0;
}
static uint64_t get64(const uint8_t *v)
{
    uint64_t result = 0; for (unsigned i = 0; i < 8; ++i) result = result * 256U + v[i]; return result;
}
static void put64(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[7-i] = (uint8_t)(value >> (8*i));
}
static bool label_valid(const char *s)
{
    if (!s) return false;
    size_t n = strnlen(s, APP_CONTROL_ACCESS_LABEL);
    if (!n || n == APP_CONTROL_ACCESS_LABEL) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return false;
    return true;
}
static cp_scope_mask_t scopes(uint8_t permission)
{
    cp_scope_mask_t result = CP_SCOPE_VIEW;
    if (permission & APP_CONTROL_ACCESS_INPUT) result |= CP_SCOPE_INPUT_CONTROL;
    if (permission & APP_CONTROL_ACCESS_MANAGE) result |= CP_SCOPE_INPUT_CONFIGURE | CP_SCOPE_CONFIG_WRITE |
        CP_SCOPE_CONNECTIONS_MANAGE | CP_SCOPE_WIFI_CONFIGURE | CP_SCOPE_DIAGNOSTICS_READ;
    if (permission & APP_CONTROL_ACCESS_UPDATE) result |= CP_SCOPE_UPDATE_CHECK | CP_SCOPE_UPDATE_STAGE |
        CP_SCOPE_UPDATE_ACTIVATE | CP_SCOPE_SYSTEM_RESTART;
    return result;
}
static uint8_t permissions(cp_scope_mask_t value)
{
    return (uint8_t)(((value & CP_SCOPE_INPUT_CONTROL) ? APP_CONTROL_ACCESS_INPUT : 0) |
        ((value & (CP_SCOPE_CONFIG_WRITE | CP_SCOPE_WIFI_CONFIGURE)) ? APP_CONTROL_ACCESS_MANAGE : 0) |
        ((value & (CP_SCOPE_UPDATE_CHECK | CP_SCOPE_UPDATE_STAGE | CP_SCOPE_UPDATE_ACTIVATE)) ? APP_CONTROL_ACCESS_UPDATE : 0));
}
static bool usable(const access_record_t *r, bool known, uint64_t wall)
{
    if (!r->principal.enabled || r->suspended || r->expired) return false;
    if (r->duration == APP_CONTROL_ACCESS_PERMANENT) return true;
    if (!r->revision) return false;
    bool expired = known && wall >= r->expires;
    /* Readers stop authority immediately. Only the management owner persists
     * the terminal observation, bound to this exact grant revision. */
    portENTER_CRITICAL(&access_lock);
    if (access_state) for (unsigned i = 0; i < access_state->count; ++i) {
        access_record_t *saved = &access_state->saved[i];
        if (r->revision && r->revision == saved->revision && same(r->principal.id, saved->principal.id, 16)) {
            if (expired && !saved->expired) { saved->expired = true; access_state->expiry_pending = true; }
            expired |= saved->expired;
            break;
        }
    }
    portEXIT_CRITICAL(&access_lock);
    return known && !expired;
}
static unsigned records_version(access_record_t out[CP_MAX_PRINCIPALS], uint64_t *revision, bool *coherent)
{
    cp_principal_t legacy[CP_MAX_PRINCIPALS];
    char digest[65];
    size_t n = app_control_trust_principals(legacy, digest);
    unsigned count = 0;
    for (size_t i = 0; i < n; ++i) {
        out[count] = (access_record_t){.principal = legacy[i], .legacy = true,
            .permissions = permissions(legacy[i].scopes), .duration = APP_CONTROL_ACCESS_PERMANENT};
        snprintf(out[count].label, sizeof(out[count].label), "Native/API client %u", (unsigned)i + 1);
        ++count;
    }
    portENTER_CRITICAL(&access_lock);
    bool valid = access_state && access_state->known && !access_state->uncertain &&
        digest[0] && !strcmp(digest, access_state->trust_digest);
    if (!valid) count = 0;
    else for (unsigned i = 0; i < access_state->count; ++i) {
        const access_record_t *r = &access_state->saved[i];
        unsigned slot = count;
        for (unsigned j = 0; j < count; ++j) if (same(out[j].principal.id, r->principal.id, 16)) slot = j;
        if (slot < count && !same(out[slot].principal.credential_sha256, r->principal.credential_sha256, 32)) {
            valid = false; count = 0; break; /* Reused IDs cannot transfer a saved authorization. */
        }
        if (r->legacy && slot == count) { valid = false; count = 0; break; }
        if (slot == count) { if (count == CP_MAX_PRINCIPALS) { valid = false; count = 0; break; } ++count; }
        bool native = slot < n ? out[slot].principal.native_only : false;
        out[slot] = *r;
        out[slot].principal.native_only = native;
    }
    if (access_state && access_state->processing) for (unsigned i = 0; i < count; ++i)
        if (same(out[i].principal.id, access_state->request.client_id, 16)) out[i].suspended = true;
    if (revision) *revision = access_state ? access_state->revision : 0;
    if (coherent) *coherent = valid;
    portEXIT_CRITICAL(&access_lock);
    return count;
}
static unsigned records(access_record_t out[CP_MAX_PRINCIPALS])
{
    return records_version(out, NULL, NULL);
}
static void pack(uint8_t *bank, const access_record_t *r, unsigned count, uint64_t revision, const char *digest)
{
    memset(bank, 0, BANK_BYTES); memcpy(bank, "ACG1", 4); put64(bank + 4, revision);
    memcpy(bank + 48, digest, 64); bank[112] = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *p = bank + 113 + i * RECORD_BYTES;
        memcpy(p, r[i].principal.id, 16); memcpy(p + 16, r[i].principal.credential_sha256, 32);
        memcpy(p + 48, r[i].label, 33); p[81] = r[i].permissions; p[82] = (uint8_t)r[i].duration;
        p[83] = (uint8_t)((r[i].legacy ? 1 : 0) | (r[i].principal.enabled ? 2 : 0) | (r[i].expired ? 4 : 0));
        put64(p + 84, r[i].expires); put64(p + 92, r[i].revision);
    }
    uint8_t hash[32]; cp_sha256(bank, BANK_BYTES, hash); memcpy(bank + 16, hash, 32);
}
static bool unpack(uint8_t *bank, access_record_t *out, unsigned *count, uint64_t *revision)
{
    uint8_t expected[32], actual[32]; memcpy(expected, bank + 16, 32); memset(bank + 16, 0, 32);
    cp_sha256(bank, BANK_BYTES, actual); memcpy(bank + 16, expected, 32);
    if (memcmp(bank, "ACG1", 4) || !same(expected, actual, 32) ||
        bank[112] > CP_MAX_PRINCIPALS || !get64(bank + 4)) return false;
    *count = bank[112]; *revision = get64(bank + 4);
    for (unsigned i = 0; i < *count; ++i) {
        const uint8_t *p = bank + 113 + i * RECORD_BYTES;
        out[i] = (access_record_t){.permissions = p[81], .duration = (app_control_access_duration_t)p[82],
            .legacy = (p[83] & 1) != 0, .expired = (p[83] & 4) != 0, .expires = get64(p+84), .revision = get64(p+92)};
        memcpy(out[i].principal.id, p, 16); memcpy(out[i].principal.credential_sha256, p+16, 32);
        memcpy(out[i].label, p+48, 33); out[i].principal.enabled = (p[83] & 2) != 0;
        out[i].principal.scopes = scopes(out[i].permissions);
        if (!label_valid(out[i].label) || p[81] > 7 || p[82] > APP_CONTROL_ACCESS_PERMANENT || p[83] > 7 || (out[i].expired && out[i].duration == APP_CONTROL_ACCESS_PERMANENT) ||
            !out[i].revision || (out[i].principal.enabled && !out[i].permissions) ||
            (out[i].duration == APP_CONTROL_ACCESS_PERMANENT ? out[i].expires != 0 : out[i].expires == 0)) return false;
        uint8_t nonzero = 0; for (unsigned j = 0; j < 16; ++j) nonzero |= p[j]; if (!nonzero) return false;
        nonzero = 0; for (unsigned j = 16; j < 48; ++j) nonzero |= p[j]; if (!nonzero) return false;
        for (unsigned j = 0; j < i; ++j) if (same(out[i].principal.id, out[j].principal.id, 16)) return false;
    }
    return true;
}
esp_err_t app_control_access_init(void)
{
    app_control_trust_snapshot_t trust; app_control_trust_snapshot(&trust);
    if (!access_state) access_state = heap_caps_aligned_calloc(_Alignof(access_t), 1, sizeof(access_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!access_state) access_state = heap_caps_aligned_calloc(_Alignof(access_t), 1, sizeof(access_t), MALLOC_CAP_8BIT);
    if (!access_state) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&access_lock);
    uint64_t previous_revision = access_state->revision;
    struct { uint8_t id[16]; uint64_t revision; } expired[CP_MAX_PRINCIPALS];
    unsigned expired_count = 0;
    if (!strcmp(access_state->trust_digest, trust.active_digest))
        for (unsigned i = 0; i < access_state->count; ++i) if (access_state->saved[i].expired) {
            memcpy(expired[expired_count].id, access_state->saved[i].principal.id, 16);
            expired[expired_count++].revision = access_state->saved[i].revision;
        }
    access_state->known = false; access_state->uncertain = true;
    if (access_state->queued || access_state->processing) {
        access_state->receipt_state = CP_RESULT_FAILED; access_state->receipt_error = CP_STALE;
        access_state->queued = access_state->processing = false; zero(&access_state->request, sizeof(access_state->request));
    }
    portEXIT_CRITICAL(&access_lock);
    if (!trust.known || !trust.present || trust.uncertain) {
        portENTER_CRITICAL(&access_lock); access_state->error = ESP_ERR_INVALID_STATE; portEXIT_CRITICAL(&access_lock);
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open_from_partition("control", "ctrl_access", NVS_READWRITE, &nvs);
    unsigned best_count = 0; uint64_t best = 0;
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 2; ++i) {
            size_t size = BANK_BYTES;
            esp_err_t read = nvs_get_blob(nvs, i ? "access_b" : "access_a", access_state->bank, &size);
            if (read == ESP_ERR_NVS_NOT_FOUND) continue;
            unsigned count; uint64_t revision;
            if (read != ESP_OK || size != BANK_BYTES ||
                !unpack(access_state->bank, access_state->candidate, &count, &revision)) {
                error = read != ESP_OK ? read : ESP_ERR_INVALID_CRC; break;
            }
            if (revision > best) {
                best = revision;
                /* A new locally enrolled trust package starts with its own native
                 * principals. Old browser credentials/grants never cross it. */
                best_count = memcmp(access_state->bank + 48, trust.active_digest, 64) ? 0 : count;
                memcpy(access_state->verify, access_state->bank, BANK_BYTES);
            }
        }
        nvs_close(nvs);
    }
    if (error == ESP_OK && best_count) {
        unsigned count; uint64_t generation;
        if (!unpack(access_state->verify, access_state->candidate, &count, &generation)) error = ESP_ERR_INVALID_CRC;
    }
    portENTER_CRITICAL(&access_lock);
    access_state->known = error == ESP_OK; access_state->uncertain = error != ESP_OK; access_state->error = error;
    if (error != ESP_OK) { portEXIT_CRITICAL(&access_lock); return error; }
    if (best_count) memcpy(access_state->saved, access_state->candidate, sizeof(access_state->saved));
    access_state->count = best_count; access_state->expiry_pending = false;
    for (unsigned i = 0; i < best_count; ++i) for (unsigned j = 0; j < expired_count; ++j)
        if (access_state->saved[i].revision == expired[j].revision &&
            same(access_state->saved[i].principal.id, expired[j].id, 16) && !access_state->saved[i].expired) {
            access_state->saved[i].expired = true; access_state->expiry_pending = true;
        }
    access_state->storage_generation = best; access_state->revision = best ? best : 1;
    for (unsigned i = 0; i < best_count; ++i) if (access_state->saved[i].revision > access_state->revision)
        access_state->revision = access_state->saved[i].revision;
    if (previous_revision >= access_state->revision && previous_revision != UINT64_MAX)
        access_state->revision = previous_revision + 1;
    access_state->queued = false; access_state->pending_id = 0; zero(&access_state->proposal, sizeof(access_state->proposal));
    memcpy(access_state->trust_digest, trust.active_digest, 65);
    portEXIT_CRITICAL(&access_lock);
    return error;
}
bool app_control_access_principal(const uint8_t id[16], cp_principal_t *out)
{
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records(list);
    uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
    for (unsigned i = 0; i < count; ++i) if (same(id, list[i].principal.id, 16)) {
        *out = list[i].principal; out->enabled = usable(&list[i], clock, wall); return true;
    }
    return false;
}
bool app_control_access_managed(const uint8_t id[16])
{
    bool managed = false; portENTER_CRITICAL(&access_lock);
    if (access_state && access_state->known) for (unsigned i = 0; i < access_state->count; ++i)
        if (same(id, access_state->saved[i].principal.id, 16)) managed = true;
    portEXIT_CRITICAL(&access_lock); return managed;
}
bool app_control_access_input(const cp_identity_t *identity)
{
    if (!identity || !(identity->scopes & CP_SCOPE_INPUT_CONTROL)) return false;
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records(list);
    uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
    for (unsigned i = 0; i < count; ++i) if (list[i].revision &&
        same(identity->principal_id, list[i].principal.id, 16) &&
        (list[i].permissions & APP_CONTROL_ACCESS_INPUT) && usable(&list[i], clock, wall)) return true;
    return false;
}
bool app_control_access_any_input(void)
{
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records(list);
    uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
    for (unsigned i = 0; i < count; ++i) if (list[i].revision &&
        (list[i].permissions & APP_CONTROL_ACCESS_INPUT) && usable(&list[i], clock, wall)) return true;
    return false;
}
static void describe(const access_record_t *r, app_control_access_client_t *v, bool known, uint64_t wall)
{
    *v = (app_control_access_client_t){.permissions = r->permissions, .duration = r->duration,
        .expires_unix_seconds = r->expires, .revision = r->revision, .legacy = r->legacy && !r->revision,
        .enabled = usable(r, known, wall), .revoked = !r->principal.enabled,
        .clock_unknown = !r->expired && r->duration != APP_CONTROL_ACCESS_PERMANENT && !known,
        .expired = r->expired || (r->duration != APP_CONTROL_ACCESS_PERMANENT && known && wall >= r->expires)};
    memcpy(v->id, r->principal.id, 16); memcpy(v->label, r->label, sizeof(v->label));
    hex(r->principal.credential_sha256, 32, v->fingerprint);
}
void app_control_access_snapshot(app_control_access_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!access_state) { out->error = ESP_ERR_INVALID_STATE; return; }
    uint64_t wall = 0, now = ticks(); bool known = app_clock_wall_time(&wall);
    uint64_t revision; bool coherent;
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records_version(list, &revision, &coherent);
    portENTER_CRITICAL(&access_lock);
    out->known = access_state->known && coherent && access_state->revision == revision; out->uncertain = access_state->uncertain; out->error = access_state->error;
    out->revision = access_state->revision; out->receipt_request = access_state->receipt;
    out->receipt_state = access_state->receipt_state; out->receipt_error = access_state->receipt_error;
    if (access_state->pending_id && now < access_state->pending_until) {
        out->pending = true; out->pairing_request = access_state->pending_id;
        out->pairing_remaining_ms = (uint32_t)(access_state->pending_until - now);
        describe(&access_state->proposal, &out->proposal, known, wall);
        out->proposal.revoked = false; out->proposal.expired = false;
    }
    portEXIT_CRITICAL(&access_lock);
    if (!out->known) { count = 0; out->pending = false; }
    out->count = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i) {
        describe(&list[i], &out->clients[i], known, wall);
        out->clients[i].connected = app_control_server_client_connected(out->clients[i].id);
    }
    app_control_server_snapshot_t server; app_control_server_snapshot(&server);
    snprintf(out->address, sizeof(out->address), "%s", server.address);
}
cp_error_t app_control_access_pair(const char *client, const char *digest, const char *label, uint64_t *request)
{
    access_record_t proposal = {.permissions = APP_CONTROL_ACCESS_INPUT, .duration = APP_CONTROL_ACCESS_SIX_HOURS};
    if (!request || !unhex(client, proposal.principal.id, 16) || !unhex(digest, proposal.principal.credential_sha256, 32) || !label_valid(label)) return CP_INVALID;
    snprintf(proposal.label, sizeof(proposal.label), "%s", label);
    bool coherent;
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records_version(list, NULL, &coherent);
    if (!coherent) return CP_UNAVAILABLE;
    for (unsigned i = 0; i < count; ++i)
        if (same(proposal.principal.id, list[i].principal.id, 16)) return CP_FORBIDDEN;
    uint64_t now = ticks(); portENTER_CRITICAL(&access_lock);
    cp_error_t error = CP_OK;
    if (!access_state || !access_state->known || access_state->uncertain) error = CP_UNAVAILABLE;
    else if (access_state->queued || access_state->processing) error = CP_BUSY;
    else if (access_state->pending_id && now < access_state->pending_until) {
        if (!same(access_state->proposal.principal.id, proposal.principal.id, 16) ||
            !same(access_state->proposal.principal.credential_sha256, proposal.principal.credential_sha256, 32) ||
            strcmp(access_state->proposal.label, label)) error = CP_BUSY;
    } else if (access_state->serial == UINT64_MAX || access_state->revision == UINT64_MAX || access_state->storage_generation == UINT64_MAX) error = CP_UNAVAILABLE;
    else {
        access_state->proposal = proposal; access_state->pending_id = ++access_state->serial;
        access_state->pending_until = now + APP_CONTROL_TRUST_WINDOW_MS; ++access_state->revision;
    }
    if (error == CP_OK) *request = access_state->pending_id;
    portEXIT_CRITICAL(&access_lock); return error;
}
cp_error_t app_control_access_submit(const app_control_access_request_t *r, uint64_t *request)
{
    if (!r || !request || r->action < APP_CONTROL_ACCESS_APPROVE || r->action > APP_CONTROL_ACCESS_REJECT ||
        r->duration < APP_CONTROL_ACCESS_SIX_HOURS || r->duration > APP_CONTROL_ACCESS_PERMANENT ||
        ((r->action == APP_CONTROL_ACCESS_APPROVE || r->action == APP_CONTROL_ACCESS_EDIT) && (!r->permissions || r->permissions > 7))) return CP_INVALID;
    portENTER_CRITICAL(&access_lock);
    cp_error_t error = CP_OK;
    if (!access_state || !access_state->known || access_state->uncertain) error = CP_STORAGE_UNCERTAIN;
    else if (access_state->queued || access_state->processing) error = CP_BUSY;
    else if (r->expected_revision != access_state->revision) error = CP_STALE;
    else if (access_state->serial == UINT64_MAX || access_state->revision == UINT64_MAX || access_state->storage_generation == UINT64_MAX) error = CP_UNAVAILABLE;
    else {
        access_state->request = *r; access_state->queued = true; access_state->receipt = ++access_state->serial;
        access_state->receipt_state = CP_RESULT_ACCEPTED; access_state->receipt_error = CP_OK; *request = access_state->receipt;
    }
    portEXIT_CRITICAL(&access_lock); return error;
}
/* Single management owner; no caller holds access_lock during storage I/O. */
static esp_err_t write_candidate(unsigned count, uint64_t generation, bool *attempted)
{
    pack(access_state->bank, access_state->candidate, count, generation, access_state->trust_digest);
    nvs_handle_t nvs;
    esp_err_t error = nvs_open_from_partition("control", "ctrl_access", NVS_READWRITE, &nvs);
    *attempted = error == ESP_OK;
    if (error == ESP_OK) {
        const char *key = (generation & 1) ? "access_a" : "access_b";
        error = nvs_set_blob(nvs, key, access_state->bank, BANK_BYTES);
        if (error == ESP_OK) error = nvs_commit(nvs);
        size_t size = BANK_BYTES;
        if (error == ESP_OK) error = nvs_get_blob(nvs, key, access_state->verify, &size);
        if (error == ESP_OK && (size != BANK_BYTES || memcmp(access_state->verify, access_state->bank, BANK_BYTES))) error = ESP_ERR_INVALID_CRC;
        nvs_close(nvs);
    }
    return error;
}
/* A reader may latch another expiry during the write. Preserve that RAM latch
 * and persist it on the next service, without changing a locally renewed grant. */
static void publish_candidate(unsigned count, uint64_t storage_generation, uint64_t revision)
{
    bool pending = false;
    for (unsigned i = 0; i < access_state->count; ++i) for (unsigned j = 0; j < count; ++j)
        if (access_state->saved[i].expired && !access_state->candidate[j].expired &&
            access_state->saved[i].revision == access_state->candidate[j].revision &&
            same(access_state->saved[i].principal.id, access_state->candidate[j].principal.id, 16)) {
            access_state->candidate[j].expired = true; pending = true;
        }
    memcpy(access_state->saved, access_state->candidate, sizeof(access_state->saved)); access_state->count = count;
    access_state->storage_generation = storage_generation; access_state->revision = revision;
    access_state->expiry_pending = pending;
}
static void persist_expiry(void)
{
    portENTER_CRITICAL(&access_lock);
    if (!access_state->known || access_state->uncertain || access_state->queued || !access_state->expiry_pending) {
        portEXIT_CRITICAL(&access_lock); return;
    }
    if (access_state->revision == UINT64_MAX || access_state->storage_generation == UINT64_MAX) {
        access_state->known = false; access_state->uncertain = true; access_state->error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&access_lock); return;
    }
    unsigned count = access_state->count;
    uint64_t generation = access_state->storage_generation + 1, revision = access_state->revision + 1;
    access_state->processing = true;
    memcpy(access_state->candidate, access_state->saved, sizeof(access_state->candidate));
    portEXIT_CRITICAL(&access_lock);
    bool attempted;
    esp_err_t error = write_candidate(count, generation, &attempted);
    portENTER_CRITICAL(&access_lock);
    access_state->error = error;
    if (error == ESP_OK) publish_candidate(count, generation, revision);
    else { access_state->known = false; access_state->uncertain = true; }
    access_state->processing = false;
    portEXIT_CRITICAL(&access_lock);
}
void app_control_access_service(bool unlocked)
{
    if (!access_state) { (void)app_control_access_init(); return; }
    app_control_trust_snapshot_t trust; app_control_trust_snapshot(&trust);
    if (trust.present && trust.known && strcmp(trust.active_digest, access_state->trust_digest)) { (void)app_control_access_init(); return; }
    portENTER_CRITICAL(&access_lock);
    if (!access_state->queued) { portEXIT_CRITICAL(&access_lock); persist_expiry(); return; }
    app_control_access_request_t r = access_state->request;
    access_state->queued = false; access_state->processing = true;
    uint64_t generation = access_state->revision + 1;
    unsigned count = access_state->count;
    memcpy(access_state->candidate, access_state->saved, sizeof(access_state->candidate));
    bool proposal = access_state->pending_id && r.pairing_request == access_state->pending_id &&
        ticks() < access_state->pending_until && same(r.client_id, access_state->proposal.principal.id, 16);
    portEXIT_CRITICAL(&access_lock);
    cp_error_t result = CP_OK;
    if (r.expected_revision + 1 != generation) result = CP_STALE;
    if (!unlocked && (r.action == APP_CONTROL_ACCESS_APPROVE || r.action == APP_CONTROL_ACCESS_EDIT)) result = CP_LOCKED;
    if ((r.action == APP_CONTROL_ACCESS_APPROVE || r.action == APP_CONTROL_ACCESS_REJECT) && !proposal) result = CP_STALE;
    if (result == CP_OK && r.action == APP_CONTROL_ACCESS_REJECT) {
        portENTER_CRITICAL(&access_lock); access_state->pending_id = 0; ++access_state->revision; portEXIT_CRITICAL(&access_lock);
    } else if (result == CP_OK) {
        access_record_t list[CP_MAX_PRINCIPALS]; unsigned total = records(list), slot = count;
        for (unsigned i = 0; i < count; ++i) if (same(r.client_id, access_state->candidate[i].principal.id, 16)) slot = i;
        if (slot == count && count == CP_MAX_PRINCIPALS) result = CP_BUSY;
        access_record_t value = {0}; bool found = false;
        if (r.action == APP_CONTROL_ACCESS_APPROVE) {
            value = access_state->proposal; found = true;
            if (total == CP_MAX_PRINCIPALS) result = CP_BUSY;
        } else for (unsigned i = 0; i < total; ++i) if (same(r.client_id, list[i].principal.id, 16)) { value = list[i]; found = true; }
        if (!found) result = CP_STALE;
        uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
        if (r.action != APP_CONTROL_ACCESS_REVOKE && r.duration != APP_CONTROL_ACCESS_PERMANENT && !clock) result = CP_UNAVAILABLE;
        if (result == CP_OK) {
            value.suspended = false;
            if (r.action != APP_CONTROL_ACCESS_REVOKE) value.expired = false;
            value.permissions = r.action == APP_CONTROL_ACCESS_REVOKE ? 0 : r.permissions;
            value.duration = r.action == APP_CONTROL_ACCESS_REVOKE ? value.duration : r.duration;
            value.expires = value.duration == APP_CONTROL_ACCESS_PERMANENT ? 0 :
                r.action == APP_CONTROL_ACCESS_REVOKE ? value.expires : wall + (r.duration == APP_CONTROL_ACCESS_DAY ? 86400U : 21600U);
            value.principal.enabled = r.action != APP_CONTROL_ACCESS_REVOKE;
            value.principal.scopes = scopes(value.permissions); value.revision = generation;
            access_state->candidate[slot] = value; if (slot == count) ++count;
            uint64_t stored_generation = access_state->storage_generation + 1;
            bool attempted;
            esp_err_t error = write_candidate(count, stored_generation, &attempted);
            portENTER_CRITICAL(&access_lock);
            access_state->error = error;
            if (error == ESP_OK) {
                publish_candidate(count, stored_generation, generation);
                access_state->known = true; access_state->uncertain = false;
                if (proposal) access_state->pending_id = 0;
            } else if (attempted) {
                access_state->known = false; access_state->uncertain = true;
            }
            portEXIT_CRITICAL(&access_lock);
            if (error != ESP_OK) result = attempted ? CP_STORAGE_UNCERTAIN : CP_UNAVAILABLE;
        }
    }
    portENTER_CRITICAL(&access_lock);
    access_state->processing = false; access_state->receipt_error = result;
    access_state->receipt_state = result == CP_OK ? CP_RESULT_APPLIED : CP_RESULT_FAILED;
    zero(&access_state->request, sizeof(access_state->request));
    portEXIT_CRITICAL(&access_lock);
}
bool app_control_access_update_policy(const uint8_t id[16], app_update_policy_t *out)
{
    access_record_t list[CP_MAX_PRINCIPALS]; unsigned count = records(list);
    uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
    for (unsigned i = 0; i < count; ++i) if (same(id, list[i].principal.id, 16) && list[i].revision) {
        if (!(list[i].permissions & APP_CONTROL_ACCESS_UPDATE) || !usable(&list[i], clock, wall)) return false;
        *out = (app_update_policy_t){.revision = list[i].revision,
            .expires_unix_seconds = list[i].duration == APP_CONTROL_ACCESS_PERMANENT ? UINT64_MAX : list[i].expires,
            .maximum_job_ms = 600000, .maximum_bytes = APP_UPDATE_SLOT_BYTES,
            .allow_download = true, .allow_activate = true, .allow_restart = true, .allow_signed_releases = true};
        memcpy(out->id, id, 16); memcpy(out->principal_id, id, 16); return true;
    }
    return false;
}
cp_error_t app_control_access_pair_status(const char *client, const char *credential, char *out, size_t capacity, size_t *length)
{
    uint8_t id[16], secret[32], digest[32];
    if (!unhex(client, id, 16) || !unhex(credential, secret, 32)) return CP_INVALID;
    cp_sha256(secret, sizeof(secret), digest); zero(secret, sizeof(secret));
    bool coherent;
    access_record_t list[CP_MAX_PRINCIPALS], value = {0}; unsigned count = records_version(list, NULL, &coherent);
    if (!coherent) { zero(digest, sizeof(digest)); return CP_UNAVAILABLE; }
    const char *status = "unknown"; bool found = false;
    for (unsigned i = 0; i < count; ++i) if (same(id, list[i].principal.id, 16) && same(digest, list[i].principal.credential_sha256, 32)) {
        value = list[i]; found = true;
    }
    uint64_t wall = 0; bool clock = app_clock_wall_time(&wall);
    if (found && value.suspended) return CP_BUSY;
    if (found) status = !value.principal.enabled ? "rejected" : value.expired ? "expired" : value.duration != APP_CONTROL_ACCESS_PERMANENT && !clock ? "clock_unknown" :
        usable(&value, clock, wall) ? "approved" : "expired";
    uint64_t request = 0; bool proposal_match = false;
    portENTER_CRITICAL(&access_lock);
    if (!found && access_state && access_state->known && same(id, access_state->proposal.principal.id, 16) &&
        same(digest, access_state->proposal.principal.credential_sha256, 32)) {
        value = access_state->proposal; request = access_state->pending_id; proposal_match = true;
        status = request && ticks() < access_state->pending_until ? "pending" : "rejected";
    }
    portEXIT_CRITICAL(&access_lock);
    if (!found && !proposal_match) return CP_UNAUTHENTICATED;
    char fingerprint[65], expiry[32] = "null", permission_scopes[512]; hex(digest, 32, fingerprint); zero(digest, 32);
    if (value.expires) snprintf(expiry, sizeof(expiry), "\"%" PRIu64 "\"", value.expires);
    cp_scope_mask_t mask = value.principal.scopes;
    size_t used = 0; permission_scopes[used++] = '[';
    for (unsigned bit = 0; bit < 25; ++bit) if (mask & (UINT64_C(1) << bit)) {
        const char *name = cp_scope_name(bit);
        int n = snprintf(permission_scopes + used, sizeof(permission_scopes) - used, "%s\"%s\"", used > 1 ? "," : "", name);
        if (n < 0 || (size_t)n >= sizeof(permission_scopes) - used) return CP_TOO_LARGE;
        used += (size_t)n;
    }
    permission_scopes[used++] = ']'; permission_scopes[used] = 0;
    int n = snprintf(out, capacity, "{\"state\":\"%s\",\"client_id\":\"%s\",\"fingerprint\":\"%s\",\"permissions\":%u,\"scopes\":%s,\"duration\":\"%s\",\"expires_unix_seconds\":%s,\"request_id\":\"%" PRIu64 "\"}",
        status, client, fingerprint, value.permissions, permission_scopes,
        value.duration == APP_CONTROL_ACCESS_PERMANENT ? "permanent" : value.duration == APP_CONTROL_ACCESS_DAY ? "24h" : "6h", expiry, request);
    if (n < 0 || (size_t)n >= capacity) return CP_TOO_LARGE;
    *length = (size_t)n; return CP_OK;
}
