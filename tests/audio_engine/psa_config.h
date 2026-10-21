// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Native build of the pinned SDK's real PSA primitives used by the recorder. */
#define PSA_WANT_ALG_GCM 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_KEY_TYPE_AES 1
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_PSA_BUILTIN_GET_ENTROPY
