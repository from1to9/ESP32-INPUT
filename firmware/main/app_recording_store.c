// SPDX-License-Identifier: Apache-2.0
#include "app_recording_store.h"
#include "audio_media.h"
#include "board_recording_flash.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

/* Software custody only: no API exports this blob, no eFuse provisioning or
 * physical-tamper claim. Key + volume identity + next nonce are one atomic NVS
 * value. A missing/malformed value on a populated volume never creates a key. */
#define CUSTODY_BYTES 64U
#define CUSTODY_PARTITION "control"
#define CUSTODY_NAMESPACE "record_audio"
#define CUSTODY_KEY "custody_v1"
typedef struct {
    app_recording_store_config_t config;
    ae_store_t *store;
    void *store_memory;
    ae_store_port_t backend;
    app_recording_store_snapshot_t view;
    uint8_t custody[CUSTODY_BYTES];
    ae_id_t volume;
    nvs_handle_t nvs;
    uint16_t limit;
    uint64_t quota;
    void (*notify)(void *);
    void *notify_context;
    bool nvs_open, custody_loaded, custody_fault, board_started, opened, prepare, volume_ready, byte_provision_pending, recovering;
} recording_state_t;
static recording_state_t *state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t read64(const uint8_t *p) { uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n |= (uint64_t)p[i] << (8U * i); return n; }
static void write64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (8U * i)); }
static ae_result_t wall_time(void *context, uint64_t *seconds)
{
    recording_state_t *s = context;
    return s->config.wall_time ? s->config.wall_time(s->config.clock_context, seconds) : AE_TIME_UNKNOWN;
}
static uint64_t clock_ms(void *context) { (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static ae_result_t open_custody(recording_state_t *s)
{
    if (s->custody_fault) return AE_UNCERTAIN;
    if (s->nvs_open) return AE_OK;
    esp_err_t error = nvs_flash_init_partition(CUSTODY_PARTITION);
    if (error != ESP_OK) return AE_UNAVAILABLE; /* Never erase/reinitialize an unknown control partition. */
    error = nvs_open_from_partition(CUSTODY_PARTITION, CUSTODY_NAMESPACE, NVS_READWRITE, &s->nvs);
    if (error != ESP_OK) return AE_UNAVAILABLE;
    s->nvs_open = true; return AE_OK;
}
static ae_result_t persist_custody(recording_state_t *s)
{
    esp_err_t error = nvs_set_blob(s->nvs, CUSTODY_KEY, s->custody, CUSTODY_BYTES);
    if (error == ESP_OK) error = nvs_commit(s->nvs);
    if (error != ESP_OK) { s->custody_fault = true; return AE_UNCERTAIN; }
    uint8_t verified[CUSTODY_BYTES]; size_t bytes = sizeof(verified);
    error = nvs_get_blob(s->nvs, CUSTODY_KEY, verified, &bytes);
    bool exact = error == ESP_OK && bytes == sizeof(verified) && memcmp(s->custody, verified, sizeof(verified)) == 0;
    ae_secure_zero(verified, sizeof(verified));
    if (!exact) { s->custody_fault = true; return AE_UNCERTAIN; }
    return AE_OK;
}
static ae_result_t volume_key(void *context, bool create, ae_id_t *id, uint8_t output[32])
{
    recording_state_t *s = context; ae_result_t r = open_custody(s); if (r != AE_OK) return r;
    if (!s->custody_loaded) {
        size_t bytes = CUSTODY_BYTES;
        esp_err_t error = nvs_get_blob(s->nvs, CUSTODY_KEY, s->custody, &bytes);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            if (!create) return AE_NOT_FOUND;
            memset(s->custody, 0, CUSTODY_BYTES); memcpy(s->custody, "AEKEY001", 8);
            memcpy(s->custody + 8, s->volume.bytes, 16); write64(s->custody + 56, 1);
            if (psa_crypto_init() != PSA_SUCCESS || psa_generate_random(s->custody + 24, 32) != PSA_SUCCESS)
                return AE_UNAVAILABLE;
            r = persist_custody(s); if (r != AE_OK) return r;
        } else if (error != ESP_OK || bytes != CUSTODY_BYTES) { s->custody_fault = true; return AE_INTEGRITY; }
        if (memcmp(s->custody, "AEKEY001", 8) || memcmp(s->custody + 8, s->volume.bytes, 16) || !read64(s->custody + 56)) {
            s->custody_fault = true; return AE_INTEGRITY;
        }
        s->custody_loaded = true;
    }
    *id = s->volume; memcpy(output, s->custody + 24, 32); return AE_OK;
}
static ae_result_t nonce_range(void *context, uint64_t count, uint64_t *first)
{
    recording_state_t *s = context;
    if (!count || !first || !s->custody_loaded || s->custody_fault) return AE_UNAVAILABLE;
    uint64_t current = read64(s->custody + 56);
    if (!current || count > UINT64_MAX - current) return AE_EXHAUSTED;
    write64(s->custody + 56, current + count);
    ae_result_t r = persist_custody(s);
    /* On ambiguous commit the worker is permanently closed; never retry a
     * range from the previous in-memory value. Reboot reloads durable custody. */
    if (r == AE_OK) *first = current;
    return r;
}
static void publish(recording_state_t *s, ae_result_t result)
{
    app_recording_store_snapshot_t view = {.initialized = true, .preparing = s->prepare,
        .byte_region_owned = s->volume_ready, .error = result};
    if (s->store) {
        view.store = ae_store_status(s->store);
        (void)s->backend.capabilities(s->backend.context, &view.capabilities);
    } else view.capabilities = (ae_store_capabilities_t){.storage = AE_STORAGE_RECORDS_PARTITION,
        .implemented = true, .reason = result};
    portENTER_CRITICAL(&lock); s->view = view; portEXIT_CRITICAL(&lock);
}
static ae_result_t service(void *context)
{
    recording_state_t *s = context; if (!s) return AE_UNAVAILABLE;
    ae_result_t r = AE_AGAIN;
    if (!s->board_started) {
        r = board_recording_flash_init();
        if (r == AE_OK) {
            s->board_started = true;
            ae_storage_device_t d = board_recording_flash_device();
            if (d.completion_notify) d.completion_notify(d.context, s->notify, s->notify_context);
        }
        publish(s, r); return r;
    }
    board_recording_flash_port_t bytes = board_recording_flash_port();
    if (!s->opened) {
        r = bytes.open(bytes.context);
        if (r != AE_AGAIN) {
            board_recording_flash_status_t observed = board_recording_flash_status();
            s->opened = observed.state == BOARD_RECORDING_FLASH_OWNED || observed.state == BOARD_RECORDING_FLASH_ERASED ||
                observed.state == BOARD_RECORDING_FLASH_PROVISIONING;
            if (s->opened && r == AE_PRIVACY) r = AE_UNAVAILABLE;
        }
        publish(s, r); return r;
    }
    if (s->byte_provision_pending) {
        r = bytes.provision(bytes.context, s->volume.bytes);
        if (r == AE_OK) s->byte_provision_pending = false;
        publish(s, r); return r;
    }
    board_recording_flash_status_t physical = board_recording_flash_status();
    if (physical.state == BOARD_RECORDING_FLASH_ERASED || physical.state == BOARD_RECORDING_FLASH_PROVISIONING) {
        if (!s->prepare) { publish(s, AE_UNAVAILABLE); return AE_UNAVAILABLE; }
        if (ae_id_empty(&s->volume)) {
            if (physical.state == BOARD_RECORDING_FLASH_PROVISIONING) memcpy(s->volume.bytes, physical.volume_id, AE_ID_BYTES);
            else if (psa_crypto_init() != PSA_SUCCESS || psa_generate_random(s->volume.bytes, AE_ID_BYTES) != PSA_SUCCESS || ae_id_empty(&s->volume)) {
                publish(s, AE_UNAVAILABLE); return AE_UNAVAILABLE;
            }
        }
        s->byte_provision_pending = true;
        r = bytes.provision(bytes.context, s->volume.bytes);
        if (r == AE_OK) s->byte_provision_pending = false;
        publish(s, r); return r;
    }
    if (physical.state != BOARD_RECORDING_FLASH_OWNED) { publish(s, AE_INTEGRITY); return AE_INTEGRITY; }
    if (!s->volume_ready) {
        memcpy(s->volume.bytes, physical.volume_id, AE_ID_BYTES); s->volume_ready = true;
    }
    if (!s->store) {
        ae_storage_device_t device = board_recording_flash_device();
        device.context = s; device.key_protection_verified = true;
        device.key = volume_key; device.nonce_range = nonce_range;
        device.trusted_unix_time = wall_time; device.monotonic_ms = clock_ms;
        r = ae_store_init(s->store_memory, ae_store_bytes(), &device, &s->store);
        if (s->store) s->backend = ae_store_port(s->store);
        publish(s, r); return r;
    }
    r = ae_store_service(s->store);
    if (s->prepare) {
        ae_result_t provisioned = ae_store_provision(s->store, s->limit, s->quota);
        if (provisioned == AE_OK) { s->prepare = false; r = AE_OK; }
        else if (provisioned != AE_AGAIN) r = provisioned;
    }
    publish(s, r); return r;
}
static ae_result_t capabilities(void *context, ae_store_capabilities_t *out)
{
    recording_state_t *s = context; if (!s || !out) return AE_UNAVAILABLE;
    portENTER_CRITICAL(&lock); *out = s->view.capabilities; portEXIT_CRITICAL(&lock); return AE_OK;
}
static ae_result_t provision(void *context, uint16_t limit, uint64_t quota)
{
    recording_state_t *s = context;
    if (!s || !limit || limit > AE_OBJECTS_MAX || quota < 65536 || quota > 512U * 1024U || quota % 4096) return AE_INVALID;
    if (s->prepare) return s->limit == limit && s->quota == quota ? AE_AGAIN : AE_BUSY;
    if (s->store && ae_store_status(s->store).mounted) return ae_store_provision(s->store, limit, quota);
    if (s->view.error != AE_OK && s->view.error != AE_AGAIN && s->view.error != AE_UNAVAILABLE) return s->view.error;
    s->limit = limit; s->quota = quota; s->prepare = true; return AE_AGAIN;
}
#define BACKEND(s, method) ((s) && (s)->store && (s)->backend.method)
static ae_result_t reserve(void *c, const ae_record_metadata_t *m)
{ recording_state_t *s = c; return BACKEND(s, reserve) ? s->backend.reserve(s->backend.context, m) : AE_UNAVAILABLE; }
static ae_result_t append(void *c, const ae_id_t *id, uint64_t offset, const void *data, size_t n, size_t *actual)
{ recording_state_t *s = c; if (actual) *actual = 0; return BACKEND(s, append) ? s->backend.append(s->backend.context, id, offset, data, n, actual) : AE_UNAVAILABLE; }
static ae_result_t checkpoint(void *c, const ae_id_t *id, ae_record_metadata_t *m)
{ recording_state_t *s = c; return BACKEND(s, checkpoint) ? s->backend.checkpoint(s->backend.context, id, m) : AE_UNAVAILABLE; }
static ae_result_t finish(void *c, const ae_record_metadata_t *m, const uint8_t header[44], ae_record_metadata_t *out)
{ recording_state_t *s = c; return BACKEND(s, finish) ? s->backend.finish(s->backend.context, m, header, out) : AE_UNAVAILABLE; }
static ae_result_t get(void *c, const ae_id_t *id, ae_record_metadata_t *m)
{ recording_state_t *s = c; return BACKEND(s, get) ? s->backend.get(s->backend.context, id, m) : AE_UNAVAILABLE; }
static ae_result_t list(void *c, const ae_id_t *id, uint32_t cursor, ae_record_metadata_t *out, size_t n, size_t *count, uint32_t *next)
{ recording_state_t *s = c; return BACKEND(s, list) ? s->backend.list(s->backend.context, id, cursor, out, n, count, next) : AE_UNAVAILABLE; }
static ae_result_t read_wav(void *c, const ae_id_t *id, uint64_t offset, void *data, size_t n, size_t *actual)
{ recording_state_t *s = c; if (actual) *actual = 0; return BACKEND(s, read) ? s->backend.read(s->backend.context, id, offset, data, n, actual) : AE_UNAVAILABLE; }
static ae_result_t remove_object(void *c, const ae_id_t *id, uint64_t revision, bool *revoked, bool *done)
{ recording_state_t *s = c; return BACKEND(s, remove) ? s->backend.remove(s->backend.context, id, revision, revoked, done) : AE_UNAVAILABLE; }
static void completion_notify(void *c, void (*notify)(void *), void *context)
{
    recording_state_t *s = c; if (!s) return;
    s->notify = notify; s->notify_context = context;
    if (s->board_started) {
        ae_storage_device_t d = board_recording_flash_device();
        if (d.completion_notify) d.completion_notify(d.context, notify, context);
    }
}

static ae_result_t recover(void *c)
{
    recording_state_t *s = c; if (!s) return AE_UNAVAILABLE;
    if (s->store && ae_store_status(s->store).mounted) { s->recovering = false; return AE_OK; }
    if (s->recovering) {
        if (s->store && ae_store_status(s->store).storage_uncertain) {
            s->recovering = false; return ae_store_status(s->store).error;
        }
        if (s->view.error == AE_TIME_UNKNOWN || (s->store && ae_store_status(s->store).error == AE_UNAVAILABLE))
            return s->view.error == AE_TIME_UNKNOWN ? AE_TIME_UNKNOWN : AE_UNAVAILABLE;
        return AE_AGAIN;
    }
    if (!s->store || !ae_store_status(s->store).storage_uncertain) return AE_AGAIN;
    /* Explicit reconciliation only, after the manager confirms hardware off.
     * Byte errors have been consumed before the store enters its fault state.
     * Reopen reads ownership and custody; it never reprovisions either. */
    ae_store_deinit(s->store); s->store = NULL; memset(&s->backend, 0, sizeof(s->backend));
    if (s->nvs_open) nvs_close(s->nvs);
    ae_secure_zero(s->custody, sizeof(s->custody));
    s->nvs_open = s->custody_loaded = s->custody_fault = false;
    s->opened = s->volume_ready = s->prepare = s->byte_provision_pending = false;
    s->recovering = true; publish(s, AE_AGAIN); return AE_AGAIN;
}

static ae_result_t cancel_finish(void *c, const ae_id_t *id, uint64_t revision)
{ recording_state_t *s = c; return BACKEND(s, cancel_finish) ? s->backend.cancel_finish(s->backend.context, id, revision) : AE_UNAVAILABLE; }
static void cancel_read(void *c, const ae_id_t *id)
{ recording_state_t *s = c; if (BACKEND(s, cancel_read)) s->backend.cancel_read(s->backend.context, id); }

ae_result_t app_recording_store_init(const app_recording_store_config_t *config)
{
    if (!config || !config->wall_time) return AE_INVALID;
    if (state) return AE_BUSY;
    recording_state_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *memory = s ? heap_caps_calloc(1, ae_store_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!s || !memory) { if (s) heap_caps_free(s); return AE_EXHAUSTED; }
    s->config = *config; s->store_memory = memory; state = s; publish(s, AE_AGAIN); return AE_OK;
}
ae_store_port_t app_recording_store_port(void)
{
    return (ae_store_port_t){.context = state, .capabilities = capabilities, .reserve = reserve, .append = append,
        .checkpoint = checkpoint, .finish = finish, .get = get, .list = list, .read = read_wav, .remove = remove_object,
        .service = service, .provision = provision, .cancel_read = cancel_read, .cancel_finish = cancel_finish, .recover = recover, .completion_notify = completion_notify};
}
bool app_recording_store_snapshot(app_recording_store_snapshot_t *out)
{
    if (!out || !state) return false;
    portENTER_CRITICAL(&lock); *out = state->view; portEXIT_CRITICAL(&lock); return true;
}
