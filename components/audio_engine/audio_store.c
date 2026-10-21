// SPDX-License-Identifier: Apache-2.0
#include "audio_store.h"
#include "audio_media.h"
#include <string.h>
#include "psa/crypto.h"

/* Version 1: authenticated fixed pages, immutable data, alternating manifests.
 * No native structure is persisted. An interrupted unknown metadata write is
 * quarantined rather than interpreted as an older successful deletion. */
#define PAGE AE_STORE_PAGE_BYTES
#define PAYLOAD 4000U
#define AAD 64U
#define NONCE 12U
#define READ_MAX 8192U
#define META_BYTES 256U
#define MAGIC "AEREC001"
#define KIND_SUPER 1U
#define KIND_META 2U
#define KIND_DATA 3U

typedef struct {
    ae_record_metadata_t meta;
    uint64_t sequence, observed_time;
    uint32_t first, pages, committed;
    bool present;
} entry_t;
typedef enum { OP_NONE, OP_FLUSH, OP_RESERVE, OP_CHECKPOINT, OP_FINISH, OP_READ, OP_REMOVE } operation_t;
typedef enum { M_KEY, M_SUPER0, M_SUPER1, M_INDEX, M_VERIFY, M_RECOVER, M_READY,
    M_BLANK, M_SCAN, M_CREATE_KEY, M_WRITE_SUPER0, M_WRITE_SUPER1, M_FAILED } mount_t;
struct ae_store {
    ae_storage_device_t device;
    ae_store_status_t status;
    mbedtls_svc_key_id_t key;
    ae_id_t key_id;
    uint64_t nonce_next, nonce_end, last_time;
    uint64_t quota, read_offset, op_revision;
    uint16_t limit, slot, mount_slot;
    uint32_t total_pages, data_first, scan, verify_page;
    uint64_t verify_bytes;
    mount_t mount;
    operation_t op;
    unsigned phase, write_phase;
    bool op_done, key_loaded, super_seen, hash_active, read_cancelled, read_pending, program_pending;
    bool finish_cancel_pending, finish_cancel_done, automatic_remove;
    uint16_t expiry_cursor;
    uint32_t pending_read_page;
    ae_id_t cancel_id;
    uint64_t cancel_revision;
    ae_result_t op_result;
    entry_t entries[AE_OBJECTS_MAX], pending;
    ae_record_metadata_t requested;
    ae_id_t op_id;
    uint32_t write_target, read_low, read_high, read_page;
    uint16_t cache_size;
    int writer;
    size_t read_size, read_done;
    uint8_t cache[PAYLOAD], plain[PAYLOAD], sealed[PAGE], verify[PAGE], read_data[READ_MAX];
    psa_hash_operation_t hash;
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (i * 8); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8); return v; }
static bool erased(const uint8_t *p) { for (size_t i = 0; i < PAGE; ++i) if (p[i] != 255) return false; return true; }
static bool terminal(const entry_t *e) { return e->meta.state >= AE_RECORD_COMPLETED; }
static bool reclaimed(const entry_t *e) { return e->meta.access_revoked && e->meta.state == AE_RECORD_DELETING; }
static ae_result_t time_now(ae_store_t *s, uint64_t *time)
{
    if (!s->device.trusted_unix_time || s->device.trusted_unix_time(s->device.context, time) != AE_OK ||
        !*time || *time < s->last_time) return AE_TIME_UNKNOWN;
    s->last_time = *time; return AE_OK;
}
static ae_result_t fault(ae_store_t *s, ae_result_t r)
{
    s->status.error = r; s->status.mounted = false; s->status.storage_uncertain = true;
    s->mount = M_FAILED;
    ae_secure_zero(s->cache, PAYLOAD); ae_secure_zero(s->plain, PAYLOAD); ae_secure_zero(s->read_data, READ_MAX);
    s->cache_size = 0;
    if (s->hash_active) { (void)psa_hash_abort(&s->hash); s->hash_active = false; }
    return r;
}
static void totals(ae_store_t *s)
{
    s->status.objects = 0; s->status.used_bytes = s->status.reserved_bytes = 0;
    for (unsigned i = 0; i < s->limit; ++i) {
        const entry_t *e = &s->entries[i];
        if (!e->present || reclaimed(e)) continue;
        ++s->status.objects;
        /* Whole immutable extents remain charged until explicit deletion. */
        if (terminal(e)) s->status.used_bytes += e->meta.reserved_bytes;
        else s->status.reserved_bytes += e->meta.reserved_bytes;
    }
    s->status.quota_bytes = s->quota;
}
static bool geometry(ae_store_t *s, uint16_t limit, uint64_t quota)
{
    bool internal = s->device.storage == AE_STORAGE_RECORDS_PARTITION;
    uint64_t minimum = internal ? 65536 : 1024U * 1024U;
    uint64_t maximum = internal ? 512U * 1024U : 32U * 1024U * 1024U;
    uint64_t reserve = s->device.region_bytes / 10;
    uint64_t floor = internal ? 32768 : 4U * 1024U * 1024U;
    if (reserve < floor) reserve = floor;
    return limit && limit <= AE_OBJECTS_MAX && quota >= minimum && quota <= maximum && quota % PAGE == 0 &&
        s->device.region_bytes > reserve + (2U + 2U * limit) * PAGE &&
        quota <= s->device.region_bytes - reserve - 2U * limit * PAGE;
}
static void encode_entry(const entry_t *e, uint8_t *p)
{
    const ae_record_metadata_t *m = &e->meta;
    memset(p, 0, PAYLOAD);
    memcpy(p, m->object_id.bytes, 16); memcpy(p + 16, m->principal_id.bytes, 16); memcpy(p + 32, m->consent_id.bytes, 16);
    const uint64_t numbers[] = {m->consent_revision, m->revision, m->created_unix_seconds, m->expires_unix_seconds,
        m->reserved_bytes, m->payload_limit_bytes, m->pcm_bytes, m->stored_bytes, m->verified_pcm_bytes,
        m->synced_pcm_bytes, m->sample_frames, e->sequence};
    for (unsigned i = 0; i < 12; ++i) put64(p + 48 + 8 * i, numbers[i]);
    put32(p + 144, m->format.sample_rate_hz); p[148] = m->format.channels; p[149] = m->format.frame_ms;
    p[150] = (uint8_t)m->format.encoding; p[151] = (uint8_t)m->state; p[152] = (uint8_t)m->storage;
    p[153] = (uint8_t)m->export_policy; p[154] = (uint8_t)m->reason;
    p[155] = (m->hash_available ? 1 : 0) | (m->integrity_verified ? 2 : 0) |
        (m->sync_acknowledged ? 4 : 0) | (m->access_revoked ? 8 : 0);
    memcpy(p + 156, m->sha256, 32);
    put32(p + 188, e->first); put32(p + 192, e->pages); put32(p + 196, e->committed); put64(p + 200, e->observed_time);
}
static bool decode_entry(ae_store_t *s, entry_t *e, const uint8_t *p)
{
    memset(e, 0, sizeof(*e)); ae_record_metadata_t *m = &e->meta;
    memcpy(m->object_id.bytes, p, 16); memcpy(m->principal_id.bytes, p + 16, 16); memcpy(m->consent_id.bytes, p + 32, 16);
    uint64_t *numbers[] = {&m->consent_revision, &m->revision, &m->created_unix_seconds, &m->expires_unix_seconds,
        &m->reserved_bytes, &m->payload_limit_bytes, &m->pcm_bytes, &m->stored_bytes, &m->verified_pcm_bytes,
        &m->synced_pcm_bytes, &m->sample_frames, &e->sequence};
    for (unsigned i = 0; i < 12; ++i) *numbers[i] = get64(p + 48 + 8 * i);
    m->format = (ae_format_t){.sample_rate_hz = get32(p + 144), .channels = p[148], .frame_ms = p[149], .encoding = p[150]};
    m->state = p[151]; m->storage = p[152]; m->export_policy = p[153]; m->reason = p[154];
    m->hash_available = (p[155] & 1) != 0; m->integrity_verified = (p[155] & 2) != 0;
    m->sync_acknowledged = (p[155] & 4) != 0; m->access_revoked = (p[155] & 8) != 0;
    memcpy(m->sha256, p + 156, 32);
    e->first = get32(p + 188); e->pages = get32(p + 192); e->committed = get32(p + 196); e->observed_time = get64(p + 200);
    size_t bytes; uint32_t frames;
    if (ae_format_size(&m->format, &bytes, &frames) != AE_OK || ae_id_empty(&m->object_id) ||
        ae_id_empty(&m->principal_id) || ae_id_empty(&m->consent_id) || !m->consent_revision || !m->revision || !e->sequence ||
        m->storage != s->device.storage || m->state > AE_RECORD_DELETING || m->export_policy > AE_EXPORT_OWNER_DOWNLOAD ||
        m->reason > AE_EXHAUSTED || (p[155] & ~15U) || !m->created_unix_seconds ||
        m->expires_unix_seconds <= m->created_unix_seconds || m->expires_unix_seconds - m->created_unix_seconds < 3600 ||
        m->expires_unix_seconds - m->created_unix_seconds > 2592000 ||
        e->observed_time < m->created_unix_seconds || m->reserved_bytes < 65536 || m->reserved_bytes > s->quota || m->stored_bytes > m->reserved_bytes ||
        m->pcm_bytes > m->payload_limit_bytes || m->verified_pcm_bytes != m->pcm_bytes || m->synced_pcm_bytes != m->pcm_bytes ||
        m->sample_frames != m->pcm_bytes / (2U * m->format.channels) || m->pcm_bytes % bytes ||
        e->first < s->data_first || !e->pages || e->first > s->total_pages || e->pages > s->total_pages - e->first ||
        e->committed > e->pages || m->payload_limit_bytes < bytes || m->payload_limit_bytes > (uint64_t)e->pages * PAYLOAD ||
        m->payload_limit_bytes % bytes || (uint64_t)e->pages * PAGE + 2U * PAGE > m->reserved_bytes ||
        (m->state == AE_RECORD_COMPLETED && (!m->pcm_bytes || !m->hash_available || !m->integrity_verified || !m->sync_acknowledged))) return false;
    for (unsigned i = 208; i < PAYLOAD; ++i) if (p[i]) return false;
    e->present = true; return true;
}
static ae_result_t load_key(ae_store_t *s, bool create)
{
    uint8_t key[32] = {0}; ae_id_t id = {0};
    ae_result_t r = s->device.key(s->device.context, create, &id, key);
    if (r == AE_OK && ae_id_empty(&id)) r = AE_INTEGRITY;
    if (r == AE_OK && psa_crypto_init() != PSA_SUCCESS) r = AE_UNAVAILABLE;
    if (r == AE_OK) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES); psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
        if (psa_import_key(&attributes, key, sizeof(key), &s->key) != PSA_SUCCESS) r = AE_UNAVAILABLE;
        psa_reset_key_attributes(&attributes);
        if (r == AE_OK) { s->key_loaded = true; s->key_id = id; }
    }
    ae_secure_zero(key, sizeof(key)); return r;
}
static ae_result_t seal(ae_store_t *s, unsigned kind, uint16_t slot, uint64_t sequence,
    const ae_id_t *object, uint64_t offset, uint16_t length)
{
    if (s->nonce_next == s->nonce_end) {
        uint64_t first = 0;
        ae_result_t r = s->device.nonce_range(s->device.context, AE_STORE_NONCE_LEASE, &first);
        if (r != AE_OK) return r;
        if (!first || first > UINT64_MAX - AE_STORE_NONCE_LEASE) return AE_EXHAUSTED;
        s->nonce_next = first; s->nonce_end = first + AE_STORE_NONCE_LEASE;
    }
    memset(s->sealed, 0, PAGE); memcpy(s->sealed, MAGIC, 8); s->sealed[8] = 1; s->sealed[9] = (uint8_t)kind;
    put16(s->sealed + 10, slot); put64(s->sealed + 12, sequence); put64(s->sealed + 20, offset); put16(s->sealed + 28, length);
    if (object) memcpy(s->sealed + 32, object->bytes, 16);
    memcpy(s->sealed + 48, s->key_id.bytes, 16); put64(s->sealed + AAD + 4, s->nonce_next++);
    size_t written = 0;
    if (psa_aead_encrypt(s->key, PSA_ALG_GCM, s->sealed + AAD, NONCE, s->sealed, AAD,
        s->plain, PAYLOAD, s->sealed + AAD + NONCE, PAYLOAD + 16, &written) != PSA_SUCCESS || written != PAYLOAD + 16)
        return AE_INTEGRITY;
    s->write_phase = 1; return AE_OK;
}
static ae_result_t unseal(ae_store_t *s, const uint8_t *page, unsigned kind, uint16_t slot, const ae_id_t *object)
{
    if (memcmp(page, MAGIC, 8) || page[8] != 1 || page[9] != kind || get16(page + 10) != slot ||
        memcmp(page + 48, s->key_id.bytes, 16) || (object && memcmp(page + 32, object->bytes, 16)) ||
        get16(page + 28) > PAYLOAD || page[30] || page[31] || get32(page + 4092)) return AE_INTEGRITY;
    size_t written = 0;
    if (psa_aead_decrypt(s->key, PSA_ALG_GCM, page + AAD, NONCE, page, AAD,
        page + AAD + NONCE, PAYLOAD + 16, s->plain, PAYLOAD, &written) != PSA_SUCCESS || written != PAYLOAD)
        return AE_INTEGRITY;
    return AE_OK;
}
static ae_result_t read_page(ae_store_t *s, uint32_t page)
{
    if (page >= s->total_pages) return AE_INTEGRITY;
    ae_result_t r = s->device.read(s->device.context, (uint64_t)page * PAGE, s->verify, PAGE);
    s->read_pending = r == AE_AGAIN; s->pending_read_page = page; return r;
}
static ae_result_t write_step(ae_store_t *s)
{
    ae_result_t r; uint64_t offset = (uint64_t)s->write_target * PAGE;
    switch (s->write_phase) {
    case 1: r = s->device.erase(s->device.context, offset, PAGE); break;
    case 2: r = s->device.program(s->device.context, offset, s->sealed, PAGE); s->program_pending = r == AE_AGAIN; break;
    case 3: r = s->device.sync(s->device.context); break;
    case 4:
        r = s->device.read(s->device.context, offset, s->verify, PAGE);
        if (r == AE_OK && memcmp(s->sealed, s->verify, PAGE)) r = AE_INTEGRITY;
        break;
    default: return AE_INVALID;
    }
    if (r == AE_AGAIN) return r;
    if (r != AE_OK) return fault(s, r);
    if (++s->write_phase == 5) { s->write_phase = 0; return AE_OK; }
    return AE_AGAIN;
}
static ae_result_t journal_step(ae_store_t *s)
{
    if (!s->write_phase) {
        if (s->last_time < s->pending.observed_time) return AE_TIME_UNKNOWN;
        s->pending.observed_time = s->last_time;
        encode_entry(&s->pending, s->plain);
        s->write_target = 2U + 2U * s->slot + (uint32_t)(s->pending.sequence & 1U);
        ae_result_t r = seal(s, KIND_META, s->slot, s->pending.sequence, &s->pending.meta.object_id, 0, META_BYTES);
        if (r != AE_OK) return r;
    }
    ae_result_t r = write_step(s);
    if (r == AE_OK) { s->entries[s->slot] = s->pending; totals(s); }
    return r;
}
static ae_result_t hash_begin(ae_store_t *s, const entry_t *e)
{
    uint8_t header[44];
    if (ae_wav_header(&e->meta.format, e->meta.pcm_bytes, header) != AE_OK) return AE_INTEGRITY;
    s->hash = psa_hash_operation_init();
    if (psa_hash_setup(&s->hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return AE_INTEGRITY;
    s->hash_active = true;
    if (psa_hash_update(&s->hash, header, sizeof(header)) != PSA_SUCCESS) return AE_INTEGRITY;
    s->verify_page = 0; s->verify_bytes = 0; return AE_OK;
}
static ae_result_t hash_step(ae_store_t *s, const entry_t *e, uint16_t slot, uint8_t digest[32])
{
    if (s->verify_page < e->committed) {
        ae_result_t r = read_page(s, e->first + s->verify_page);
        if (r != AE_OK) return r;
        r = unseal(s, s->verify, KIND_DATA, slot, &e->meta.object_id);
        size_t length = get16(s->verify + 28);
        if (r != AE_OK || !length || length > e->meta.pcm_bytes - s->verify_bytes ||
            get64(s->verify + 20) != s->verify_bytes || get64(s->verify + 12) != s->verify_page + 1U ||
            length % (e->meta.format.channels * 2U)) return AE_INTEGRITY;
        if (psa_hash_update(&s->hash, s->plain, length) != PSA_SUCCESS) return AE_INTEGRITY;
        s->verify_bytes += length; ++s->verify_page; return AE_AGAIN;
    }
    size_t size = 0;
    if (s->verify_bytes != e->meta.pcm_bytes || psa_hash_finish(&s->hash, digest, 32, &size) != PSA_SUCCESS || size != 32)
        return AE_INTEGRITY;
    s->hash_active = false; return AE_OK;
}

size_t ae_store_bytes(void) { return sizeof(ae_store_t); }
void ae_store_deinit(ae_store_t *s)
{
    if (!s) return;
    if (s->hash_active) (void)psa_hash_abort(&s->hash);
    if (s->key_loaded) (void)psa_destroy_key(s->key);
    ae_secure_zero(s, sizeof(*s));
}
ae_result_t ae_store_init(void *memory, size_t bytes, const ae_storage_device_t *d, ae_store_t **out)
{
    if (!memory || bytes < sizeof(ae_store_t) || (uintptr_t)memory % _Alignof(ae_store_t) || !d || !out ||
        !d->read || !d->program || !d->erase || !d->sync || !d->key || !d->nonce_range ||
        !d->geometry_verified || !d->explicitly_owned || !d->key_protection_verified || d->erase_bytes != PAGE ||
        d->region_bytes % PAGE || d->region_bytes / PAGE > UINT32_MAX || d->region_bytes < 65536 ||
        (d->storage != AE_STORAGE_RECORDS_PARTITION && d->storage != AE_STORAGE_NAND)) return AE_UNAVAILABLE;
    ae_store_t *s = memory; memset(s, 0, sizeof(*s)); s->device = *d; s->total_pages = (uint32_t)(d->region_bytes / PAGE);
    s->status.initialized = true; s->status.recovery_pending = true; s->mount = M_KEY; s->writer = -1;
    *out = s; return AE_AGAIN;
}
ae_store_status_t ae_store_status(const ae_store_t *s)
{
    return s ? s->status : (ae_store_status_t){.error = AE_UNAVAILABLE};
}
ae_result_t ae_store_provision(ae_store_t *s, uint16_t limit, uint64_t quota)
{
    if (!s || !geometry(s, limit, quota)) return AE_INVALID;
    if (s->status.mounted) return s->limit == limit && s->quota == quota ? AE_OK : AE_BUSY;
    if (s->status.provisioning) return s->limit == limit && s->quota == quota ? AE_AGAIN : AE_BUSY;
    if (s->mount != M_BLANK) return s->mount == M_FAILED ? s->status.error : AE_AGAIN;
    s->limit = limit; s->quota = quota; s->data_first = 2U + 2U * limit; s->scan = 0;
    s->status.provisioning = true; s->mount = M_SCAN; return AE_AGAIN;
}
static ae_result_t mount_step(ae_store_t *s)
{
    ae_result_t r; uint64_t now;
    switch (s->mount) {
    case M_KEY:
        r = load_key(s, false);
        if (r == AE_AGAIN) return r;
        if (r == AE_NOT_FOUND || r == AE_UNAVAILABLE) { s->mount = M_BLANK; s->status.error = AE_UNAVAILABLE; return AE_UNAVAILABLE; }
        if (r != AE_OK) return fault(s, r);
        s->mount = M_SUPER0; return AE_AGAIN;
    case M_SUPER0: case M_SUPER1: {
        unsigned page = s->mount == M_SUPER0 ? 0 : 1;
        r = read_page(s, page); if (r != AE_OK) return r;
        if (!erased(s->verify)) {
            r = unseal(s, s->verify, KIND_SUPER, 0, NULL);
            uint16_t limit = get16(s->plain); uint64_t quota = get64(s->plain + 8);
            if (r != AE_OK || get64(s->plain + 16) != s->device.region_bytes || s->plain[24] != s->device.storage ||
                !geometry(s, limit, quota) || (s->super_seen && (s->limit != limit || s->quota != quota))) return fault(s, AE_INTEGRITY);
            s->limit = limit; s->quota = quota; s->data_first = 2U + 2U * limit; s->super_seen = true;
        }
        if (!page) { s->mount = M_SUPER1; return AE_AGAIN; }
        if (!s->super_seen) { s->mount = M_BLANK; s->status.error = AE_UNAVAILABLE; return AE_UNAVAILABLE; }
        s->scan = 0; s->mount = M_INDEX; return AE_AGAIN;
    }
    case M_INDEX:
        if (s->scan < 2U * s->limit) {
            r = read_page(s, 2U + s->scan); if (r != AE_OK) return r;
            uint16_t slot = (uint16_t)(s->scan / 2U);
            if (!erased(s->verify)) {
                r = unseal(s, s->verify, KIND_META, slot, NULL);
                if (r != AE_OK || !decode_entry(s, &s->pending, s->plain) ||
                    get64(s->verify + 12) != s->pending.sequence || (s->pending.sequence & 1U) != (s->scan & 1U) ||
                    memcmp(s->verify + 32, s->pending.meta.object_id.bytes, 16)) return fault(s, AE_INTEGRITY);
                if (!s->entries[slot].present || s->pending.sequence > s->entries[slot].sequence) s->entries[slot] = s->pending;
            }
            ++s->scan; return AE_AGAIN;
        }
        totals(s);
        if (s->status.used_bytes > s->quota || s->status.reserved_bytes > s->quota - s->status.used_bytes) return fault(s, AE_QUOTA);
        for (unsigned i = 0; i < s->limit; ++i) for (unsigned j = i + 1; j < s->limit; ++j) {
            entry_t *a = &s->entries[i], *b = &s->entries[j];
            if (!a->present || !b->present || reclaimed(a) || reclaimed(b)) continue;
            if (ae_id_equal(&a->meta.object_id, &b->meta.object_id) ||
                (a->first < b->first + b->pages && b->first < a->first + a->pages)) return fault(s, AE_INTEGRITY);
        }
        s->mount_slot = 0; s->phase = 0; s->mount = M_VERIFY; return AE_AGAIN;
    case M_VERIFY:
        if (time_now(s, &now) != AE_OK) return AE_TIME_UNKNOWN;
        if (s->mount_slot == s->limit) {
            s->mount = M_READY; s->status.mounted = true; s->status.recovery_pending = false;
            s->status.error = AE_OK; totals(s); return AE_OK;
        }
        if (s->entries[s->mount_slot].present && now < s->entries[s->mount_slot].observed_time) return AE_TIME_UNKNOWN;
        if (!s->entries[s->mount_slot].present || reclaimed(&s->entries[s->mount_slot])) { ++s->mount_slot; return AE_AGAIN; }
        if (!s->phase) {
            r = hash_begin(s, &s->entries[s->mount_slot]); if (r != AE_OK) return fault(s, r);
            s->phase = 1; return AE_AGAIN;
        } else {
            uint8_t digest[32]; entry_t *e = &s->entries[s->mount_slot];
            r = hash_step(s, e, s->mount_slot, digest); if (r == AE_AGAIN) return r;
            if (r != AE_OK || (e->meta.hash_available && memcmp(e->meta.sha256, digest, 32))) return fault(s, AE_INTEGRITY);
            if (!terminal(e) || now >= e->meta.expires_unix_seconds) {
                s->pending = *e; s->slot = s->mount_slot;
                if (e->sequence == UINT64_MAX || e->meta.revision == UINT64_MAX) return fault(s, AE_EXHAUSTED);
                ++s->pending.sequence; ++s->pending.meta.revision;
                s->pending.meta.state = e->meta.pcm_bytes ? AE_RECORD_PARTIAL : AE_RECORD_ERROR;
                s->pending.meta.reason = AE_UNCERTAIN; s->pending.meta.hash_available = true;
                memcpy(s->pending.meta.sha256, digest, 32);
                if (now >= e->meta.expires_unix_seconds) { s->pending.meta.access_revoked = true; s->pending.meta.state = AE_RECORD_DELETING; }
                s->mount = M_RECOVER; return AE_AGAIN;
            }
            ++s->mount_slot; s->phase = 0; return AE_AGAIN;
        }
    case M_RECOVER:
        r = journal_step(s); if (r != AE_OK) return r;
        ++s->mount_slot; s->phase = 0; s->mount = M_VERIFY; return AE_AGAIN;
    case M_SCAN:
        if (s->scan < s->total_pages) {
            r = read_page(s, s->scan); if (r != AE_OK) return r;
            if (!erased(s->verify)) return fault(s, AE_INTEGRITY);
            ++s->scan; return AE_AGAIN;
        }
        s->mount = M_CREATE_KEY; return AE_AGAIN;
    case M_CREATE_KEY:
        r = s->key_loaded ? AE_OK : load_key(s, true); if (r == AE_AGAIN) return r;
        if (r != AE_OK) return fault(s, r);
        s->mount = M_WRITE_SUPER0; return AE_AGAIN;
    case M_WRITE_SUPER0: case M_WRITE_SUPER1:
        if (!s->write_phase) {
            memset(s->plain, 0, PAYLOAD); put16(s->plain, s->limit); put64(s->plain + 8, s->quota);
            put64(s->plain + 16, s->device.region_bytes); s->plain[24] = (uint8_t)s->device.storage;
            s->write_target = s->mount == M_WRITE_SUPER0 ? 0 : 1;
            r = seal(s, KIND_SUPER, 0, 1, NULL, 0, 32); if (r != AE_OK) return r;
        }
        r = write_step(s); if (r != AE_OK) return r;
        if (s->mount == M_WRITE_SUPER0) { s->mount = M_WRITE_SUPER1; return AE_AGAIN; }
        s->mount = M_READY; s->status.provisioning = s->status.recovery_pending = false;
        s->status.mounted = true; s->status.error = AE_OK; totals(s); return AE_OK;
    case M_BLANK: return AE_UNAVAILABLE;
    case M_FAILED: return s->status.error;
    case M_READY: return AE_OK;
    }
    return AE_INVALID;
}

static int find(ae_store_t *s, const ae_id_t *id)
{
    for (unsigned i = 0; i < s->limit; ++i) if (s->entries[i].present && ae_id_equal(id, &s->entries[i].meta.object_id)) return (int)i;
    return -1;
}
static ae_result_t ready(ae_store_t *s)
{
    if (!s || !s->status.mounted) return s && s->status.storage_uncertain ? s->status.error : AE_UNAVAILABLE;
    uint64_t now; return time_now(s, &now);
}
static ae_result_t live_entry(ae_store_t *s, const ae_id_t *id, int *index, bool allow_revoked)
{
    ae_result_t r = ready(s); if (r != AE_OK) return r;
    *index = find(s, id); if (*index < 0) return AE_NOT_FOUND;
    entry_t *e = &s->entries[*index];
    if (!allow_revoked && (e->meta.access_revoked || s->last_time >= e->meta.expires_unix_seconds)) return AE_DENIED;
    return AE_OK;
}
static ae_result_t flush_step(ae_store_t *s)
{
    if (!s->cache_size) return AE_OK;
    entry_t *e = &s->entries[s->writer];
    if (e->committed >= e->pages) return AE_QUOTA;
    if (!s->write_phase) {
        memset(s->plain, 0, PAYLOAD); memcpy(s->plain, s->cache, s->cache_size);
        s->write_target = e->first + e->committed;
        ae_result_t r = seal(s, KIND_DATA, (uint16_t)s->writer, e->committed + 1U, &e->meta.object_id,
            e->meta.pcm_bytes - s->cache_size, s->cache_size);
        if (r != AE_OK) return r;
    }
    ae_result_t r = write_step(s);
    if (r == AE_OK) { ++e->committed; ae_secure_zero(s->cache, PAYLOAD); s->cache_size = 0; }
    return r;
}
static ae_result_t operation_step(ae_store_t *s)
{
    ae_result_t r = AE_OK;
    if (s->op == OP_READ && s->op_done && s->read_cancelled) {
        ae_secure_zero(s->read_data, READ_MAX); s->op = OP_NONE; s->op_done = s->read_cancelled = false;
    }
    if (s->op == OP_NONE) {
        uint64_t now;
        if (time_now(s, &now) != AE_OK) return AE_TIME_UNKNOWN;
        unsigned slot = s->expiry_cursor++ % s->limit;
        entry_t *e = &s->entries[slot];
        if (e->present && terminal(e) && !reclaimed(e) && now >= e->meta.expires_unix_seconds) {
            if (e->sequence == UINT64_MAX || e->meta.revision == UINT64_MAX) return AE_EXHAUSTED;
            s->pending = *e; ++s->pending.sequence; ++s->pending.meta.revision;
            s->pending.meta.access_revoked = true; s->pending.meta.state = AE_RECORD_DELETING;
            s->pending.meta.hash_available = false; memset(s->pending.meta.sha256, 0, 32);
            s->op = OP_REMOVE; s->op_done = false; s->phase = 0; s->slot = (uint16_t)slot;
            s->op_id = e->meta.object_id; s->op_revision = e->meta.revision; s->automatic_remove = true;
        } else return AE_OK;
    }
    if (s->op_done) return AE_OK;
    if (s->op == OP_FINISH && s->finish_cancel_pending) {
        if (s->read_pending) { r = read_page(s, s->pending_read_page); if (r != AE_OK) return r; }
        if (s->write_phase == 1) {
            r = s->device.erase(s->device.context, (uint64_t)s->write_target * PAGE, PAGE);
            if (r != AE_OK) return r;
        }
        if (s->hash_active) { (void)psa_hash_abort(&s->hash); s->hash_active = false; }
        s->write_phase = s->phase = 0; s->op = OP_NONE; s->op_done = false;
        s->finish_cancel_pending = false; s->finish_cancel_done = true;
        ae_secure_zero(s->plain, PAYLOAD); ae_secure_zero(s->sealed, PAGE); return AE_OK;
    }
    if (s->op == OP_FLUSH) r = flush_step(s);
    else if (s->op == OP_RESERVE || s->op == OP_REMOVE) r = journal_step(s);
    else if (s->op == OP_CHECKPOINT) {
        if (!s->phase) {
            r = flush_step(s); if (r != AE_OK) return r;
            s->pending = s->entries[s->slot];
            if (s->pending.sequence == UINT64_MAX) return AE_EXHAUSTED;
            ++s->pending.sequence;
            s->pending.meta.verified_pcm_bytes = s->pending.meta.synced_pcm_bytes = s->pending.meta.pcm_bytes;
            s->pending.meta.sample_frames = s->pending.meta.pcm_bytes / (s->pending.meta.format.channels * 2U);
            s->pending.meta.stored_bytes = 2U * PAGE + (uint64_t)s->pending.committed * PAGE;
            s->pending.meta.integrity_verified = s->pending.meta.sync_acknowledged = true;
            s->phase = 1; return AE_AGAIN;
        }
        r = journal_step(s);
    } else if (s->op == OP_FINISH) {
        if (!s->phase) {
            r = hash_begin(s, &s->pending); if (r != AE_OK) return r;
            s->phase = 1; return AE_AGAIN;
        }
        if (s->phase == 1) {
            r = hash_step(s, &s->pending, s->slot, s->pending.meta.sha256);
            if (r != AE_OK) return r;
            s->pending.meta.hash_available = true; s->phase = 2; return AE_AGAIN;
        }
        r = journal_step(s);
    } else if (s->op == OP_READ) {
        entry_t *e = &s->entries[s->slot];
        uint64_t offset = s->read_offset + s->read_done;
        if (offset < 44) {
            uint8_t header[44]; if (ae_wav_header(&e->meta.format, e->meta.pcm_bytes, header) != AE_OK) return AE_INTEGRITY;
            size_t n = 44U - (size_t)offset; if (n > s->read_size - s->read_done) n = s->read_size - s->read_done;
            memcpy(s->read_data + s->read_done, header + offset, n); s->read_done += n;
            if (s->read_done == s->read_size) goto completed;
            return AE_AGAIN;
        }
        offset -= 44;
        if (s->read_low >= s->read_high) return AE_INTEGRITY;
        uint32_t page = s->read_low + (s->read_high - s->read_low) / 2;
        r = read_page(s, e->first + page); if (r != AE_OK) return r;
        r = unseal(s, s->verify, KIND_DATA, s->slot, &e->meta.object_id); if (r != AE_OK) return r;
        uint64_t first = get64(s->verify + 20); size_t length = get16(s->verify + 28);
        if (!length || first > e->meta.pcm_bytes || length > e->meta.pcm_bytes - first || get64(s->verify + 12) != page + 1U) return AE_INTEGRITY;
        if (offset < first) s->read_high = page;
        else if (offset >= first + length) s->read_low = page + 1;
        else {
            size_t n = length - (size_t)(offset - first); if (n > s->read_size - s->read_done) n = s->read_size - s->read_done;
            memcpy(s->read_data + s->read_done, s->plain + (size_t)(offset - first), n); s->read_done += n;
            s->read_low = page + 1; s->read_high = e->committed;
            if (s->read_done == s->read_size) goto completed;
        }
        return AE_AGAIN;
    }
    if (r == AE_AGAIN) return r;
completed:
    s->op_done = true; s->op_result = r;
    if (s->op == OP_REMOVE && s->automatic_remove && r == AE_OK) {
        s->op = OP_NONE; s->op_done = s->automatic_remove = false;
    }
    if (s->op == OP_READ && s->read_cancelled && r == AE_OK) {
        ae_secure_zero(s->read_data, READ_MAX); s->op = OP_NONE; s->op_done = s->read_cancelled = false;
    }
    if (r != AE_OK && r != AE_QUOTA) return fault(s, r);
    return r;
}
ae_result_t ae_store_service(ae_store_t *s)
{
    if (!s) return AE_INVALID;
    ae_result_t r = s->mount != M_READY ? mount_step(s) : operation_step(s);
    if (r != AE_OK && r != AE_AGAIN && r != AE_UNAVAILABLE && r != AE_TIME_UNKNOWN && s->mount != M_FAILED)
        return fault(s, r);
    return r;
}
static ae_result_t complete(ae_store_t *s, operation_t op, const ae_id_t *id)
{
    if (s->op == OP_NONE) return AE_NOT_FOUND;
    if (s->op != op || !ae_id_equal(&s->op_id, id)) return AE_AGAIN;
    if (!s->op_done) return AE_AGAIN;
    ae_result_t r = s->op_result; s->op = OP_NONE; s->op_done = false; s->phase = 0;
    return r;
}
static void begin(ae_store_t *s, operation_t op, unsigned slot)
{
    s->op = op; s->op_done = false; s->phase = 0; s->slot = (uint16_t)slot; s->op_id = s->entries[slot].meta.object_id;
}
static ae_result_t caps(void *context, ae_store_capabilities_t *out)
{
    ae_store_t *s = context; if (!s || !out) return AE_INVALID; uint64_t now;
    *out = (ae_store_capabilities_t){.storage = s->device.storage, .implemented = true, .available = s->status.mounted,
        .geometry_verified = s->device.geometry_verified, .owned_and_provisioned = s->status.mounted,
        .encryption_ready = s->key_loaded, .authenticated_metadata = s->status.mounted,
        .recovery_complete = !s->status.recovery_pending && s->status.mounted,
        .retention_time_known = time_now(s, &now) == AE_OK, .usable_bytes = s->device.region_bytes,
        .quota_bytes = s->quota, .used_bytes = s->status.used_bytes, .reserved_bytes = s->status.reserved_bytes,
        .per_object_overhead_bytes = 2U * PAGE, .media_chunk_bytes = PAYLOAD, .media_chunk_overhead_bytes = PAGE - PAYLOAD,
        .object_count = s->status.objects, .object_limit = s->limit, .reason = s->status.error}; return AE_OK;
}
static bool same_immutable(const ae_record_metadata_t *a, const ae_record_metadata_t *b)
{
    return ae_id_equal(&a->object_id, &b->object_id) && ae_id_equal(&a->principal_id, &b->principal_id) &&
        ae_id_equal(&a->consent_id, &b->consent_id) && a->consent_revision == b->consent_revision &&
        a->created_unix_seconds == b->created_unix_seconds && a->expires_unix_seconds == b->expires_unix_seconds &&
        a->reserved_bytes == b->reserved_bytes && a->payload_limit_bytes == b->payload_limit_bytes &&
        a->storage == b->storage && a->export_policy == b->export_policy && ae_format_equal(&a->format, &b->format);
}
static ae_result_t reserve(void *context, const ae_record_metadata_t *m)
{
    ae_store_t *s = context; if (!s || !m) return AE_INVALID;
    ae_result_t r = ready(s); if (r != AE_OK) return r;
    if (s->op != OP_NONE) {
        if (s->op == OP_RESERVE && ae_id_equal(&s->op_id, &m->object_id) && !same_immutable(m, &s->requested)) return AE_INVALID;
        r = complete(s, OP_RESERVE, &m->object_id); if (r == AE_OK) s->writer = s->slot; return r;
    }
    int existing = find(s, &m->object_id);
    if (existing >= 0) return same_immutable(m, &s->entries[existing].meta) && !terminal(&s->entries[existing]) ? AE_OK : AE_STALE;
    if (s->writer >= 0 && !terminal(&s->entries[s->writer])) return AE_BUSY;
    if (m->reserved_bytes > s->quota - s->status.used_bytes - s->status.reserved_bytes || m->reserved_bytes < 65536) return AE_QUOTA;
    unsigned slot = 0; while (slot < s->limit && s->entries[slot].present && !reclaimed(&s->entries[slot])) ++slot;
    if (slot == s->limit) return AE_QUOTA;
    uint32_t pages = (uint32_t)((m->reserved_bytes - 2U * PAGE) / PAGE);
    uint32_t first = s->data_first;
    for (;;) {
        if (first > s->total_pages || pages > s->total_pages - first) return AE_QUOTA;
        bool overlap = false;
        for (unsigned i = 0; i < s->limit; ++i) {
            entry_t *e = &s->entries[i]; if (!e->present || reclaimed(e)) continue;
            if (first < e->first + e->pages && e->first < first + pages) { first = e->first + e->pages; overlap = true; break; }
        }
        if (!overlap) break;
    }
    if (s->entries[slot].sequence == UINT64_MAX) return AE_EXHAUSTED;
    s->pending = (entry_t){.meta = *m, .sequence = s->entries[slot].sequence + 1U, .first = first, .pages = pages, .present = true, .observed_time = s->last_time};
    s->pending.meta.stored_bytes = 2U * PAGE; s->pending.meta.integrity_verified = s->pending.meta.sync_acknowledged = true;
    encode_entry(&s->pending, s->plain);
    entry_t checked; if (!decode_entry(s, &checked, s->plain) || m->state != AE_RECORD_STARTING || m->pcm_bytes ||
        m->revision != 1 || m->access_revoked || m->hash_available || s->last_time >= m->expires_unix_seconds) return AE_INVALID;
    begin(s, OP_RESERVE, slot); s->op_id = m->object_id; s->requested = *m; return AE_AGAIN;
}
static ae_result_t append(void *context, const ae_id_t *id, uint64_t offset, const void *data, size_t bytes, size_t *actual)
{
    ae_store_t *s = context; if (!s || !id || !data || !actual || !bytes || bytes > AE_FRAME_BYTES_MAX) return AE_INVALID;
    *actual = 0; int index; ae_result_t r = live_entry(s, id, &index, false); if (r != AE_OK) return r;
    entry_t *e = &s->entries[index]; size_t frame; uint32_t samples;
    if (ae_format_size(&e->meta.format, &frame, &samples) != AE_OK || bytes != frame || index != s->writer || terminal(e) || offset != e->meta.pcm_bytes) return AE_INVALID;
    if (s->op != OP_NONE) { r = complete(s, OP_FLUSH, id); if (r != AE_OK) return r; }
    if (bytes > e->meta.payload_limit_bytes - offset) return AE_QUOTA;
    if (s->cache_size + bytes > PAYLOAD) { begin(s, OP_FLUSH, (unsigned)index); return AE_AGAIN; }
    if (e->committed >= e->pages) return AE_QUOTA;
    memcpy(s->cache + s->cache_size, data, bytes); s->cache_size += (uint16_t)bytes;
    e->meta.pcm_bytes += bytes; e->meta.state = AE_RECORD_RECORDING; *actual = bytes; return AE_OK;
}
static ae_result_t checkpoint(void *context, const ae_id_t *id, ae_record_metadata_t *out)
{
    ae_store_t *s = context; if (!s || !id || !out) return AE_INVALID;
    if (s->op == OP_RESERVE && ae_id_equal(&s->op_id, id)) {
        ae_result_t reserved = complete(s, OP_RESERVE, id);
        if (reserved != AE_OK) return reserved;
        s->writer = s->slot;
    }
    int index; ae_result_t r = live_entry(s, id, &index, true); if (r != AE_OK) return r;
    if (s->op != OP_NONE) {
        if (s->op == OP_RESERVE) { r = complete(s, OP_RESERVE, id); if (r != AE_OK) return r; s->writer = index; }
        else if (s->op == OP_FLUSH) { r = complete(s, OP_FLUSH, id); if (r != AE_OK) return r; }
        else { r = complete(s, OP_CHECKPOINT, id); if (r == AE_OK) *out = s->entries[index].meta; return r; }
    }
    entry_t *e = &s->entries[index];
    if (terminal(e) || (e->meta.pcm_bytes == e->meta.synced_pcm_bytes && !s->cache_size)) { *out = e->meta; return AE_OK; }
    if (index != s->writer) return AE_BUSY;
    begin(s, OP_CHECKPOINT, (unsigned)index); return AE_AGAIN;
}
static ae_result_t finish(void *context, const ae_record_metadata_t *m, const uint8_t header[44], ae_record_metadata_t *out)
{
    ae_store_t *s = context; if (!s || !m || !header || !out) return AE_INVALID;
    if (s->finish_cancel_done && ae_id_equal(&s->cancel_id, &m->object_id)) return AE_TOO_LATE;
    int index; ae_result_t r = live_entry(s, &m->object_id, &index, true); if (r != AE_OK) return r;
    if (s->op != OP_NONE) {
        if (s->op == OP_FINISH && (!same_immutable(m, &s->requested) || m->revision != s->requested.revision || m->state != s->requested.state)) return AE_INVALID;
        r = complete(s, OP_FINISH, &m->object_id);
        if (r == AE_OK) { *out = s->entries[index].meta; s->writer = -1; } return r;
    }
    entry_t *e = &s->entries[index]; uint8_t expected[44];
    if (terminal(e)) { if (e->meta.revision != m->revision || e->meta.state != m->state) return AE_TOO_LATE; *out = e->meta; return AE_OK; }
    if (!same_immutable(m, &e->meta) || m->pcm_bytes != e->meta.synced_pcm_bytes || m->pcm_bytes != e->meta.pcm_bytes ||
        s->cache_size || e->sequence == UINT64_MAX || e->meta.revision == UINT64_MAX || m->revision != e->meta.revision + 1 ||
        m->state < AE_RECORD_COMPLETED || m->state > AE_RECORD_ERROR || (m->state == AE_RECORD_COMPLETED && !m->pcm_bytes) ||
        ae_wav_header(&m->format, m->pcm_bytes, expected) != AE_OK || memcmp(header, expected, 44)) return AE_INVALID;
    s->pending = *e; s->pending.meta = *m; ++s->pending.sequence;
    s->pending.meta.integrity_verified = s->pending.meta.sync_acknowledged = true;
    begin(s, OP_FINISH, (unsigned)index); s->requested = *m; return AE_AGAIN;
}
static ae_result_t get(void *context, const ae_id_t *id, ae_record_metadata_t *out)
{
    ae_store_t *s = context; if (!id || !out) return AE_INVALID;
    int index; ae_result_t r = live_entry(s, id, &index, true); if (r == AE_OK) *out = s->entries[index].meta; return r;
}
static ae_result_t list(void *context, const ae_id_t *principal, uint32_t cursor,
    ae_record_metadata_t *out, size_t capacity, size_t *count, uint32_t *next)
{
    ae_store_t *s = context;
    if (!s || !principal || ae_id_empty(principal) || !out || !count || !next || !capacity || capacity > AE_LIST_MAX || cursor > s->limit) return AE_INVALID;
    *count = 0; *next = 0; ae_result_t r = ready(s); if (r != AE_OK) return r;
    while (cursor < s->limit && *count < capacity) {
        entry_t *e = &s->entries[cursor++];
        if (e->present && !e->meta.access_revoked && s->last_time < e->meta.expires_unix_seconds && ae_id_equal(principal, &e->meta.principal_id)) out[(*count)++] = e->meta;
    }
    *next = cursor == s->limit ? 0 : cursor; return AE_OK;
}
static void cancel_read(void *context, const ae_id_t *id)
{
    ae_store_t *s = context;
    if (s && id && s->op == OP_READ && ae_id_equal(id, &s->op_id)) s->read_cancelled = true;
}
static ae_result_t read_wav(void *context, const ae_id_t *id, uint64_t offset, void *data, size_t bytes, size_t *actual)
{
    ae_store_t *s = context; if (!s || !id || !data || !actual || !bytes || bytes > READ_MAX) return AE_INVALID;
    *actual = 0; int index; ae_result_t r = live_entry(s, id, &index, false);
    if (r != AE_OK) { cancel_read(s, id); return r; }
    entry_t *e = &s->entries[index];
    if (e->meta.state != AE_RECORD_COMPLETED || !e->meta.hash_available || offset >= e->meta.pcm_bytes + 44 || bytes > e->meta.pcm_bytes + 44 - offset) return AE_DENIED;
    if (s->op != OP_NONE) {
        if (s->op == OP_READ && (s->read_offset != offset || s->read_size != bytes)) return AE_INVALID;
        r = complete(s, OP_READ, id);
        if (r == AE_OK) { memcpy(data, s->read_data, bytes); *actual = bytes; ae_secure_zero(s->read_data, READ_MAX); } return r;
    }
    begin(s, OP_READ, (unsigned)index); s->read_offset = offset; s->read_size = bytes; s->read_done = 0;
    s->read_low = 0; s->read_high = e->committed; return AE_AGAIN;
}
static ae_result_t remove_object(void *context, const ae_id_t *id, uint64_t revision, bool *revoked, bool *done)
{
    ae_store_t *s = context; if (!s || !id || !revoked || !done) return AE_INVALID;
    *revoked = *done = false; int index; ae_result_t r = live_entry(s, id, &index, true); if (r != AE_OK) return r;
    entry_t *e = &s->entries[index];
    if (s->op != OP_NONE) {
        if (s->op == OP_REMOVE && s->op_revision != revision) return AE_STALE;
        r = complete(s, OP_REMOVE, id);
        if (r == AE_OK) { *revoked = *done = true; if (s->writer == index) { s->writer = -1; s->cache_size = 0; ae_secure_zero(s->cache, PAYLOAD); } }
        return r;
    }
    if (reclaimed(e)) { if (revision == UINT64_MAX || revision + 1 != e->meta.revision) return AE_STALE; *revoked = *done = true; return AE_OK; }
    if (revision != e->meta.revision) return AE_STALE;
    if (e->sequence == UINT64_MAX || revision == UINT64_MAX) return AE_EXHAUSTED;
    s->pending = *e; ++s->pending.sequence; ++s->pending.meta.revision;
    /* Journal only the durable prefix, never a volatile capture cache. */
    s->pending.meta.pcm_bytes = e->meta.synced_pcm_bytes;
    s->pending.meta.sample_frames = e->meta.synced_pcm_bytes / (e->meta.format.channels * 2U);
    s->pending.meta.state = AE_RECORD_DELETING; s->pending.meta.access_revoked = true;
    s->pending.meta.hash_available = false; memset(s->pending.meta.sha256, 0, 32);
    begin(s, OP_REMOVE, (unsigned)index); s->op_revision = revision; return AE_AGAIN;
}
static ae_result_t cancel_finish(void *context, const ae_id_t *id, uint64_t revision)
{
    ae_store_t *s = context; if (!s || !id) return AE_INVALID;
    if (s->finish_cancel_done && ae_id_equal(id, &s->cancel_id) && revision == s->cancel_revision) {
        s->finish_cancel_done = false; return AE_OK;
    }
    if (s->op != OP_FINISH || !ae_id_equal(id, &s->op_id)) {
        int index = find(s, id);
        return index >= 0 && terminal(&s->entries[index]) ? AE_TOO_LATE : s->op == OP_NONE ? AE_OK : AE_AGAIN;
    }
    if (revision != s->requested.revision) return AE_STALE;
    if (s->write_phase > 2 || (s->write_phase == 2 && s->program_pending) || s->op_done) return AE_TOO_LATE;
    s->cancel_id = *id; s->cancel_revision = revision; s->finish_cancel_pending = true; return AE_AGAIN;
}

ae_store_port_t ae_store_port(ae_store_t *s)
{
    return (ae_store_port_t){.context = s, .capabilities = caps, .reserve = reserve, .append = append,
        .checkpoint = checkpoint, .finish = finish, .get = get, .list = list, .read = read_wav, .remove = remove_object, .cancel_read = cancel_read, .cancel_finish = cancel_finish};
}
