#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "host/ble_hs.h"

// Called on the NimBLE host owner. SDK in-memory store remains the lookup owner.
int passport_store_init(void);
bool passport_store_known(void);
uint32_t passport_store_security_revision(void);
int passport_store_write(int type, const union ble_store_value *value);
// ENOENT means a legacy/missing preference; CLEARED records explicit no target.
#define PASSPORT_STORE_SELECTION_CLEARED (-1)
int passport_store_selected(ble_addr_t *peer);
int passport_store_select(const ble_addr_t *peer);
int passport_store_forget(const ble_addr_t *peer);
