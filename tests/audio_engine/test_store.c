// SPDX-License-Identifier: Apache-2.0
#include "audio_store.h"
#include "audio_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_BYTES (828U * 1024U)
static struct {
    uint8_t bytes[DISK_BYTES], saved[DISK_BYTES], key[32];
    ae_id_t key_id;
    uint64_t next_nonce, time;
    unsigned writes, erases, leases;
    bool has_key, async, pending, torn;
} disk;
static ae_store_t *store;
static void *memory;
static ae_store_port_t port;
static const ae_format_t format = {.sample_rate_hz = 16000, .channels = 1, .frame_ms = 10, .encoding = AE_PCM_S16_LE};
static ae_id_t id(unsigned n) { ae_id_t value = {0}; value.bytes[0] = (uint8_t)n; return value; }
static bool again(void) { if (!disk.async) return false; disk.pending = !disk.pending; return disk.pending; }
static ae_result_t read_disk(void *c, uint64_t offset, void *out, size_t bytes)
{
    (void)c; assert(offset <= DISK_BYTES && bytes <= DISK_BYTES - offset && bytes <= 4096);
    if (again()) return AE_AGAIN;
    memcpy(out, disk.bytes + offset, bytes); return AE_OK;
}
static ae_result_t program(void *c, uint64_t offset, const void *in, size_t bytes)
{
    (void)c; assert(offset % 4096 == 0 && bytes == 4096 && offset + bytes <= DISK_BYTES);
    if (again()) return AE_AGAIN;
    ++disk.writes;
    size_t n = disk.torn ? bytes / 2 : bytes;
    const uint8_t *p = in;
    for (size_t i = 0; i < n; ++i) { assert((disk.bytes[offset + i] & p[i]) == p[i]); disk.bytes[offset + i] = p[i]; }
    return disk.torn ? AE_UNCERTAIN : AE_OK;
}
static ae_result_t erase(void *c, uint64_t offset, size_t bytes)
{
    (void)c; assert(offset % 4096 == 0 && bytes == 4096 && offset + bytes <= DISK_BYTES);
    if (again()) return AE_AGAIN;
    ++disk.erases; memset(disk.bytes + offset, 255, bytes); return AE_OK;
}
static ae_result_t sync_disk(void *c) { (void)c; return again() ? AE_AGAIN : AE_OK; }
static ae_result_t key(void *c, bool create, ae_id_t *key_id, uint8_t out[32])
{
    (void)c;
    if (!disk.has_key && !create) return AE_NOT_FOUND;
    if (!disk.has_key) { disk.has_key = true; disk.key_id = id(239); for (unsigned i = 0; i < 32; ++i) disk.key[i] = (uint8_t)(i + 89); }
    *key_id = disk.key_id; memcpy(out, disk.key, 32); return AE_OK;
}
static ae_result_t nonce(void *c, uint64_t count, uint64_t *first)
{
    (void)c; *first = disk.next_nonce; disk.next_nonce += count; ++disk.leases; return AE_OK;
}
static ae_result_t utc(void *c, uint64_t *out) { (void)c; *out = disk.time; return disk.time ? AE_OK : AE_TIME_UNKNOWN; }
static ae_storage_device_t device(void)
{
    return (ae_storage_device_t){.storage = AE_STORAGE_RECORDS_PARTITION, .region_bytes = DISK_BYTES,
        .erase_bytes = 4096, .geometry_verified = true, .explicitly_owned = true, .key_protection_verified = true,
        .read = read_disk, .program = program, .erase = erase, .sync = sync_disk, .key = key, .nonce_range = nonce,
        .trusted_unix_time = utc};
}
static void reboot(void)
{
    if (memory) { ae_store_deinit(store); free(memory); }
    memory = calloc(1, ae_store_bytes()); assert(memory); disk.pending = false;
    ae_storage_device_t d = device(); assert(ae_store_init(memory, ae_store_bytes(), &d, &store) == AE_AGAIN);
    port = ae_store_port(store);
}
static ae_result_t service(void)
{
    ae_result_t r = ae_store_service(store);
    if (!(r == AE_OK || r == AE_AGAIN || r == AE_UNAVAILABLE || r == AE_INTEGRITY || r == AE_UNCERTAIN || r == AE_TIME_UNKNOWN)) fprintf(stderr, "unexpected service result %d\n", r);
    assert(r == AE_OK || r == AE_AGAIN || r == AE_UNAVAILABLE || r == AE_INTEGRITY || r == AE_UNCERTAIN || r == AE_TIME_UNKNOWN);
    return r;
}
static void mount(void)
{
    for (unsigned i = 0; i < 10000 && !ae_store_status(store).mounted && !ae_store_status(store).storage_uncertain; ++i) (void)service();
}
static void fresh(bool async)
{
    if (memory) { ae_store_deinit(store); free(memory); memory = NULL; }
    memset(&disk, 0, sizeof(disk)); memset(disk.bytes, 255, DISK_BYTES); disk.time = 100000; disk.next_nonce = 1; disk.async = async;
    reboot(); (void)service();
    assert(ae_store_provision(store, 8, 512U * 1024U) == AE_AGAIN);
    mount(); assert(ae_store_status(store).mounted);
}
static ae_record_metadata_t metadata(unsigned n)
{
    return (ae_record_metadata_t){.object_id = id(n), .principal_id = id(200), .consent_id = id(201),
        .consent_revision = 1, .revision = 1, .created_unix_seconds = disk.time,
        .expires_unix_seconds = disk.time + 3600, .reserved_bytes = 65536, .payload_limit_bytes = 48000,
        .format = format, .state = AE_RECORD_STARTING, .storage = AE_STORAGE_RECORDS_PARTITION,
        .export_policy = AE_EXPORT_OWNER_DOWNLOAD};
}
#define RETRY(expression) do { ae_result_t result_ = AE_AGAIN; unsigned loops_ = 0; \
    while ((result_ = (expression)) == AE_AGAIN && loops_++ < 20000) (void)service(); \
    if (result_ != AE_OK) fprintf(stderr, "failure line %d: %d after %u\n", __LINE__, result_, loops_); \
    assert(result_ == AE_OK); } while (0)
static void reserve_object(ae_record_metadata_t *m) { RETRY(port.reserve(port.context, m)); }
static void write_frames(ae_record_metadata_t *m, unsigned count, uint8_t value)
{
    uint8_t frame[320]; memset(frame, value, sizeof(frame)); size_t actual;
    for (unsigned i = 0; i < count; ++i) { RETRY(port.append(port.context, &m->object_id, m->pcm_bytes, frame, sizeof(frame), &actual)); assert(actual == sizeof(frame)); m->pcm_bytes += actual; }
}
static void checkpoint_object(ae_record_metadata_t *m)
{
    ae_record_metadata_t out; RETRY(port.checkpoint(port.context, &m->object_id, &out));
    assert(out.synced_pcm_bytes == m->pcm_bytes && out.integrity_verified && out.sync_acknowledged); *m = out;
}
static void finish_object(ae_record_metadata_t *m)
{
    checkpoint_object(m); m->state = AE_RECORD_COMPLETED; ++m->revision;
    uint8_t header[44]; assert(ae_wav_header(&m->format, m->pcm_bytes, header) == AE_OK);
    ae_record_metadata_t out; RETRY(port.finish(port.context, m, header, &out));
    assert(out.hash_available && out.integrity_verified && out.state == AE_RECORD_COMPLETED); *m = out;
}
static void roundtrip(bool async)
{
    fresh(async); ae_record_metadata_t m = metadata(1); reserve_object(&m);
    write_frames(&m, 15, 0x37); checkpoint_object(&m); write_frames(&m, 9, 0x5a); finish_object(&m);
    uint8_t wav[7724]; size_t actual; RETRY(port.read(port.context, &m.object_id, 0, wav, sizeof(wav), &actual));
    assert(actual == sizeof(wav)); ae_wav_info_t info; assert(ae_wav_inspect(wav, sizeof(wav), &info) == AE_OK);
    for (size_t i = 44; i < 4844; ++i) assert(wav[i] == 0x37);
    for (size_t i = 4844; i < sizeof(wav); ++i) assert(wav[i] == 0x5a);
    for (size_t i = 0; i + 32 < DISK_BYTES; ++i) assert(memcmp(disk.bytes + i, wav + 44, 32));
    const char *export_path = getenv("AE_TEST_RECORD_WAV");
    if (export_path) {
        FILE *file = fopen(export_path, "wb"); assert(file && fwrite(wav, 1, sizeof(wav), file) == sizeof(wav)); assert(fclose(file) == 0);
        char digest_path[1024]; int written = snprintf(digest_path, sizeof(digest_path), "%s.sha256", export_path);
        assert(written > 0 && (size_t)written < sizeof(digest_path)); file = fopen(digest_path, "w"); assert(file);
        for (unsigned i = 0; i < 32; ++i) assert(fprintf(file, "%02x", m.sha256[i]) == 2);
        assert(fclose(file) == 0);
    }
    assert(disk.leases == 1);
    unsigned writes = disk.writes; reboot(); mount(); assert(ae_store_status(store).mounted && disk.writes == writes);
    ae_record_metadata_t found; assert(port.get(port.context, &m.object_id, &found) == AE_OK && !memcmp(found.sha256, m.sha256, 32));
    RETRY(port.read(port.context, &m.object_id, 4700, wav, 600, &actual)); assert(actual == 600);
    for (size_t i = 0; i < 144; ++i) assert(wav[i] == 0x37);
    for (size_t i = 144; i < 600; ++i) assert(wav[i] == 0x5a);
    bool revoked, done; RETRY(port.remove(port.context, &m.object_id, m.revision, &revoked, &done)); assert(revoked && done);
    assert(port.get(port.context, &m.object_id, &found) == AE_OK && found.access_revoked);
    reboot(); mount(); assert(ae_store_status(store).mounted);
    assert(port.get(port.context, &m.object_id, &found) == AE_OK && found.access_revoked);
    ae_record_metadata_t next = metadata(2); reserve_object(&next); write_frames(&next, 2, 0x19); finish_object(&next);
    reboot(); mount(); assert(ae_store_status(store).mounted && ae_store_status(store).objects == 1);
    assert(port.get(port.context, &m.object_id, &found) == AE_NOT_FOUND);
}
static void recovery(void)
{
    fresh(false); ae_record_metadata_t m = metadata(1); reserve_object(&m); write_frames(&m, 13, 42); checkpoint_object(&m);
    uint64_t durable = m.pcm_bytes; write_frames(&m, 3, 99); unsigned leases = disk.leases;
    reboot(); mount(); assert(ae_store_status(store).mounted && disk.leases == leases + 1);
    ae_record_metadata_t found; assert(port.get(port.context, &m.object_id, &found) == AE_OK);
    assert(found.state == AE_RECORD_PARTIAL && found.pcm_bytes == durable && found.reason == AE_UNCERTAIN);
    uint8_t bytes[44]; size_t actual; assert(port.read(port.context, &m.object_id, 0, bytes, 44, &actual) == AE_DENIED);
    disk.time += 3601;
    assert(port.read(port.context, &m.object_id, 0, bytes, 44, &actual) == AE_DENIED);
    reboot(); mount(); assert(ae_store_status(store).mounted && ae_store_status(store).objects == 0);
}
static void corruption(void)
{
    fresh(false); ae_record_metadata_t m = metadata(1); reserve_object(&m); write_frames(&m, 2, 42); finish_object(&m);
    memcpy(disk.saved, disk.bytes, DISK_BYTES);
    const size_t positions[] = {100, 3U * 4096U + 200, 18U * 4096U + 90};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        memcpy(disk.bytes, disk.saved, DISK_BYTES); disk.bytes[positions[i]] ^= 1;
        unsigned writes = disk.writes; reboot(); mount();
        assert(!ae_store_status(store).mounted && ae_store_status(store).storage_uncertain && disk.writes == writes);
    }
}
static void interrupted_deletion(void)
{
    fresh(false); ae_record_metadata_t m = metadata(1); reserve_object(&m); write_frames(&m, 2, 42); finish_object(&m);
    bool revoked, done; assert(port.remove(port.context, &m.object_id, m.revision, &revoked, &done) == AE_AGAIN);
    assert(service() == AE_AGAIN); disk.torn = true; assert(service() == AE_UNCERTAIN); disk.torn = false;
    reboot(); mount(); assert(!ae_store_status(store).mounted && ae_store_status(store).storage_uncertain);
}
static void unknown_preserved(void)
{
    fresh(false); ae_store_deinit(store); free(memory); memory = NULL; disk.has_key = false;
    memset(disk.bytes, 255, DISK_BYTES); disk.bytes[DISK_BYTES - 1] = 0;
    reboot(); (void)service(); unsigned writes = disk.writes, erases = disk.erases;
    assert(ae_store_provision(store, 8, 512U * 1024U) == AE_AGAIN); mount();
    assert(ae_store_status(store).storage_uncertain && disk.writes == writes && disk.erases == erases && !disk.has_key);
}
static void cancellation_and_abandoned_read(void)
{
    fresh(true); ae_record_metadata_t m = metadata(1); reserve_object(&m); write_frames(&m, 20, 42); checkpoint_object(&m);
    ae_record_metadata_t terminal_meta = m, evidence; terminal_meta.state = AE_RECORD_COMPLETED; ++terminal_meta.revision;
    uint8_t header[44]; assert(ae_wav_header(&m.format, m.pcm_bytes, header) == AE_OK);
    assert(port.finish(port.context, &terminal_meta, header, &evidence) == AE_AGAIN);
    assert(service() == AE_AGAIN); /* Hash setup. */
    assert(service() == AE_AGAIN); /* An actual async read is outstanding. */
    RETRY(port.cancel_finish(port.context, &m.object_id, terminal_meta.revision));
    assert(port.get(port.context, &m.object_id, &evidence) == AE_OK && evidence.state == AE_RECORD_RECORDING);
    terminal_meta.state = AE_RECORD_PARTIAL; terminal_meta.reason = AE_TOO_LATE;
    RETRY(port.finish(port.context, &terminal_meta, header, &evidence)); assert(evidence.state == AE_RECORD_PARTIAL);
    reboot(); mount(); assert(ae_store_status(store).mounted);
    assert(port.get(port.context, &m.object_id, &evidence) == AE_OK && evidence.state == AE_RECORD_PARTIAL);
    fresh(true); m = metadata(1); reserve_object(&m); write_frames(&m, 20, 42); finish_object(&m);
    uint8_t out[640]; size_t actual;
    assert(port.read(port.context, &m.object_id, 44, out, sizeof(out), &actual) == AE_AGAIN);
    assert(service() == AE_AGAIN); port.cancel_read(port.context, &m.object_id);
    for (unsigned i = 0; i < 50; ++i) (void)service();
    bool revoked, done; RETRY(port.remove(port.context, &m.object_id, m.revision, &revoked, &done)); assert(revoked && done);
}

static void retention_and_clock(void)
{
    fresh(false); ae_record_metadata_t m = metadata(1); reserve_object(&m); write_frames(&m, 2, 42);
    disk.time += 100; finish_object(&m); unsigned writes = disk.writes;
    disk.time -= 50; reboot(); mount();
    assert(!ae_store_status(store).mounted && !ae_store_status(store).storage_uncertain && disk.writes == writes);
    disk.time += 50; mount(); assert(ae_store_status(store).mounted);
    disk.time += 3600;
    for (unsigned i = 0; i < 100; ++i) (void)service();
    assert(ae_store_status(store).objects == 0);
    ae_record_metadata_t observed; assert(port.get(port.context, &m.object_id, &observed) == AE_OK && observed.access_revoked);
    reboot(); mount(); assert(ae_store_status(store).mounted && ae_store_status(store).objects == 0);
}

int main(void)
{
    roundtrip(false); roundtrip(true); recovery(); corruption(); interrupted_deletion(); unknown_preserved(); cancellation_and_abandoned_read(); retention_and_clock();
    printf("PASS: real AES-GCM/SHA256 storage, async ports, WAV roundtrip, nonce recovery, deletion, corruption and torn-write quarantine; %zu bytes\n", ae_store_bytes());
    ae_store_deinit(store); free(memory); return 0;
}
