// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_control_trust.h"
#include <string.h>

/* Synthetic public metadata only. Maximal mode exercises the public Review
 * capacity; package-parser size validation is a separate trust-owner test. */
static inline void trust_review_fixture(app_control_trust_review_t *r, bool maximal)
{
    memset(r, 0, sizeof(*r));
    memset(r->digest, 'a', 64); strcpy(r->host, "synthetic-device.example"); r->port = 8443;
    memset(r->certificate_pem_sha256, 'b', 64);
    r->clock_server_count = 2; strcpy(r->clock_servers[0], "clock-one.example"); strcpy(r->clock_servers[1], "clock-two.example");
    r->principal_count = maximal ? CP_MAX_PRINCIPALS : 1;
    for (unsigned i = 0; i < r->principal_count; ++i) {
        r->principal_scopes[i] = maximal ? CP_SCOPE_ALL : CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL;
        r->principals[i].id[0] = (uint8_t)(i + 1); r->principals[i].scopes = r->principal_scopes[i];
        r->principals[i].enabled = true; r->principals[i].native_only = true;
    }
    r->capture_grant_count = maximal ? CP_MAX_CAPTURE_GRANTS : 1;
    for (unsigned i = 0; i < r->capture_grant_count; ++i) {
        cp_capture_grant_t *g = &r->grants[i]; g->id[0] = (uint8_t)(i + 17); g->principal_id[0] = 1;
        g->revision = maximal ? UINT64_MAX : 7; g->purpose = CP_CAPTURE_RECORDING;
        g->expires_unix_seconds = maximal ? UINT64_MAX : UINT64_C(1900000000);
        g->maximum_duration_seconds = maximal ? UINT32_MAX : 60;
        g->maximum_bytes = maximal ? UINT64_MAX : 524288;
        g->maximum_retention_seconds = maximal ? UINT32_MAX : 3600;
        g->live_destination = g->file_destination = g->owner_download = true;
    }
    r->update_source_present = true;
    strcpy(r->update_catalog_url, "https://updates.example/releases.json"); strcpy(r->update_signing_key_id, "synthetic-signing-key");
    memset(r->update_public_key_sha256, 'c', 64); memset(r->update_tls_ca_sha256, 'd', 64);
    strcpy(r->update_product, "esp32-input"); strcpy(r->update_board, "mosaico"); strcpy(r->update_layout, "ota-v2");
    r->update_minimum_release_index = maximal ? UINT32_MAX : 42; r->update_allow_private_source = true;
    r->update_policy_present = true;
    r->update_policy = (app_update_policy_t){.id = {33}, .principal_id = {1}, .revision = maximal ? UINT64_MAX : 9,
        .expires_unix_seconds = maximal ? UINT64_MAX : UINT64_C(1900000001),
        .maximum_job_ms = maximal ? UINT32_MAX : 120000, .maximum_bytes = maximal ? UINT32_MAX : 5242880,
        .allow_download = true, .allow_activate = true, .allow_restart = true, .continue_after_owner_loss = true,
        .allow_signed_releases = true};
    memset(r->update_policy.allowed_digest, 'e', 64);
    if (maximal) {
        memset(r->clock_servers[0], 'x', 128); memset(r->clock_servers[1], 'y', 128);
        memset(r->update_catalog_url, 'u', 511); memcpy(r->update_catalog_url, "https://", 8);
        memset(r->update_signing_key_id, 'k', 64); memset(r->update_product, 'p', 64);
        memset(r->update_board, 'q', 64); memset(r->update_layout, 'r', 64);
    }
}

/* Public snapshot from a 7,735-byte synthetic enrollment accepted by the trust
 * parser with four principals, all 25 scopes, eight grants and source/policy.
 * A fresh real P-256 certificate/key pair was checked separately. No credentials
 * or private keys are stored here; parser/crypto tests remain trust-owned. */
static inline void trust_package_review_fixture(app_control_trust_review_t *r)
{
    trust_review_fixture(r, false);
    r->replace_unknown_store = true;
    strcpy(r->digest, "fa4eac770e3aae58f086fd27996fb6eec5d55c9ea97c61af926fc17cb595e8fe");
    strcpy(r->certificate_pem_sha256, "2f31567fa25d2a381375dee3b27e0b083c48b6a09d1eac777f40488ee025e7b5");
    strcpy(r->update_public_key_sha256, "fca846e27892cffb65fcd58f52a584adb85e542a12e464cfd391da1630361bb2");
    strcpy(r->update_tls_ca_sha256, "2f31567fa25d2a381375dee3b27e0b083c48b6a09d1eac777f40488ee025e7b5");
    strcpy(r->clock_servers[0], "first-clock.example"); strcpy(r->clock_servers[1], "second-clock.example");
    r->principal_count = CP_MAX_PRINCIPALS; memset(r->principals, 0, sizeof(r->principals));
    for (unsigned i = 0; i < r->principal_count; ++i) {
        r->principal_scopes[i] = r->principals[i].scopes = CP_SCOPE_ALL;
        r->principals[i].id[CP_ID_BYTES - 1] = (uint8_t)(i + 1);
        r->principals[i].enabled = r->principals[i].native_only = true;
    }
    r->capture_grant_count = CP_MAX_CAPTURE_GRANTS; memset(r->grants, 0, sizeof(r->grants));
    const cp_capture_purpose_t purposes[] = {CP_CAPTURE_LIVE, CP_CAPTURE_RECORDING, CP_CAPTURE_BACKGROUND, CP_CAPTURE_SPEECH};
    for (unsigned i = 0; i < r->capture_grant_count; ++i) {
        cp_capture_grant_t *g = &r->grants[i];
        g->id[CP_ID_BYTES - 1] = (uint8_t)(i + 10); g->principal_id[CP_ID_BYTES - 1] = (uint8_t)(i % 4 + 1);
        g->revision = UINT64_MAX; g->expires_unix_seconds = UINT64_C(2000000000); g->purpose = purposes[i % 4];
        g->maximum_bytes = UINT64_C(8589934592); g->maximum_duration_seconds = i % 4 == 3 ? 8 : 28800;
        g->live_destination = i % 4 == 0 || i % 4 == 3;
        g->maximum_retention_seconds = g->live_destination ? 0 : 604800;
        g->file_destination = g->owner_download = !g->live_destination;
    }
    strcpy(r->update_catalog_url, "https://publisher.example/catalog.json");
    strcpy(r->update_signing_key_id, "synthetic-release-key"); strcpy(r->update_layout, "esp32s31-16m-ota6-v3");
    r->update_minimum_release_index = UINT32_MAX;
    memset(&r->update_policy, 0, sizeof(r->update_policy));
    memset(r->update_policy.id, 255, CP_ID_BYTES); r->update_policy.principal_id[CP_ID_BYTES - 1] = 1;
    r->update_policy.revision = UINT64_MAX; r->update_policy.expires_unix_seconds = UINT64_C(2000000000);
    r->update_policy.maximum_job_ms = 600000; r->update_policy.maximum_bytes = 6291456;
    r->update_policy.allow_download = r->update_policy.allow_activate = r->update_policy.allow_restart = true;
    r->update_policy.continue_after_owner_loss = true; memset(r->update_policy.allowed_digest, 'a', 64);
}
