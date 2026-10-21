// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "audio_io.h"
#include "audio_store.h"

#define BOARD_RECORDING_FLASH_LABEL "recordings"
#define BOARD_RECORDING_FLASH_SUBTYPE 0x40
#define BOARD_RECORDING_FLASH_ERASE_BYTES UINT32_C(4096)
#define BOARD_RECORDING_FLASH_IO_MAX UINT32_C(4096)

typedef enum {
    BOARD_RECORDING_FLASH_CLOSED,
    BOARD_RECORDING_FLASH_ERASED,
    BOARD_RECORDING_FLASH_OWNED,
    BOARD_RECORDING_FLASH_PROVISIONING,
    BOARD_RECORDING_FLASH_UNKNOWN,
    BOARD_RECORDING_FLASH_FAULT,
} board_recording_flash_state_t;

typedef struct {
    board_recording_flash_state_t state;
    bool geometry_verified;
    uint32_t partition_address;
    uint32_t partition_bytes;
    uint32_t capacity_bytes; /* Data bytes only; excludes the ownership sector. */
    uint32_t erase_bytes;
    uint8_t write_alignment;
    uint8_t volume_id[AE_ID_BYTES];
    ae_result_t result;
    int sdk_error;
} board_recording_flash_status_t;

/* A private worker owns SDK access. Public port calls enqueue/poll a copied
 * request and return AE_AGAIN until complete. One caller serializes requests
 * and retries identical arguments; no borrowed caller buffer is retained.
 *
 * open validates the exact named data partition and reads ownership. It never
 * provisions, erases or repairs. provision is a distinct explicit action,
 * accepted only for a fully erased region or the same valid volume identity.
 * It never erases an existing volume. Unknown/corrupt contents are preserved.
 *
 * Data offsets exclude the first 4096 bytes. read/program accept 1..4096 bytes;
 * erase accepts exactly one aligned sector. Program and erase confirm readback;
 * partial/uncertain writes report zero verified bytes and latch fault. Reopen
 * is read-only and permits the journal owner to recover, never resume capture.
 * The marker's CRC detects corruption; it is NOT authenticated metadata. The
 * store above must provide encryption, authentication, journal/recovery and
 * retention. This byte backend must not be advertised as a complete store.
 */
typedef struct {
    void *context;
    ae_result_t (*open)(void *);
    ae_result_t (*provision)(void *, const uint8_t volume_id[AE_ID_BYTES]);
    ae_result_t (*read)(void *, uint32_t offset, void *, size_t bytes, size_t *actual);
    ae_result_t (*program)(void *, uint32_t offset, const void *, size_t bytes, size_t *actual);
    ae_result_t (*erase)(void *, uint32_t offset);
    ae_result_t (*sync)(void *);
} board_recording_flash_port_t;

board_recording_flash_port_t board_recording_flash_port(void);
board_recording_flash_status_t board_recording_flash_status(void);
ae_result_t board_recording_flash_init(void);
/* The encrypted store receives offsets within the data area. The returned
 * callbacks ignore context so the caller may supply key/nonce/time callbacks
 * with its own context. These security callbacks are deliberately absent here;
 * missing protected key/counter evidence must keep recording unavailable. */
ae_storage_device_t board_recording_flash_device(void);
