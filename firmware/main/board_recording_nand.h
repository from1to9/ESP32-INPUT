// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "audio_io.h"

/* Inventory is explicitly separate from a writable recording store. The probe
 * never initializes Dhara or reads through its ECC-refreshing logical API.
 * No page program, block/chip erase, format or sync command is exposed here.
 * A recognized device ID does not prove geometry, ownership or a valid mount.
 * Start once at boot; poll the cached snapshot without touching hardware.
 */
typedef enum {
    BOARD_NAND_NOT_PROBED,
    BOARD_NAND_PROBING,
    BOARD_NAND_NEEDS_BACKUP_AND_MIGRATION,
    BOARD_NAND_PROBE_FAILED,
    BOARD_NAND_CLEANUP_UNCERTAIN,
} board_nand_state_t;

typedef struct {
    board_nand_state_t state;
    ae_result_t result;
    int sdk_error;
    uint8_t jedec_id[3];
    uint8_t configuration_register;
    uint8_t status_register;
    bool identification_observed;
    bool cleanup_complete;
} board_nand_status_t;

ae_result_t board_recording_nand_probe_start(void);
board_nand_status_t board_recording_nand_status(void);
/* Truthful unavailable capabilities until a versioned, owned, authenticated
 * store and non-destructive migration have been implemented and verified. */
ae_store_capabilities_t board_recording_nand_capabilities(void);
