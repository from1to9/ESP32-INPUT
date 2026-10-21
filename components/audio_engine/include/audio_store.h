// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_io.h"

#define AE_STORE_PAGE_BYTES 4096U
#define AE_STORE_NONCE_LEASE 32768U

/* Lower storage boundary for the real encrypted object/journal backend.
 * This is a dedicated explicitly owned region, not a fallback filesystem.
 * Every offset is relative to that region; no caller supplies a partition/path.
 * Operations are serialized and bounded by AE_IO_TIMEOUT_MS. AE_AGAIN retains
 * no caller pointer and means retry identical bytes/identity. The adapter may
 * privately copy one bounded request; safety never depends on waiting for it.
 * read/program transfer at most one 4096-byte page. erase is one aligned page.
 * A returned write error/uncertainty must not be converted to durable success.
 */
typedef struct {
    void *context;
    ae_storage_t storage;
    uint64_t region_bytes;
    uint32_t erase_bytes; /* Initial backend requires exactly 4096. */
    bool geometry_verified;
    bool explicitly_owned;
    /* Selected software-custody profile: key lives only in the dedicated
     * control namespace, is never exported by product APIs, and is coupled to
     * the durable nonce counter. This does NOT claim physical tamper resistance
     * or require irreversible eFuse/flash-encryption provisioning. */
    bool key_protection_verified;
    ae_result_t (*read)(void *, uint64_t offset, void *output, size_t bytes);
    ae_result_t (*program)(void *, uint64_t offset, const void *data, size_t bytes);
    ae_result_t (*erase)(void *, uint64_t offset, size_t bytes);
    ae_result_t (*sync)(void *);
    /* Read a volume-specific AES-256 key and opaque key ID. create=true occurs
     * only during explicit provisioning of an all-erased owned region. Key
     * storage must satisfy that custody profile; missing/uncertain keys disable writes.
     * The backend clears the temporary key after expanding its cipher state. */
    ae_result_t (*key)(void *, bool create, ae_id_t *key_id, uint8_t output[32]);
    /* Atomically persist a NEVER-REUSED counter range for this key, including
     * across resets/power loss, before returning AE_OK. Ambiguous allocation
     * burns the range. The backend leases 32768 counters lazily, not per frame
     * or per progress tick. A key must never survive a counter reset. */
    ae_result_t (*nonce_range)(void *, uint64_t count, uint64_t *first);
    ae_result_t (*trusted_unix_time)(void *, uint64_t *seconds);
    uint64_t (*monotonic_ms)(void *);
    void (*completion_notify)(void *, void (*notify)(void *), void *notify_context);
} ae_storage_device_t;

typedef struct ae_store ae_store_t;
typedef struct {
    bool initialized, mounted, provisioning, recovery_pending, storage_uncertain;
    ae_result_t error;
    uint16_t objects;
    uint64_t quota_bytes, used_bytes, reserved_bytes;
} ae_store_status_t;

/* Memory includes bounded page caches/index and must normally be PSRAM.
 * init/mount never formats or writes. Unknown/non-erased contents are rejected.
 * service makes bounded recovery/provisioning progress; no capture resumes.
 * Explicit provisioning is the ONLY creation path. It first verifies every
 * page is erased, then writes authenticated ownership/superblock records.
 * Any non-erased unknown region remains untouched, requiring migration. */
size_t ae_store_bytes(void);
void ae_store_deinit(ae_store_t *);
ae_result_t ae_store_init(void *memory, size_t bytes, const ae_storage_device_t *, ae_store_t **);
ae_result_t ae_store_provision(ae_store_t *, uint16_t object_limit, uint64_t quota_bytes);
ae_result_t ae_store_service(ae_store_t *);
ae_store_port_t ae_store_port(ae_store_t *);
ae_store_status_t ae_store_status(const ae_store_t *);
