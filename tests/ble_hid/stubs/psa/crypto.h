// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int psa_status_t;
#define PSA_SUCCESS 0
#define PSA_ERROR_BAD_STATE (-137)
#define PSA_ALG_SHA_256 0x02000009
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(unsigned int, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
