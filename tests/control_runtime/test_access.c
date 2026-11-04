// SPDX-License-Identifier: Apache-2.0
/* Production grant storage/authentication and runtime authorities. Only NVS,
 * trusted enrollment, trusted time and live owner observations are replaced. */
#include <assert.h>
#include <stdlib.h>
#include "control_json.h"
#include "../../firmware/main/app_control_access.c"
#include "../../firmware/main/app_control_runtime.h"
#include "weak_heap.h"

static uint64_t clock_ms = 100, wall = 1800000000;
static bool clock_known = true, owner_alive = true, apply_write = true;
static void (*during_write)(void), (*during_open)(void);
static cp_principal_t native[CP_MAX_PRINCIPALS];
static size_t native_count;
static app_control_trust_snapshot_t trusted;
static app_update_snapshot_t update;
static cp_capture_grant_t capture;
static uint8_t banks[2][BANK_BYTES];
static size_t sizes[2];
static esp_err_t write_error, read_error, commit_error, open_error;
static unsigned writes, critical, lock_entries, interleave_at;
static void (*interleave)(void);
void test_enter(void)
{
    if (++lock_entries == interleave_at && interleave) {
        void (*run)(void) = interleave; interleave = NULL; run();
    }
    assert(!critical); ++critical;
}
void test_leave(void) { assert(critical == 1); --critical; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
void *heap_caps_aligned_calloc(size_t a, size_t n, size_t s, unsigned caps)
{ return test_heap_calloc(a, n, s, caps); }
void heap_caps_free(void *p) { test_heap_free(p); }
bool app_clock_wall_time(uint64_t *out) { *out = wall; return clock_known; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out) { *out = trusted; }
size_t app_control_trust_principals(cp_principal_t out[CP_MAX_PRINCIPALS], char digest[65])
{
    memcpy(out, native, sizeof(native));
    snprintf(digest, 65, "%s", trusted.known && !trusted.uncertain ? trusted.active_digest : "");
    return native_count;
}
bool app_control_trust_update_policy(const uint8_t id[16], app_update_policy_t *out)
{ (void)id; (void)out; return false; /* No old OTA policy is available. */ }
bool app_control_trust_grant(const uint8_t id[16], cp_capture_grant_t *out)
{ if (memcmp(id, capture.id, 16)) return false; *out = capture; return true; }
void app_control_server_snapshot(app_control_server_snapshot_t *out)
{ *out = (app_control_server_snapshot_t){0}; strcpy(out->address, "https://device.test"); }
bool app_control_server_client_connected(const uint8_t id[16]) { return owner_alive && id[0]; }
bool app_control_runtime_session_alive(const uint8_t session[16], const uint8_t id[16])
{ return owner_alive && session[0] && id[0]; }
void app_update_snapshot(app_update_snapshot_t *out) { *out = update; }
esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{
    assert(!strcmp(partition, "control") && !strcmp(ns, "ctrl_access") && mode == NVS_READWRITE);
    assert(!critical);
    if (during_open) { void (*run)(void) = during_open; during_open = NULL; run(); }
    *out = 1; return open_error;
}
static unsigned bank_index(const char *key)
{ assert(!strcmp(key, "access_a") || !strcmp(key, "access_b")); return !strcmp(key, "access_b"); }
esp_err_t nvs_get_blob(nvs_handle_t n, const char *key, void *out, size_t *size)
{
    assert(n == 1 && !critical); unsigned i = bank_index(key);
    if (read_error) return read_error;
    if (!sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= sizes[i]); memcpy(out, banks[i], sizes[i]); *size = sizes[i]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t n, const char *key, const void *p, size_t size)
{
    assert(n == 1 && size == BANK_BYTES && !critical); ++writes;
    unsigned i = bank_index(key);
    if (apply_write) { memcpy(banks[i], p, size); sizes[i] = size; }
    if (during_write) { void (*run)(void) = during_write; during_write = NULL; run(); }
    return write_error; /* A failed call can still have reached flash. */
}
esp_err_t nvs_commit(nvs_handle_t n) { assert(n == 1 && !critical); return commit_error; }
void nvs_close(nvs_handle_t n) { assert(n == 1); }
static uint64_t now_ms(void) { return clock_ms; }
#include "access_authority_fixture.inc"

static void boot(void)
{
    if (access_state) { heap_caps_free(access_state); access_state = NULL; }
    assert(app_control_access_init() == ESP_OK);
}
static void reset(void)
{
    memset(banks, 0, sizeof(banks)); memset(sizes, 0, sizeof(sizes));
    write_error = read_error = commit_error = open_error = ESP_OK; writes = 0;
    trusted = (app_control_trust_snapshot_t){.known = true, .present = true, .revision = 9};
    memset(trusted.active_digest, 'a', 64); trusted.active_digest[64] = 0;
    native_count = 1; memset(native, 0, sizeof(native));
    native[0] = (cp_principal_t){.enabled = true, .native_only = true, .scopes = CP_SCOPE_ALL};
    memset(native[0].id, 0x11, 16); memset(native[0].credential_sha256, 0x22, 32);
    clock_known = owner_alive = apply_write = true; during_write = during_open = NULL; wall = 1800000000; clock_ms = 100;
    boot();
}
static void proposal(unsigned tag, char id[33], char secret[65], char hash[65], uint64_t *request)
{
    uint8_t bytes[32], client[16], digest[32];
    memset(client, (int)tag, 16); memset(bytes, (int)(tag + 1), 32);
    cp_sha256(bytes, sizeof(bytes), digest);
    hex(client, 16, id); hex(bytes, 32, secret); hex(digest, 32, hash);
    assert(app_control_access_pair(id, hash, "Same browser name", request) == CP_OK);
}
static app_control_access_snapshot_t view(void)
{ app_control_access_snapshot_t s; app_control_access_snapshot(&s); return s; }
static app_control_access_request_t approval(uint8_t permissions_value, app_control_access_duration_t duration)
{
    app_control_access_snapshot_t s = view(); assert(s.known && s.pending);
    app_control_access_request_t r = {.action = APP_CONTROL_ACCESS_APPROVE,
        .expected_revision = s.revision, .pairing_request = s.pairing_request,
        .permissions = permissions_value, .duration = duration};
    memcpy(r.client_id, s.proposal.id, 16); return r;
}
static void apply(app_control_access_request_t r)
{
    uint64_t receipt; assert(app_control_access_submit(&r, &receipt) == CP_OK);
    assert(view().receipt_state == CP_RESULT_ACCEPTED);
    app_control_access_service(true); app_control_access_snapshot_t s = view();
    assert(s.receipt_request == receipt && s.receipt_state == CP_RESULT_APPLIED);
}
static void pairing_duration_and_storage(void)
{
    reset(); cp_principal_t p; assert(app_control_access_principal(native[0].id, &p));
    assert(p.native_only && p.scopes == CP_SCOPE_ALL && p.enabled);
    char id[33], secret[65], hash[65], json[2048]; size_t length; uint64_t request, repeat;
    proposal(0x33, id, secret, hash, &request);
    app_control_access_snapshot_t s = view();
    assert(s.proposal.permissions == APP_CONTROL_ACCESS_INPUT && s.proposal.duration == APP_CONTROL_ACCESS_SIX_HOURS);
    assert(!s.proposal.revoked && !s.proposal.expired);
    ++clock_ms; assert(app_control_access_pair(id, hash, "Same browser name", &repeat) == CP_OK && repeat == request);
    assert(view().pairing_remaining_ms == APP_CONTROL_TRUST_WINDOW_MS - 1);
    assert(app_control_access_pair_status(id, secret, json, sizeof(json), &length) == CP_OK && strstr(json, "pending"));
    secret[0] = secret[0] == '0' ? '1' : '0';
    assert(app_control_access_pair_status(id, secret, json, sizeof(json), &length) == CP_UNAUTHENTICATED);
    uint8_t bytes[32]; memset(bytes, 0x34, 32); hex(bytes, 32, secret);
    apply(approval(APP_CONTROL_ACCESS_INPUT | APP_CONTROL_ACCESS_MANAGE, APP_CONTROL_ACCESS_SIX_HOURS));
    assert(sizes[0] && !sizes[1]);
    assert(app_control_access_pair_status(id, secret, json, sizeof(json), &length) == CP_OK && strstr(json, "approved"));
    assert(!strstr(json, "audio.capture") && !strstr(json, "recording.control"));
    uint8_t client[16]; memset(client, 0x33, 16);
    assert(app_control_access_principal(client, &p) && p.enabled && !p.native_only && !(p.scopes & CP_SCOPE_AUDIO_CAPTURE));
    boot(); assert(!view().pending && app_control_access_principal(client, &p) && p.enabled);
    clock_known = false; assert(app_control_access_principal(client, &p) && !p.enabled && view().clients[1].clock_unknown);
    clock_known = true; wall += 21600;
    assert(app_control_access_principal(client, &p) && !p.enabled && view().clients[1].expired);
    uint64_t expired_at = wall;
    --wall;
    assert(app_control_access_principal(client, &p) && !p.enabled);
    assert(app_control_access_pair_status(id, secret, json, sizeof(json), &length) == CP_OK && strstr(json, "expired"));
    app_control_access_service(true);
    printf("Fixed: six-hour grant expired at %llu, remains expired at %llu after a -1s anchor change.\n",
        (unsigned long long)expired_at, (unsigned long long)wall);
    boot();
    assert(app_control_access_principal(client, &p) && !p.enabled);
    wall = expired_at;
    s = view(); app_control_access_request_t edit = {.action = APP_CONTROL_ACCESS_EDIT,
        .expected_revision = s.revision, .permissions = APP_CONTROL_ACCESS_INPUT, .duration = APP_CONTROL_ACCESS_DAY};
    memcpy(edit.client_id, client, 16); apply(edit); assert(sizes[0] && sizes[1]);
    assert(view().clients[1].expires_unix_seconds == wall + 86400);
    edit.expected_revision = view().revision; edit.duration = APP_CONTROL_ACCESS_PERMANENT;
    clock_known = false; apply(edit); boot();
    assert(app_control_access_principal(client, &p) && p.enabled && !view().clients[1].clock_unknown);
    edit.action = APP_CONTROL_ACCESS_REVOKE; edit.expected_revision = view().revision; apply(edit); boot();
    assert(app_control_access_principal(client, &p) && !p.enabled && view().clients[1].revoked);
}
static void races_expiry_and_faults(void)
{
    reset(); char id[33], secret[65], hash[65]; uint64_t request, receipt;
    proposal(0x33, id, secret, hash, &request);
    app_control_access_request_t r = approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_SIX_HOURS);
    assert(app_control_access_submit(&r, &receipt) == CP_OK);
    app_control_access_service(false); assert(view().receipt_error == CP_LOCKED && !writes);
    clock_known = false;
    assert(app_control_access_submit(&r, &receipt) == CP_OK); app_control_access_service(true);
    assert(view().receipt_error == CP_UNAVAILABLE && !writes && view().known); clock_known = true;
    clock_ms += APP_CONTROL_TRUST_WINDOW_MS;
    assert(app_control_access_submit(&r, &receipt) == CP_OK);
    app_control_access_service(true); assert(view().receipt_error == CP_STALE && !writes);
    proposal(0x33, id, secret, hash, &request); r = approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_PERMANENT);
    apply(r);
    r.action = APP_CONTROL_ACCESS_REVOKE; r.expected_revision = view().revision;
    assert(app_control_access_submit(&r, &receipt) == CP_OK);
    /* Finish a real queued save at the second snapshot critical-section entry. */
    interleave_at = lock_entries + 2; interleave = NULL;
    /* Hook assigned below to keep the worker callback signature explicit. */
    extern void test_save_between_snapshot_locks(void);
    interleave = test_save_between_snapshot_locks;
    app_control_access_snapshot_t s = view(); assert(!s.known && !s.count);
    s = view(); assert(s.known && s.clients[1].revoked);
    r.action = APP_CONTROL_ACCESS_EDIT; r.expected_revision = s.revision; r.permissions = APP_CONTROL_ACCESS_INPUT;
    open_error = ESP_FAIL; unsigned previous_writes = writes;
    assert(app_control_access_submit(&r, &receipt) == CP_OK); app_control_access_service(true);
    assert(view().known && !view().uncertain && view().receipt_error == CP_UNAVAILABLE && writes == previous_writes);
    open_error = ESP_OK; write_error = ESP_FAIL;
    assert(app_control_access_submit(&r, &receipt) == CP_OK); app_control_access_service(true);
    s = view(); assert(s.uncertain && !s.known && s.receipt_error == CP_STORAGE_UNCERTAIN);
    cp_principal_t p; assert(!app_control_access_principal(r.client_id, &p));
    write_error = ESP_OK; boot(); /* Readback now establishes what reached storage. */
    assert(app_control_access_principal(r.client_id, &p) && p.enabled);
    app_control_access_request_t legacy = r; legacy.expected_revision = view().revision;
    memcpy(legacy.client_id, native[0].id, 16); apply(legacy);
    native[0].credential_sha256[0] ^= 1; /* Same ID is insufficient identity. */
    s = view(); assert(!s.known && !s.count);
    native[0].credential_sha256[0] ^= 1;
    r.expected_revision = view().revision;
    assert(app_control_access_submit(&r, &receipt) == CP_OK);
    memset(trusted.active_digest, 'b', 64); app_control_access_service(true);
    assert(view().receipt_state == CP_RESULT_FAILED && view().receipt_error == CP_STALE);
    assert(view().known && view().count == 1 && !app_control_access_principal(r.client_id, &p));
    assert(app_control_access_principal(native[0].id, &p) && p.scopes == CP_SCOPE_ALL);
    read_error = ESP_FAIL; assert(app_control_access_init() == ESP_FAIL && view().uncertain);
}
void test_save_between_snapshot_locks(void) { app_control_access_service(true); }
static void runtime_update_and_capture(void)
{
    reset(); app_control_access_request_t r = {.action = APP_CONTROL_ACCESS_EDIT,
        .expected_revision = view().revision, .permissions = APP_CONTROL_ACCESS_UPDATE,
        .duration = APP_CONTROL_ACCESS_PERMANENT};
    memcpy(r.client_id, native[0].id, 16); apply(r);
    cp_principal_t principal; assert(app_control_access_principal(r.client_id, &principal));
    assert(principal.native_only && !(principal.scopes & CP_SCOPE_AUDIO_CAPTURE));
    app_update_policy_t policy; assert(update_policy(r.client_id, &policy));
    assert(policy.expires_unix_seconds == UINT64_MAX && policy.maximum_job_ms == 600000 && policy.allow_signed_releases);
    assert(!policy.continue_after_owner_loss && policy.maximum_bytes == APP_UPDATE_SLOT_BYTES);
    update = (app_update_snapshot_t){.available = true, .trust_revision = trusted.revision, .total_bytes = 1024};
    strcpy(update.digest, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); strcpy(update.release_id, "release-1");
    char client[33]; hex(r.client_id, 16, client);
    cp_command_t command = {.operation = CP_OP_FIRMWARE_DOWNLOAD, .deadline_ms = clock_ms + 1000,
        .identity = {.scopes = principal.scopes}};
    memcpy(command.identity.principal_id, r.client_id, 16); memset(command.identity.session_id, 0x44, 16);
    snprintf(command.args_json, sizeof(command.args_json), "{\"policy_id\":\"%s\",\"policy_revision\":\"%llu\",\"digest\":\"%s\",\"release_id\":\"release-1\"}", client, (unsigned long long)policy.revision, update.digest);
    app_update_authority_t authority;
    assert(app_control_runtime_update_authority(&command, &authority) == CP_OK && authority.authorized);
    clock_known = false;
    assert(app_control_access_principal(r.client_id, &principal) && principal.enabled);
    assert(app_control_runtime_update_authority(&command, &authority) == CP_UNAVAILABLE);
    clock_known = true; owner_alive = false;
    assert(app_control_runtime_update_authority(&command, &authority) == CP_UNAUTHENTICATED); owner_alive = true;
    r.action = APP_CONTROL_ACCESS_REVOKE; r.expected_revision = view().revision; apply(r);
    assert(app_control_runtime_update_authority(&command, &authority) == CP_FORBIDDEN);
    cp_capture_authority_request_t request = {.purpose = CP_CAPTURE_RECORDING, .identity = command.identity,
        .duration_seconds = 1, .bytes = 100, .file_destination = true, .grant_revision = 1};
    request.identity.scopes = CP_SCOPE_ALL;
    capture = (cp_capture_grant_t){.purpose = CP_CAPTURE_RECORDING, .revision = 1,
        .expires_unix_seconds = wall + 3600, .maximum_duration_seconds = 10, .maximum_bytes = 1000,
        .file_destination = true};
    memset(capture.id, 0x55, 16); memcpy(capture.principal_id, r.client_id, 16); memcpy(request.grant_id, capture.id, 16);
    cp_capture_authority_t capture_authority;
    assert(app_control_runtime_capture_authority(&request, &capture_authority) == CP_FORBIDDEN && !capture_authority.allowed);
}
static void finite_expiry_is_terminal_and_renewal_is_explicit(void)
{
    for (unsigned duration = APP_CONTROL_ACCESS_SIX_HOURS; duration <= APP_CONTROL_ACCESS_DAY; ++duration) {
        reset(); char client[33], secret[65], digest[65], json[2048]; size_t length; uint64_t request;
        proposal(0x33, client, secret, digest, &request);
        apply(approval(APP_CONTROL_ACCESS_INPUT, (app_control_access_duration_t)duration));
        uint8_t id[16]; memset(id, 0x33, 16); cp_principal_t p;
        access_record_t previous = access_state->saved[0];
        wall = previous.expires - 1;
        boot();
        assert(app_control_access_principal(id, &p) && p.enabled);
        wall = previous.expires; unsigned before = writes;
        assert(app_control_access_principal(id, &p) && !p.enabled && writes == before);
        --wall; clock_known = false;
        assert(app_control_access_principal(id, &p) && !p.enabled);
        assert(app_control_access_pair_status(client, secret, json, sizeof(json), &length) == CP_OK && strstr(json, "expired"));
        clock_known = true;
        assert(app_control_access_principal(id, &p) && !p.enabled);
        /* A locally queued renewal owns the next write. No old pending expiry
         * transaction can overwrite its newly approved grant revision. */
        app_control_access_request_t edit = {.action = APP_CONTROL_ACCESS_EDIT,
            .expected_revision = view().revision, .permissions = APP_CONTROL_ACCESS_INPUT,
            .duration = (app_control_access_duration_t)duration}; memcpy(edit.client_id, id, 16);
        apply(edit); assert(app_control_access_principal(id, &p) && p.enabled);
        assert(!usable(&previous, true, previous.expires));
        assert(app_control_access_principal(id, &p) && p.enabled);
        assert(!access_state->expiry_pending); before = writes; app_control_access_service(true); assert(writes == before);
        wall = access_state->saved[0].expires;
        assert(app_control_access_principal(id, &p) && !p.enabled);
        app_control_access_service(true); assert(writes == before + 1);
        --wall; boot(); assert(app_control_access_principal(id, &p) && !p.enabled && view().clients[1].expired);
        clock_known = false; assert(view().clients[1].expired && !view().clients[1].clock_unknown);
        clock_known = true;
    }
    /* A permanent grant neither expires nor causes expiry writes. */
    reset(); char client[33], secret[65], digest[65]; uint64_t request;
    proposal(0x33, client, secret, digest, &request); apply(approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_PERMANENT));
    unsigned before = writes; wall += 1000000; clock_known = false;
    cp_principal_t p; uint8_t id[16]; memset(id, 0x33, 16);
    assert(app_control_access_principal(id, &p) && p.enabled); app_control_access_service(true);
    assert(writes == before && !access_state->expiry_pending); boot();
    assert(app_control_access_principal(id, &p) && p.enabled);
}
static void expiry_write_failures_keep_this_boot_closed(void)
{
    for (unsigned phase = 0; phase < 4; ++phase) {
        reset(); char client[33], secret[65], digest[65]; uint64_t request;
        proposal(0x33, client, secret, digest, &request); apply(approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_DAY));
        wall += 86400; uint8_t id[16]; memset(id, 0x33, 16); cp_principal_t p;
        unsigned before = writes; assert(app_control_access_principal(id, &p) && !p.enabled && writes == before); --wall;
        if (phase == 0) open_error = ESP_FAIL;
        if (phase == 1) { write_error = ESP_FAIL; apply_write = false; }
        if (phase == 2) commit_error = ESP_FAIL;
        if (phase == 3) read_error = ESP_FAIL;
        app_control_access_service(true); assert(!view().known && view().uncertain && view().error == ESP_FAIL);
        before = writes; app_control_access_service(true); assert(writes == before); /* No periodic retry writes. */
        /* A failed explicit reload must also retain this boot's expiry latch. */
        read_error = ESP_FAIL; assert(app_control_access_init() == ESP_FAIL);
        open_error = write_error = commit_error = read_error = ESP_OK; apply_write = true;
        assert(app_control_access_init() == ESP_OK);
        assert(app_control_access_principal(id, &p) && !p.enabled);
        app_control_access_service(true); boot();
        assert(app_control_access_principal(id, &p) && !p.enabled && view().clients[1].expired);
    }
}
static uint8_t expire_during_write_id[16];
static void observe_other_expiry(void)
{
    cp_principal_t p;
    assert(app_control_access_principal(expire_during_write_id, &p) && !p.enabled);
}
static void concurrent_expiry_is_not_lost_during_storage(void)
{
    reset(); char client[33], secret[65], digest[65]; uint64_t request;
    proposal(0x33, client, secret, digest, &request); apply(approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_SIX_HOURS));
    ++wall; proposal(0x44, client, secret, digest, &request); apply(approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_SIX_HOURS));
    uint8_t first[16]; memset(first, 0x33, 16); memset(expire_during_write_id, 0x44, 16); cp_principal_t p;
    wall += 21600; assert(app_control_access_principal(first, &p) && !p.enabled);
    during_write = observe_other_expiry; app_control_access_service(true);
    assert(access_state->expiry_pending); --wall;
    assert(app_control_access_principal(expire_during_write_id, &p) && !p.enabled);
    app_control_access_service(true); assert(!access_state->expiry_pending); boot();
    assert(app_control_access_principal(first, &p) && !p.enabled);
    assert(app_control_access_principal(expire_during_write_id, &p) && !p.enabled);
}

static bool edit_observer_expects_expiry;
static unsigned edit_observations;
static void observe_suspended_edit(void)
{
    ++wall;
    unsigned before = writes;
    app_control_access_snapshot_t s = view();
    assert(s.known && s.count == 2 && !s.clients[1].enabled);
    assert(s.clients[1].expired == edit_observer_expects_expiry);
    cp_principal_t p;
    assert(app_control_access_principal(s.clients[1].id, &p) && !p.enabled);
    assert(writes == before); /* The observer never performs storage I/O. */
    ++edit_observations;
}
static void suspended_expiry_survives_failed_edit_but_not_explicit_renewal(void)
{
    for (unsigned duration = APP_CONTROL_ACCESS_SIX_HOURS; duration <= APP_CONTROL_ACCESS_PERMANENT; ++duration)
    for (unsigned crosses_expiry = 0; crosses_expiry < (duration == APP_CONTROL_ACCESS_PERMANENT ? 1U : 2U); ++crosses_expiry)
    for (unsigned succeeds = 0; succeeds < 2; ++succeeds) {
        reset(); char client[33], secret[65], digest[65], json[2048]; uint64_t request; size_t length;
        proposal(0x33, client, secret, digest, &request);
        apply(approval(APP_CONTROL_ACCESS_INPUT, (app_control_access_duration_t)duration));
        app_control_access_snapshot_t initial = view();
        access_record_t previous = access_state->saved[0];
        app_control_access_request_t edit = {.action = APP_CONTROL_ACCESS_EDIT,
            .expected_revision = initial.revision, .permissions = APP_CONTROL_ACCESS_MANAGE,
            .duration = APP_CONTROL_ACCESS_DAY};
        memcpy(edit.client_id, initial.clients[1].id, 16);
        if (duration != APP_CONTROL_ACCESS_PERMANENT)
            wall = previous.expires - (crosses_expiry ? 1U : 2U);
        unsigned before = writes;
        assert(app_control_access_submit(&edit, &request) == CP_OK);
        edit_observations = 0; edit_observer_expects_expiry = crosses_expiry != 0;
        during_open = observe_suspended_edit; open_error = succeeds ? ESP_OK : ESP_FAIL;
        app_control_access_service(true); open_error = ESP_OK;
        assert(edit_observations == 1);
        --wall; /* Same boot, no reload, no successful renewal in failure cases. */
        app_control_access_snapshot_t s = view(); cp_principal_t p;
        assert(s.known && !s.uncertain && app_control_access_principal(edit.client_id, &p));
        assert(app_control_access_pair_status(client, secret, json, sizeof(json), &length) == CP_OK);
        if (succeeds) {
            assert(s.receipt_state == CP_RESULT_APPLIED && s.clients[1].revision > previous.revision);
            assert(p.enabled && !s.clients[1].expired && strstr(json, "approved"));
            assert((p.scopes & CP_SCOPE_CONFIG_WRITE) && !(p.scopes & CP_SCOPE_INPUT_CONTROL));
            if (crosses_expiry) assert(!usable(&previous, true, previous.expires));
            assert(app_control_access_principal(edit.client_id, &p) && p.enabled && !access_state->expiry_pending);
            assert(writes == before + 1);
        } else {
            assert(s.receipt_state == CP_RESULT_FAILED && s.receipt_error == CP_UNAVAILABLE);
            assert(s.clients[1].revision == previous.revision && writes == before);
            assert(p.enabled == !crosses_expiry && s.clients[1].expired == (crosses_expiry != 0));
            assert(strstr(json, crosses_expiry ? "expired" : "approved"));
        }
    }
}

static unsigned random_value, revoked_sessions, input_revocations, dispatch_count;
static cp_command_t dispatched;
static bool random_source(void *user, uint8_t *out, size_t n)
{ (void)user; for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)++random_value; return true; }
static bool principal_source(void *user, const uint8_t id[16], cp_principal_t *out)
{ (void)user; return app_control_access_principal(id, out); }
static bool input_authorized(void *user, const cp_identity_t *identity)
{ (void)user; return app_control_access_input(identity); }
static bool available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{ (void)user; (void)op; reason[0] = 0; return true; }
static cp_error_t dispatch(void *user, const cp_command_t *command, cp_result_t *out)
{ (void)user; ++dispatch_count; dispatched = *command; *out = (cp_result_t){.state = CP_RESULT_ACCEPTED}; return CP_OK; }
static void safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    (void)user; (void)identity;
    if (reason == CP_SAFETY_SESSION_LOST || reason == CP_SAFETY_REVOKED) ++revoked_sessions;
    else ++input_revocations;
}
static void field(const char *json, const char *name, char *out, size_t size)
{
    cj_document_t *d = malloc(sizeof(*d)); assert(d && cj_parse(d, json, strlen(json)));
    const char *s = cj_string(d, cj_get(d, 0, name)); assert(s && strlen(s) < size);
    strcpy(out, s); free(d);
}
static void protocol_pairing_and_live_revocation(void)
{
    reset(); random_value = revoked_sessions = input_revocations = dispatch_count = 0;
    cp_callbacks_t callbacks = {.random = random_source, .principal = principal_source,
        .input_authorized = input_authorized, .pairing = pairing, .available = available,
        .dispatch = dispatch, .safety = safety};
    cp_context_t *context; void *memory = calloc(1, cp_context_size()); assert(memory);
    assert(cp_initialize(memory, cp_context_size(), &callbacks, &context) == CP_OK);
    assert(cp_http_configure(context, "device.test", "https://device.test") == CP_OK);
    cp_device_state_t state = {.management_generation = 1, .input_epoch = 2, .transport_generation = 3,
        .ui_revision = 4, .state_sequence = 5, .allowance_generation = 1, .input_ready = true};
    strcpy(state.target_id, "test-target"); strcpy(state.mode, "mouse"); cp_set_device_state(context, &state, clock_ms);
    cp_http_security_t security = {.tls = true, .host = "device.test", .origin = "https://device.test"};
    cp_http_response_t *response = calloc(1, sizeof(*response)); assert(response);
    char client[33], secret[65], digest[65], body[2048];
    uint8_t bytes[32], id[16], hash[32]; memset(bytes, 0x34, 32); memset(id, 0x33, 16);
    cp_sha256(bytes, 32, hash); hex(bytes, 32, secret); hex(id, 16, client); hex(hash, 32, digest);
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"credential_sha256\":\"%s\",\"label\":\"Browser\"}", client, digest);
    security.origin = NULL;
    assert(cp_http(context, "POST", "/api/v1/pairing/requests", &security, body, strlen(body), clock_ms, response) == CP_FORBIDDEN);
    security.origin = "https://evil.test";
    assert(cp_http(context, "POST", "/api/v1/pairing/requests", &security, body, strlen(body), clock_ms, response) == CP_FORBIDDEN);
    security.origin = "https://device.test";
    assert(cp_http(context, "POST", "/api/v1/pairing/requests", &security, body, strlen(body), clock_ms, response) == CP_OK && response->status == 202);
    assert(strstr(response->body, digest));
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"credential\":\"%s\"}", client, secret);
    assert(cp_http(context, "POST", "/api/v1/pairing/status", &security, body, strlen(body), clock_ms, response) == CP_OK && strstr(response->body, "pending"));
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms, response) == CP_UNAUTHENTICATED);
    apply(approval(APP_CONTROL_ACCESS_INPUT, APP_CONTROL_ACCESS_PERMANENT));
    char cookie[256], old_cookie[256], csrf[33], session[33], prefix[17], boot_id[33];
    assert(cp_http(context, "GET", "/api/v1/login-challenge", &security, NULL, 0, clock_ms, response) == CP_OK);
    strcpy(cookie, response->set_cookie); field(response->body, "csrf", csrf, sizeof(csrf));
    security.cookie = cookie; security.csrf = csrf;
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms, response) == CP_OK);
    assert(strstr(response->body, "input.control") && !strstr(response->body, "config.write"));
    strcpy(cookie, response->set_cookie); strcpy(old_cookie, cookie);
    field(response->body, "csrf", csrf, sizeof(csrf)); field(response->body, "session_id", session, sizeof(session));
    field(response->body, "request_prefix", prefix, sizeof(prefix)); field(response->body, "boot_id", boot_id, sizeof(boot_id));
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, response) == CP_OK && strstr(response->body, "\"remote_allowed\":true"));
    snprintf(body, sizeof(body), "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s0000000000000001\",\"seq\":1,\"deadline_ms\":\"%llu\",\"op\":\"control.acquire\",\"args\":{\"state_sequence\":\"5\"},\"epoch\":\"2\",\"transport_generation\":\"3\",\"target_id\":\"test-target\",\"ui_revision\":\"4\"}", boot_id, session, prefix, (unsigned long long)(clock_ms + 100));
    assert(cp_http(context, "POST", "/api/v1/commands", &security, body, strlen(body), clock_ms, response) == CP_OK && dispatch_count == 1);
    state.remote_owner = true; memcpy(state.owner_session, dispatched.identity.session_id, 16);
    ++state.input_epoch; cp_set_device_state(context, &state, clock_ms);
    cp_result_t result = {.state = CP_RESULT_APPLIED, .uncertain_index = -1};
    assert(cp_complete(context, dispatched.command_key, &result, clock_ms) == CP_OK);
    cp_physical_override(context, false, clock_ms); assert(input_revocations);
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, response) == CP_OK && strstr(response->body, "\"remote_allowed\":true") && !strstr(response->body, "lease_id"));
    /* Reload authenticates again, invalidates only this client's old session,
     * and requires a fresh explicit acquire. It cannot restore the old lease. */
    assert(cp_http(context, "GET", "/api/v1/login-challenge", &security, NULL, 0, clock_ms, response) == CP_OK);
    strcpy(cookie, response->set_cookie); field(response->body, "csrf", csrf, sizeof(csrf));
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"credential\":\"%s\"}", client, secret);
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms, response) == CP_OK && revoked_sessions == 1);
    strcpy(cookie, response->set_cookie); field(response->body, "csrf", csrf, sizeof(csrf));
    cp_http_security_t old = security; old.cookie = old_cookie;
    assert(cp_http(context, "GET", "/api/v1/state", &old, NULL, 0, clock_ms, response) == CP_UNAUTHENTICATED);
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, response) == CP_OK && !strstr(response->body, "lease_id"));
    app_control_access_request_t revoke = {.action = APP_CONTROL_ACCESS_REVOKE, .expected_revision = view().revision};
    memcpy(revoke.client_id, id, 16); apply(revoke);
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, response) == CP_UNAUTHENTICATED && revoked_sessions == 2);
    free(response); free(memory);
}
int main(void)
{
    suspended_expiry_survives_failed_edit_but_not_explicit_renewal();
    finite_expiry_is_terminal_and_renewal_is_explicit(); expiry_write_failures_keep_this_boot_closed();
    concurrent_expiry_is_not_lost_during_storage();
    pairing_duration_and_storage(); races_expiry_and_faults(); runtime_update_and_capture(); protocol_pairing_and_live_revocation();
    heap_caps_free(access_state); access_state = NULL; assert(!test_heap_live && !critical);
    puts("PASS: production credential pairing, durable duration/revoke, coherent snapshots, native policy migration and runtime update/capture authority");
    return 0;
}
