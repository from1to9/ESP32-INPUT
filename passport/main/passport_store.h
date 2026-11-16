#pragma once

#include <stdbool.h>
#include "host/ble_hs.h"

// Called on the NimBLE host owner. SDK in-memory store remains the lookup owner.
int passport_store_init(void);
bool passport_store_known(void);
int passport_store_write(int type, const union ble_store_value *value);
int passport_store_forget(const ble_addr_t *peer);
