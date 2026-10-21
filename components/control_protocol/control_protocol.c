// SPDX-License-Identifier: Apache-2.0
#include "control_protocol.h"
#include "control_registry.h"
#include "control_registry_internal.h"
#include <inttypes.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_IDLE_MS UINT64_C(900000)
#define SESSION_ABSOLUTE_MS UINT64_C(28800000)
#define STATE_FRESH_MS UINT64_C(1500)
#define LEASE_MAX_MS UINT64_C(3000)
#define WS_TICKET_MS UINT64_C(10000)

typedef struct {
    bool used, browser, commands_blocked;
    uint8_t id[16], token_hash[32], csrf[16], prefix[8], ws_ticket_hash[32];
    uint64_t ws_ticket_until;
    uint8_t principal;
    uint64_t generation, opened_at, last_seen, state_at, delivered_sequence;
    uint64_t rate_at, safety_at;
    uint32_t sequence, rate_tokens, safety_count;
} session_t;
typedef struct {
    bool used, active, revoked, transition_seen;
    char transition_mode[12];
    uint8_t session;
    uint64_t session_generation, key, input_epoch, lease_expiry;
    uint32_t sequence;
    uint8_t digest[32];
    cp_operation_id_t operation;
    cp_result_t result;
} record_t;
typedef struct {
    bool pending, active;
    uint8_t session, id[16];
    uint64_t session_generation, expires, epoch, acquire_key;
    char producer[16];
} lease_t;
struct cp_context {
    cp_callbacks_t callbacks;
    cp_principal_t principals[CP_MAX_PRINCIPALS];
    bool principal_used[CP_MAX_PRINCIPALS];
    session_t sessions[CP_MAX_SESSIONS];
    record_t records[CP_MAX_RESULTS];
    lease_t lease;
    cp_device_state_t state;
    uint8_t boot[16], challenge[16];
    uint64_t next_key, session_generation, now, challenge_until;
    uint64_t blocked_allowance, login_window;
    unsigned failed_logins;
    bool clock_failed, state_known, websocket_available, binary_download_available;
    char host[129], origin[193];
    cj_document_t document;
    char canonical[CJ_CAPACITY + 1U];
    cp_command_t command;
};
static void wipe(void *p, size_t length)
{
    volatile unsigned char *s = p;
    while (length--) {
        *s++ = 0;
    }
}
static bool equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        v |= x[i] ^ y[i];
    }
    return v == 0;
}
static bool nonzero(const uint8_t *v, size_t n)
{
    unsigned x = 0;
    for (size_t i = 0; i < n; ++i) {
        x |= v[i];
    }
    return x != 0;
}
static void encode(const uint8_t *bytes, size_t count, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = h[bytes[i] >> 4];
        out[i * 2 + 1] = h[bytes[i] & 15];
    }
    out[count * 2] = 0;
}
static bool decode(const char *s, uint8_t *bytes, size_t count)
{
    if (!s || strlen(s) != count * 2) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[i * 2 + j];
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = (unsigned)(c - 'a' + 10);
            } else {
                return false;
            }
            v = v * 16 + d;
        }
        bytes[i] = (uint8_t)v;
    }
    return true;
}
static bool random_bytes(cp_context_t *c, uint8_t *out, size_t length)
{
    if (!c->callbacks.random || !c->callbacks.random(c->callbacks.user, out, length) ||
        !nonzero(out, length)) {
        wipe(out, length);
        return false;
    }
    return true;
}
const char *cp_error_name(cp_error_t error)
{
    static const char *const names[] = {"ok",
                                        "invalid",
                                        "too_large",
                                        "unauthenticated",
                                        "forbidden",
                                        "unavailable",
                                        "not_implemented",
                                        "locked",
                                        "stale",
                                        "expired",
                                        "busy",
                                        "sequence_gap",
                                        "request_conflict",
                                        "result_unknown",
                                        "owner_busy",
                                        "local_busy",
                                        "release_pending",
                                        "storage_uncertain",
                                        "internal",
                                        "too_late"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "internal";
}
const char *cp_result_state_name(cp_result_state_t state)
{
    static const char *const names[] = {"rejected",  "accepted", "running",
                                        "applied",   "sent",     "completed",
                                        "cancelled", "failed",   "cleanup_pending"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "failed";
}
static bool active_result(cp_result_state_t s)
{
    return s == CP_RESULT_ACCEPTED || s == CP_RESULT_RUNNING || s == CP_RESULT_CLEANUP_PENDING;
}
static const cp_operation_info_t *operation(cp_operation_id_t id)
{
    return id ? cp_operation_at(id - 1U) : NULL;
}
static cp_identity_t identity(cp_context_t *c, unsigned index)
{
    session_t *s = &c->sessions[index];
    cp_identity_t i = {.scopes = c->principals[s->principal].scopes, .browser = s->browser};
    memcpy(i.principal_id, c->principals[s->principal].id, 16);
    memcpy(i.session_id, s->id, 16);
    return i;
}
static bool transition(cp_operation_id_t op)
{
    return op == CP_OP_MODE_SELECT || op == CP_OP_INPUT_MOTION_SET ||
           op == CP_OP_KEYBOARD_SUBMODE_SELECT;
}
static void revoke_records(cp_context_t *c, unsigned session, bool input_only, cp_error_t error)
{
    for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
        record_t *r = &c->records[i];
        const cp_operation_info_t *o = operation(r->operation);
        if (!r->used || !r->active || r->session != session ||
            r->session_generation != c->sessions[session].generation || !o ||
            (input_only && (!o->input || o->safety))) {
            continue;
        }
        r->revoked = true;
        r->result.state = CP_RESULT_CLEANUP_PENDING;
        r->result.error = error;
        snprintf(r->result.reason, sizeof(r->result.reason), "cancellation_requested");
    }
}
static void revoke_lease(cp_context_t *c, cp_safety_reason_t reason)
{
    if (!c->lease.active && !c->lease.pending) {
        return;
    }
    unsigned owner = c->lease.session;
    cp_identity_t i = identity(c, owner);
    revoke_records(c, owner, true, CP_STALE);
    wipe(&c->lease, sizeof(c->lease));
    if (c->callbacks.safety) {
        c->callbacks.safety(c->callbacks.user, reason, &i);
    }
}
static void close_session(cp_context_t *c, unsigned index, cp_safety_reason_t reason)
{
    session_t *s = &c->sessions[index];
    if (!s->used) {
        return;
    }
    cp_identity_t i = identity(c, index);
    if ((c->lease.active || c->lease.pending) && c->lease.session == index &&
        c->lease.session_generation == s->generation) {
        revoke_lease(c, reason);
    }
    revoke_records(c, index, false, CP_UNAUTHENTICATED);
    /* Session loss separately stops default media. Physical takeover never
     * enters this path and never invalidates independent management/media. */
    if (c->callbacks.safety) {
        c->callbacks.safety(c->callbacks.user, reason, &i);
    }
    wipe(s, sizeof(*s));
}
size_t cp_sessions_snapshot(const cp_context_t *c, cp_session_view_t out[CP_MAX_SESSIONS])
{
    if (!c || !out) {
        return 0;
    }
    size_t count = 0;
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        if (!c->sessions[i].used) {
            continue;
        }
        out[count++] = (cp_session_view_t){.identity = identity((cp_context_t *)c, i),
                                           .last_authenticated_ms = c->sessions[i].last_seen,
                                           .authenticated = true};
    }
    return count;
}
size_t cp_context_size(void) { return sizeof(cp_context_t); }
cp_error_t cp_initialize(void *memory, size_t size, const cp_callbacks_t *callbacks,
                         cp_context_t **out)
{
    if (!memory || size < sizeof(cp_context_t) || !callbacks || !out || !callbacks->random) {
        return CP_INVALID;
    }
    cp_context_t *c = memory;
    memset(c, 0, sizeof(*c));
    c->callbacks = *callbacks;
    c->blocked_allowance = UINT64_MAX;
    if (!random_bytes(c, c->boot, 16)) {
        return CP_UNAVAILABLE;
    }
    /* Command keys remain distinct across listener/context replacement, so a
     * delayed coordinator completion cannot target a new boot's first job. */
    for (unsigned i = 0; i < 8; ++i) {
        c->next_key = (c->next_key << 8) | c->boot[i];
    }
    c->next_key &= UINT64_C(0x7fffffffffffffff);
    *out = c;
    return CP_OK;
}
cp_error_t cp_load_principal(cp_context_t *c, const cp_principal_t *p)
{
    if (!c || !p || !nonzero(p->id, 16) || !nonzero(p->credential_sha256, 32) || !p->scopes ||
        (p->scopes & ~CP_SCOPE_ALL)) {
        return CP_INVALID;
    }
    size_t slot = CP_MAX_PRINCIPALS;
    for (size_t i = 0; i < CP_MAX_PRINCIPALS; ++i) {
        if (c->principal_used[i] && equal(c->principals[i].id, p->id, 16)) {
            slot = i;
            break;
        }
        if (!c->principal_used[i] && slot == CP_MAX_PRINCIPALS) {
            slot = i;
        }
    }
    if (slot == CP_MAX_PRINCIPALS) {
        return CP_BUSY;
    }
    for (size_t i = 0; i < CP_MAX_SESSIONS; ++i) {
        if (c->sessions[i].used && c->sessions[i].principal == slot) {
            return CP_BUSY;
        }
    }
    c->principals[slot] = *p;
    c->principal_used[slot] = true;
    return CP_OK;
}
cp_error_t cp_revoke_principal(cp_context_t *c, const uint8_t id[16], uint64_t now)
{
    if (!c || !id) {
        return CP_INVALID;
    }
    cp_tick(c, now);
    for (unsigned i = 0; i < CP_MAX_PRINCIPALS; ++i) {
        if (c->principal_used[i] && equal(c->principals[i].id, id, 16)) {
            c->principals[i].enabled = false;
            for (unsigned j = 0; j < CP_MAX_SESSIONS; ++j) {
                if (c->sessions[j].used && c->sessions[j].principal == i) {
                    close_session(c, j, CP_SAFETY_REVOKED);
                }
            }
            return CP_OK;
        }
    }
    return CP_UNAVAILABLE;
}
void cp_tick(cp_context_t *c, uint64_t now)
{
    if (!c) {
        return;
    }
    if (now < c->now) {
        c->clock_failed = true;
        for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
            close_session(c, i, CP_SAFETY_SESSION_LOST);
        }
        return;
    }
    c->now = now;
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        session_t *s = &c->sessions[i];
        if (s->used &&
            (now - s->opened_at >= SESSION_ABSOLUTE_MS || now - s->last_seen >= SESSION_IDLE_MS ||
             !c->principals[s->principal].enabled)) {
            close_session(c, i, CP_SAFETY_SESSION_LOST);
        }
    }
    if ((c->lease.active || c->lease.pending) && now >= c->lease.expires) {
        revoke_lease(c, CP_SAFETY_LEASE_EXPIRED);
    }
}
void cp_physical_override(cp_context_t *c, bool locked, uint64_t now)
{
    if (!c) {
        return;
    }
    cp_tick(c, now);
    c->blocked_allowance = c->state.allowance_generation;
    revoke_lease(c, locked ? CP_SAFETY_LOCKED : CP_SAFETY_LOCAL_OVERRIDE);
    if (locked) {
        c->state.locked = true;
        for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
            if (c->sessions[i].used) {
                revoke_records(c, i, false, CP_LOCKED);
            }
        }
        if (c->callbacks.safety) {
            c->callbacks.safety(c->callbacks.user, CP_SAFETY_LOCKED, NULL);
        }
    }
}
void cp_shutdown(cp_context_t *c, uint64_t now)
{
    if (!c) {
        return;
    }
    cp_tick(c, now);
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        close_session(c, i, CP_SAFETY_SESSION_LOST);
    }
}
void cp_set_device_state(cp_context_t *c, const cp_device_state_t *state, uint64_t now)
{
    if (!c || !state ||
        strnlen(state->target_id, sizeof(state->target_id)) == sizeof(state->target_id) ||
        strnlen(state->mode, sizeof(state->mode)) == sizeof(state->mode)) {
        return;
    }
    cp_tick(c, now);
    bool changed =
        c->state_known && (state->transport_generation != c->state.transport_generation ||
                           strcmp(state->target_id, c->state.target_id) != 0);
    bool lock = state->locked && !c->state.locked;
    c->state = *state;
    c->state_known = true;
    if (lock) {
        cp_physical_override(c, true, now);
    }
    if (c->lease.active) {
        session_t *s = &c->sessions[c->lease.session];
        if (changed || state->locked || !state->remote_owner ||
            !equal(state->owner_session, s->id, 16)) {
            revoke_lease(c, CP_SAFETY_STATE_CHANGED);
        } else if (state->input_epoch != c->lease.epoch) {
            for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
                record_t *r = &c->records[i];
                const cp_operation_info_t *o = operation(r->operation);
                if (r->used && r->active && o && o->input && r->input_epoch != state->input_epoch) {
                    bool intentional = transition(r->operation) && !r->transition_seen &&
                                       r->session == c->lease.session &&
                                       r->session_generation == c->lease.session_generation &&
                                       r->input_epoch == c->lease.epoch && now < c->lease.expires &&
                                       !strcmp(state->mode, r->transition_mode);
                    if (intentional) {
                        r->transition_seen = true;
                        r->input_epoch = state->input_epoch;
                    } else {
                        r->revoked = true;
                        r->result.state = CP_RESULT_CLEANUP_PENDING;
                        r->result.error = CP_STALE;
                    }
                }
            }
            c->lease.epoch = state->input_epoch;
        }
    }
}
static record_t *find_key(cp_context_t *c, uint64_t key)
{
    for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
        if (c->records[i].used && c->records[i].key == key) {
            return &c->records[i];
        }
    }
    return NULL;
}
static bool valid_result(const cp_result_t *result)
{
    return result && (unsigned)result->state <= CP_RESULT_CLEANUP_PENDING &&
           (unsigned)result->error <= CP_TOO_LATE &&
           memchr(result->reason, 0, sizeof(result->reason)) &&
           memchr(result->object_id, 0, sizeof(result->object_id)) &&
           result->uncertain_index >= -1 &&
           (!result->progress_known || result->completed_pairs <= result->started_prefix);
}
cp_error_t cp_complete(cp_context_t *c, uint64_t key, const cp_result_t *result, uint64_t now)
{
    if (!c || !valid_result(result)) {
        return CP_INVALID;
    }
    cp_tick(c, now);
    record_t *r = find_key(c, key);
    if (!r || !r->active) {
        return CP_RESULT_UNKNOWN;
    }
    if (r->revoked && result->state != CP_RESULT_CANCELLED && result->state != CP_RESULT_FAILED &&
        result->state != CP_RESULT_CLEANUP_PENDING) {
        if (active_result(result->state)) {
            return CP_STALE;
        }
        r->result = *result;
        r->result.command_key = key;
        r->result.state = CP_RESULT_CANCELLED;
        r->result.error = CP_STALE;
        snprintf(r->result.reason, sizeof(r->result.reason), "stale_completion");
        r->active = false;
        return CP_STALE;
    }
    session_t *s = &c->sessions[r->session];
    if ((r->operation == CP_OP_CONTROL_ACQUIRE || r->operation == CP_OP_CONTROL_RENEW) &&
        result->state == CP_RESULT_APPLIED) {
        if (!s->used || s->generation != r->session_generation ||
            (!c->lease.pending && !c->lease.active) || c->lease.session != r->session ||
            now >= r->lease_expiry || c->state.locked || c->state.release_pending ||
            !c->state.input_ready || !c->state.remote_owner ||
            !equal(c->state.owner_session, s->id, 16)) {
            return CP_STALE;
        }
        c->lease.pending = false;
        c->lease.active = true;
        c->lease.expires = r->lease_expiry;
        c->lease.epoch = c->state.input_epoch;
    }
    if (r->operation == CP_OP_CONTROL_ACQUIRE && !active_result(result->state) &&
        result->state != CP_RESULT_APPLIED && c->lease.pending && c->lease.acquire_key == r->key &&
        c->lease.session == r->session && c->lease.session_generation == r->session_generation) {
        revoke_lease(c, CP_SAFETY_STATE_CHANGED);
    }
    r->result = *result;
    r->result.command_key = key;
    r->result.reason[CP_REASON_CAPACITY - 1] = 0;
    r->result.object_id[CP_ID_HEX_CAPACITY - 1] = 0;
    r->active = active_result(result->state);
    return CP_OK;
}
static bool append(char *out, size_t capacity, size_t *length, const char *fmt, ...)
{
    if (*length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *length, capacity - *length, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *length) {
        return false;
    }
    *length += (size_t)n;
    return true;
}
static cp_error_t failure(cp_http_response_t *out, cp_error_t error)
{
    out->status = error == CP_UNAUTHENTICATED                              ? 401
                  : error == CP_FORBIDDEN                                  ? 403
                  : error == CP_TOO_LARGE                                  ? 413
                  : error == CP_BUSY                                       ? 429
                  : error == CP_UNAVAILABLE || error == CP_NOT_IMPLEMENTED ? 503
                  : error == CP_LOCKED                                     ? 423
                  : error == CP_EXPIRED                                    ? 408
                  : error == CP_STALE || error == CP_SEQUENCE_GAP || error == CP_REQUEST_CONFLICT ||
                          error == CP_RESULT_UNKNOWN || error == CP_OWNER_BUSY ||
                          error == CP_LOCAL_BUSY || error == CP_RELEASE_PENDING
                      ? 409
                      : 400;
    int n = snprintf(out->body, sizeof(out->body), "{\"state\":\"rejected\",\"error\":\"%s\"}",
                     cp_error_name(error));
    out->length = n > 0 ? (size_t)n : 0;
    return error;
}
cp_error_t cp_http_configure(cp_context_t *c, const char *host, const char *origin)
{
    if (!c || !host || !origin || !strlen(host) || strlen(host) >= sizeof(c->host) ||
        strlen(origin) >= sizeof(c->origin) || strncmp(origin, "https://", 8) != 0 ||
        strcmp(origin + 8, host) != 0) {
        return CP_INVALID;
    }
    for (const char *s = host; *s; ++s) {
        if ((unsigned char)*s <= 32 || *s == '/' || *s == '\\' || *s == '#' || *s == '?' ||
            *s == '@') {
            return CP_INVALID;
        }
    }
    strcpy(c->host, host);
    strcpy(c->origin, origin);
    return CP_OK;
}
static bool cookie_value(const char *cookies, const char *name, char *out, size_t capacity)
{
    if (!cookies) {
        return false;
    }
    size_t name_len = strlen(name);
    bool found = false;
    for (const char *p = cookies; *p;) {
        while (*p == ' ' || *p == ';') {
            ++p;
        }
        const char *end = strchr(p, ';');
        if (!end) {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) > name_len && memcmp(p, name, name_len) == 0 && p[name_len] == '=') {
            size_t length = (size_t)(end - p) - name_len - 1;
            if (found || length >= capacity) {
                return false;
            }
            memcpy(out, p + name_len + 1, length);
            out[length] = 0;
            found = true;
        }
        p = end;
    }
    return found;
}
static cp_error_t login(cp_context_t *c, const cp_http_security_t *security, const char *body,
                        size_t length, cp_http_response_t *out)
{
    if (c->now - c->login_window >= 60000U) {
        c->login_window = c->now;
        c->failed_logins = 0;
    }
    if (c->failed_logins >= 5) {
        return failure(out, CP_BUSY);
    }
    cj_document_t *d = &c->document;
    if (!cj_parse(d, body, length) || d->nodes[0].type != CJ_OBJECT) {
        return failure(out, CP_INVALID);
    }
    unsigned members = 0;
    for (uint16_t i = d->nodes[0].child; i != CJ_NONE; i = d->nodes[i].next) {
        ++members;
    }
    uint8_t client[16] = {0}, secret[32] = {0}, digest[32] = {0};
    if (members != 2 || !decode(cj_string(d, cj_get(d, 0, "client_id")), client, 16) ||
        !decode(cj_string(d, cj_get(d, 0, "credential")), secret, 32)) {
        wipe(secret, sizeof(secret));
        return failure(out, CP_INVALID);
    }
    cp_sha256(secret, 32, digest);
    wipe(secret, sizeof(secret));
    unsigned principal = CP_MAX_PRINCIPALS;
    for (unsigned i = 0; i < CP_MAX_PRINCIPALS; ++i) {
        if (c->principal_used[i] && c->principals[i].enabled &&
            equal(c->principals[i].id, client, 16) &&
            equal(c->principals[i].credential_sha256, digest, 32)) {
            principal = i;
        }
    }
    wipe(digest, sizeof(digest));
    bool browser = security->origin != NULL;
    if (principal == CP_MAX_PRINCIPALS || c->principals[principal].native_only == browser) {
        ++c->failed_logins;
        return failure(out, CP_UNAUTHENTICATED);
    }
    if (browser) {
        char nonce[33];
        uint8_t value[16];
        if (c->now >= c->challenge_until ||
            !cookie_value(security->cookie, "__Host-control-preauth", nonce, sizeof(nonce)) ||
            !decode(nonce, value, 16) || !equal(value, c->challenge, 16) || !security->csrf ||
            strcmp(nonce, security->csrf) != 0) {
            return failure(out, CP_FORBIDDEN);
        }
        wipe(c->challenge, 16);
        c->challenge_until = 0;
    }
    unsigned slot = CP_MAX_SESSIONS;
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        if (!c->sessions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == CP_MAX_SESSIONS || c->session_generation == UINT64_MAX) {
        return failure(out, CP_BUSY);
    }
    session_t s = {.used = true,
                   .browser = browser,
                   .principal = (uint8_t)principal,
                   .generation = ++c->session_generation,
                   .opened_at = c->now,
                   .last_seen = c->now,
                   .rate_at = c->now,
                   .rate_tokens = 8000};
    uint8_t token[32];
    if (!random_bytes(c, s.id, 16) || !random_bytes(c, s.csrf, 16) ||
        !random_bytes(c, s.prefix, 8) || !random_bytes(c, token, 32)) {
        wipe(token, sizeof(token));
        return failure(out, CP_UNAVAILABLE);
    }
    cp_sha256(token, 32, s.token_hash);
    char token_hex[65], session_hex[33], prefix[17], csrf[33], boot[33];
    encode(token, 32, token_hex);
    wipe(token, 32);
    encode(s.id, 16, session_hex);
    encode(s.csrf, 16, csrf);
    encode(s.prefix, 8, prefix);
    encode(c->boot, 16, boot);
    c->sessions[slot] = s;
    out->status = 200;
    size_t used = 0;
    append(out->body, sizeof(out->body), &used,
           "{\"session_id\":\"%s\",\"request_prefix\":\"%s\",\"boot_id\":\"%s\",\"csrf\":\"%s\","
           "\"next_seq\":1",
           session_hex, prefix, boot, csrf);
    if (browser) {
        snprintf(out->set_cookie, sizeof(out->set_cookie),
                 "__Host-control=%s; Secure; HttpOnly; SameSite=Strict; Path=/", token_hex);
    } else {
        append(out->body, sizeof(out->body), &used, ",\"session_token\":\"%s\"", token_hex);
    }
    append(out->body, sizeof(out->body), &used, "}");
    out->length = used;
    wipe(token_hex, sizeof(token_hex));
    return CP_OK;
}
static int authenticate(cp_context_t *c, const cp_http_security_t *security, bool mutation)
{
    char token[65];
    bool bearer = security->authorization != NULL;
    if (bearer) {
        if (strncmp(security->authorization, "Bearer ", 7) != 0 ||
            strlen(security->authorization + 7) != 64) {
            return -1;
        }
        memcpy(token, security->authorization + 7, 65);
    } else if (!cookie_value(security->cookie, "__Host-control", token, sizeof(token))) {
        return -1;
    }
    uint8_t secret[32], hash[32];
    if (!decode(token, secret, 32)) {
        wipe(token, sizeof(token));
        return -1;
    }
    cp_sha256(secret, 32, hash);
    wipe(secret, 32);
    wipe(token, sizeof(token));
    int result = -1;
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        session_t *s = &c->sessions[i];
        if (!s->used || !equal(hash, s->token_hash, 32)) {
            continue;
        }
        if (s->browser == bearer) {
            continue;
        }
        if (s->browser && mutation) {
            uint8_t csrf[16];
            if (!security->origin || !decode(security->csrf, csrf, 16) ||
                !equal(csrf, s->csrf, 16)) {
                continue;
            }
        }
        if (!s->browser && security->origin) {
            continue;
        }
        result = (int)i;
    }
    wipe(hash, 32);
    return result;
}
static bool allowed_envelope(const cj_document_t *d)
{
    static const char *const keys[] = {"v",
                                       "boot_id",
                                       "session_id",
                                       "request_id",
                                       "seq",
                                       "deadline_ms",
                                       "management_generation",
                                       "resource_revision",
                                       "epoch",
                                       "transport_generation",
                                       "target_id",
                                       "lease_id",
                                       "ui_revision",
                                       "op",
                                       "args"};
    if (d->nodes[0].type != CJ_OBJECT) {
        return false;
    }
    for (uint16_t n = d->nodes[0].child; n != CJ_NONE; n = d->nodes[n].next) {
        bool found = false;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (strcmp(d->text + d->nodes[n].key, keys[i]) == 0) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
static bool read_u64(const cj_document_t *d, const char *key, uint64_t *out)
{
    return cj_u64(d, cj_get(d, 0, key), out);
}
static bool copy_string(const cj_document_t *d, uint16_t node, char *out, size_t cap)
{
    const char *s = cj_string(d, node);
    if (!s || strlen(s) >= cap) {
        return false;
    }
    memcpy(out, s, strlen(s) + 1);
    return true;
}
static cp_error_t config_scopes(cp_context_t *c, unsigned session, uint16_t args,
                                cp_operation_id_t op)
{
    cj_document_t *d = &c->document;
    if (op == CP_OP_CONFIG_STAGE) {
        uint16_t patch = cj_get(d, args, "patch");
        if (patch == CJ_NONE || d->nodes[patch].child == CJ_NONE) {
            return CP_INVALID;
        }
        bool has_cell = false, has_hysteresis = false;
        double cell = 0, hysteresis = 0;
        for (uint16_t n = d->nodes[patch].child; n != CJ_NONE; n = d->nodes[n].next) {
            const char *key = d->text + d->nodes[n].key;
            const cp_config_info_t *f = cp_config_find(key);
            if (!f || !cs_validate(d, n, f->schema)) {
                return CP_INVALID;
            }
            if ((c->principals[c->sessions[session].principal].scopes & f->scope) != f->scope) {
                return CP_FORBIDDEN;
            }
            if (strcmp(key, "selection.cell_degrees") == 0) {
                has_cell = true;
                cell = d->nodes[n].type == CJ_INTEGER ? (double)d->nodes[n].value.integer
                                                      : d->nodes[n].value.number;
            }
            if (strcmp(key, "selection.hysteresis_degrees") == 0) {
                has_hysteresis = true;
                hysteresis = d->nodes[n].type == CJ_INTEGER ? (double)d->nodes[n].value.integer
                                                            : d->nodes[n].value.number;
            }
        }
        if (has_cell && has_hysteresis && hysteresis >= cell / 2) {
            return CP_INVALID;
        }
        /* Candidate-versus-current dependencies and provider qualification are
         * rechecked by the owning config manager at inert stage and apply. */
    } else if (op == CP_OP_CONFIG_RESET) {
        uint16_t keys = cj_get(d, args, "keys");
        if (keys == CJ_NONE || d->nodes[keys].child == CJ_NONE) {
            return CP_INVALID;
        }
        for (uint16_t n = d->nodes[keys].child; n != CJ_NONE; n = d->nodes[n].next) {
            const cp_config_info_t *f = cp_config_find(cj_string(d, n));
            if (!f) {
                return CP_INVALID;
            }
            if ((c->principals[c->sessions[session].principal].scopes & f->scope) != f->scope) {
                return CP_FORBIDDEN;
            }
        }
    }
    return CP_OK;
}
static cp_error_t gates(cp_context_t *c, unsigned session, const cp_operation_info_t *o,
                        uint16_t args)
{
    session_t *s = &c->sessions[session];
    cp_command_t *cmd = &c->command;
    cj_document_t *d = &c->document;
    cmd->identity = identity(c, session);
    if ((cmd->identity.scopes & o->scopes) != o->scopes) {
        return CP_FORBIDDEN;
    }
    if (!o->initial_v1_supported) {
        return CP_NOT_IMPLEMENTED;
    }
    if (!cs_validate(d, args, cp_argument_schema(o->id))) {
        return CP_INVALID;
    }
    uint16_t muted = cj_get(d, args, "muted"), enabled = cj_get(d, args, "enabled");
    bool safety_action =
        o->safety ||
        (cp_operation_flag(o->id, "conditional_safety") &&
         ((muted != CJ_NONE && d->nodes[muted].type == CJ_BOOL && d->nodes[muted].value.boolean) ||
          (enabled != CJ_NONE && d->nodes[enabled].type == CJ_BOOL &&
           !d->nodes[enabled].value.boolean)));
    if (c->state.locked && !safety_action && !o->read_only) {
        return CP_LOCKED;
    }
    if (!c->state_known) {
        return CP_UNAVAILABLE;
    }
    if (o->input) {
        if (o->safety) {
            cmd->input_epoch = c->state.input_epoch;
            cmd->transport_generation = c->state.transport_generation;
            memcpy(cmd->target_id, c->state.target_id, sizeof(cmd->target_id));
        } else {
            if (!read_u64(d, "epoch", &cmd->input_epoch) ||
                !read_u64(d, "transport_generation", &cmd->transport_generation) ||
                !copy_string(d, cj_get(d, 0, "target_id"), cmd->target_id,
                             sizeof(cmd->target_id))) {
                return CP_INVALID;
            }
            if (cmd->input_epoch != c->state.input_epoch ||
                cmd->transport_generation != c->state.transport_generation ||
                strcmp(cmd->target_id, c->state.target_id) != 0) {
                return CP_STALE;
            }
        }
        if (o->ui_context && (!read_u64(d, "ui_revision", &cmd->ui_revision) ||
                              cmd->ui_revision != c->state.ui_revision)) {
            return CP_STALE;
        }
        if (o->id == CP_OP_CONTROL_ACQUIRE) {
            if (c->lease.pending || c->lease.active) {
                return CP_OWNER_BUSY;
            }
            if (!c->state.remote_allowed || c->blocked_allowance == c->state.allowance_generation) {
                return CP_FORBIDDEN;
            }
            if (c->state.local_busy) {
                return CP_LOCAL_BUSY;
            }
            if (c->state.release_pending) {
                return CP_RELEASE_PENDING;
            }
            if (!c->state.input_ready) {
                return CP_UNAVAILABLE;
            }
        } else if (!o->safety) {
            if (!c->lease.active || c->lease.session != session ||
                c->lease.session_generation != s->generation) {
                return CP_FORBIDDEN;
            }
            if (!decode(cj_string(d, cj_get(d, 0, "lease_id")), cmd->lease_id, 16) ||
                !equal(cmd->lease_id, c->lease.id, 16)) {
                return CP_FORBIDDEN;
            }
            if (c->state.release_pending && !cp_operation_flag(o->id, "renew")) {
                return CP_RELEASE_PENDING;
            }
            if (!c->state.input_ready && !cp_operation_flag(o->id, "renew")) {
                return CP_UNAVAILABLE;
            }
        }
        if ((o->id == CP_OP_CONTROL_ACQUIRE || o->id == CP_OP_CONTROL_RENEW)) {
            uint64_t ack;
            if (!cj_u64(d, cj_get(d, args, "state_sequence"), &ack) ||
                ack != s->delivered_sequence || c->now - s->state_at > STATE_FRESH_MS ||
                !s->state_at) {
                return CP_STALE;
            }
        }
        if (cp_operation_flag(o->id, "script_movement") &&
            strcmp(c->lease.producer, "device_imu") == 0) {
            return CP_BUSY;
        }
        if (!o->safety && o->id != CP_OP_CONTROL_ACQUIRE && o->id != CP_OP_CONTROL_RENEW &&
            o->id != CP_OP_MODE_SELECT) {
            const char *mode = NULL;
            if (strncmp(o->name, "mouse.", 6) == 0 || o->id == CP_OP_TOUCH_MOVE) {
                mode = "mouse";
            }
            if (strncmp(o->name, "keyboard.", 9) == 0 || strncmp(o->name, "preview.", 8) == 0 ||
                strncmp(o->name, "selection.", 10) == 0) {
                mode = "keyboard";
            }
            if (mode && strcmp(c->state.mode, mode) != 0) {
                return CP_STALE;
            }
        }
    } else if (!o->read_only) {
        if (!read_u64(d, "management_generation", &cmd->management_generation) ||
            !read_u64(d, "resource_revision", &cmd->resource_revision)) {
            return CP_INVALID;
        }
        if (cmd->management_generation != c->state.management_generation) {
            return CP_STALE;
        }
        /* Management cannot smuggle input ownership fields across domains. */
        const char *const names[] = {"epoch", "transport_generation", "target_id", "lease_id",
                                     "ui_revision"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            uint16_t n = cj_get(d, 0, names[i]);
            if (n != CJ_NONE && d->nodes[n].type != CJ_NULL) {
                return CP_INVALID;
            }
        }
    }
    cp_error_t error = config_scopes(c, session, args, o->id);
    if (error != CP_OK) {
        return error;
    }
    if (o->id == CP_OP_RECORDING_CANCEL) {
        const char *disposition = cj_string(d, cj_get(d, args, "disposition"));
        if (disposition && strcmp(disposition, "discard") == 0 &&
            !(cmd->identity.scopes & CP_SCOPE_RECORDING_DELETE)) {
            return CP_FORBIDDEN;
        }
    }
    if (o->id == CP_OP_RECORDING_START) {
        const char *policy = cj_string(d, cj_get(d, args, "disconnect_policy"));
        if (policy && strcmp(policy, "continue_local") == 0 &&
            cj_get(d, args, "background_grant_id") == CJ_NONE) {
            return CP_FORBIDDEN;
        }
    }
    if (cp_operation_flag(o->id, "privacy")) {
        uint8_t consent[16];
        if (!decode(cj_string(d, cj_get(d, args, "consent_id")), consent, 16)) {
            return CP_FORBIDDEN;
        }
        /* Scope alone never establishes a source/destination grant. The owning
         * capture manager must validate consent/indicator before any samples. */
    }
    if (cp_operation_flag(o->id, "update_authority") && cj_get(d, args, "policy_id") == CJ_NONE &&
        cj_get(d, args, "approval_id") == CJ_NONE) {
        return CP_FORBIDDEN;
    }
    if (o->input && o->safety) {
        return CP_OK;
    }
    if (!c->callbacks.available) {
        return CP_UNAVAILABLE;
    }
    char reason[CP_REASON_CAPACITY] = {0};
    if (!c->callbacks.available(c->callbacks.user, o->id, reason)) {
        return CP_UNAVAILABLE;
    }
    return CP_OK;
}
static record_t *allocate_record(cp_context_t *c)
{
    record_t *oldest = NULL;
    for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
        record_t *r = &c->records[i];
        if (!r->used) {
            return r;
        }
        if (!r->active && (!oldest || r->key < oldest->key)) {
            oldest = r;
        }
    }
    return oldest;
}
static cp_error_t result_response(cp_context_t *c, unsigned session, const record_t *r,
                                  cp_http_response_t *out)
{
    size_t used = 0;
    char reason[CP_REASON_CAPACITY * 6 + 3], object[CP_ID_HEX_CAPACITY * 6 + 3];
    if (!cj_escape(r->result.reason, reason, sizeof(reason)) ||
        !cj_escape(r->result.object_id, object, sizeof(object))) {
        return failure(out, CP_INTERNAL);
    }
    out->status = r->result.error == CP_OK ? (active_result(r->result.state) ? 202 : 200) : 400;
    char job[32] = "null", revision[32] = "null", next[32] = "null";
    if (r->result.job_id) {
        snprintf(job, sizeof(job), "\"%" PRIu64 "\"", r->result.job_id);
    }
    if (r->result.resource_revision) {
        snprintf(revision, sizeof(revision), "\"%" PRIu64 "\"", r->result.resource_revision);
    }
    if (c->sessions[session].used && c->sessions[session].generation == r->session_generation) {
        snprintf(next, sizeof(next), "%" PRIu64, (uint64_t)c->sessions[session].sequence + 1);
    }
    append(
        out->body, sizeof(out->body), &used,
        "{\"state\":\"%s\",\"error\":\"%s\",\"command_key\":\"%" PRIu64
        "\",\"job_id\":%s,\"resource_revision\":%s,\"object_id\":%s,\"reason\":%s,\"next_seq\":%s",
        cp_result_state_name(r->result.state), cp_error_name(r->result.error), r->key, job,
        revision, r->result.object_id[0] ? object : "null", reason, next);
    if (r->result.progress_known) {
        append(out->body, sizeof(out->body), &used,
               ",\"started_prefix\":%u,\"completed_pairs\":%u,\"uncertain_index\":%d",
               r->result.started_prefix, r->result.completed_pairs, r->result.uncertain_index);
    } else {
        append(out->body, sizeof(out->body), &used,
               ",\"started_prefix\":null,\"completed_pairs\":null,\"uncertain_index\":null");
    }
    if ((c->lease.pending || c->lease.active) && c->lease.session == session &&
        c->lease.session_generation == c->sessions[session].generation &&
        (r->operation == CP_OP_CONTROL_ACQUIRE || r->operation == CP_OP_CONTROL_RENEW)) {
        char lease[33];
        encode(c->lease.id, 16, lease);
        append(out->body, sizeof(out->body), &used,
               ",\"lease_id\":\"%s\",\"lease_active\":%s,\"lease_expires_ms\":\"%" PRIu64 "\"",
               lease, c->lease.active ? "true" : "false", c->lease.expires);
    }
    append(out->body, sizeof(out->body), &used, "}");
    out->length = used;
    return r->result.error;
}
static cp_error_t commands(cp_context_t *c, unsigned session, const char *body, size_t length,
                           cp_http_response_t *out)
{
    session_t *s = &c->sessions[session];
    cj_document_t *d = &c->document;
    if (s->commands_blocked) {
        return failure(out, CP_REQUEST_CONFLICT);
    }
    if (length > CP_MAX_FRAME_BYTES) {
        return failure(out, CP_TOO_LARGE);
    }
    if (!cj_parse(d, body, length) || !allowed_envelope(d)) {
        return failure(out, CP_INVALID);
    }
    int64_t version, seq;
    uint8_t boot[16], sid[16], request[16];
    if (!cj_integer(d, cj_get(d, 0, "v"), &version) || version != 1 ||
        !cj_integer(d, cj_get(d, 0, "seq"), &seq) || seq < 1 || seq > UINT32_MAX ||
        !decode(cj_string(d, cj_get(d, 0, "boot_id")), boot, 16) ||
        !decode(cj_string(d, cj_get(d, 0, "session_id")), sid, 16) ||
        !decode(cj_string(d, cj_get(d, 0, "request_id")), request, 16)) {
        return failure(out, CP_INVALID);
    }
    if (!equal(boot, c->boot, 16) || !equal(sid, s->id, 16)) {
        return failure(out, CP_STALE);
    }
    uint64_t suffix = 0;
    for (unsigned i = 8; i < 16; ++i) {
        suffix = suffix * 256U + request[i];
    }
    if (!equal(request, s->prefix, 8) || suffix != (uint64_t)seq) {
        return failure(out, CP_REQUEST_CONFLICT);
    }
    size_t canonical_length;
    if (!cj_canonical(d, 0, c->canonical, sizeof(c->canonical), &canonical_length)) {
        return failure(out, CP_TOO_LARGE);
    }
    uint8_t digest[32];
    cp_sha256(c->canonical, canonical_length, digest);
    if ((uint32_t)seq <= s->sequence) {
        for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
            record_t *r = &c->records[i];
            if (r->used && r->session == session && r->session_generation == s->generation &&
                r->sequence == (uint32_t)seq) {
                if (equal(r->digest, digest, 32)) {
                    return result_response(c, session, r, out);
                }
                s->commands_blocked = true;
                revoke_lease(c, CP_SAFETY_REQUEST_CONFLICT);
                return failure(out, CP_REQUEST_CONFLICT);
            }
        }
        return failure(out, CP_RESULT_UNKNOWN);
    }
    if ((uint64_t)seq != (uint64_t)s->sequence + 1) {
        return failure(out, CP_SEQUENCE_GAP);
    }
    if (c->next_key == UINT64_MAX) {
        return failure(out, CP_BUSY);
    }
    record_t *r = allocate_record(c);
    if (!r) {
        return failure(out, CP_BUSY);
    }
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->session = (uint8_t)session;
    r->session_generation = s->generation;
    r->sequence = (uint32_t)seq;
    r->key = ++c->next_key;
    memcpy(r->digest, digest, 32);
    r->result = (cp_result_t){.state = CP_RESULT_REJECTED,
                              .error = CP_INVALID,
                              .command_key = r->key,
                              .uncertain_index = -1};
    s->sequence = (uint32_t)seq;
    cp_command_t *cmd = &c->command;
    memset(cmd, 0, sizeof(*cmd));
    cmd->version = 1;
    cmd->command_key = r->key;
    const char *name = cj_string(d, cj_get(d, 0, "op"));
    const cp_operation_info_t *o = name ? cp_operation_find(name) : NULL;
    uint16_t args = cj_get(d, 0, "args");
    cp_error_t error = CP_INVALID;
    if (o && args != CJ_NONE && d->nodes[args].type == CJ_OBJECT) {
        r->operation = o->id;
        cmd->operation = o->id;
        if (!read_u64(d, "deadline_ms", &cmd->deadline_ms)) {
            error = CP_INVALID;
        } else if (!(o->input && o->safety) &&
                   (cmd->deadline_ms <= c->now ||
                    cmd->deadline_ms - c->now >
                        (cp_operation_flag(o->id, "script_movement") ? 250U : 2000U))) {
            error = CP_EXPIRED;
        } else {
            error = gates(c, session, o, args);
        }
        if (error == CP_OK &&
            !cj_canonical(d, args, cmd->args_json, sizeof(cmd->args_json), NULL)) {
            error = CP_TOO_LARGE;
        }
        if (error == CP_OK && o->input && !o->safety && o->id != CP_OP_CONTROL_RENEW) {
            uint64_t delta = c->now - s->rate_at;
            s->rate_at = c->now;
            uint64_t refill = delta > 134 ? 8000U : delta * 60U;
            s->rate_tokens =
                (uint32_t)(s->rate_tokens + refill > 8000U ? 8000U : s->rate_tokens + refill);
            if (s->rate_tokens < 1000U) {
                error = CP_BUSY;
            } else {
                s->rate_tokens -= 1000U;
            }
        }
        if (error == CP_OK && (o->safety || o->id == CP_OP_CONTROL_RENEW)) {
            if (c->now - s->safety_at >= 1000U) {
                s->safety_at = c->now;
                s->safety_count = 0;
            }
            if (s->safety_count >= 8) {
                error = CP_BUSY;
            } else {
                ++s->safety_count;
            }
        }
        if (error == CP_OK) {
            size_t active = 0;
            for (size_t i = 0; i < CP_MAX_RESULTS; ++i) {
                if (c->records[i].active) {
                    ++active;
                }
            }
            if (active >= CP_MAX_ACTIVE_REQUESTS && !o->safety) {
                error = CP_BUSY;
            }
        }
        if (error == CP_OK && (o->id == CP_OP_CONTROL_ACQUIRE || o->id == CP_OP_CONTROL_RENEW)) {
            uint64_t lifetime = LEASE_MAX_MS;
            int64_t requested;
            if (o->id == CP_OP_CONTROL_ACQUIRE &&
                cj_integer(d, cj_get(d, args, "lifetime_ms"), &requested)) {
                lifetime = (uint64_t)requested;
            }
            cmd->lease_expires_ms = c->now + lifetime;
            r->lease_expiry = cmd->lease_expires_ms;
            if (o->id == CP_OP_CONTROL_ACQUIRE) {
                c->lease = (lease_t){.pending = true,
                                     .session = (uint8_t)session,
                                     .session_generation = s->generation,
                                     .expires = cmd->lease_expires_ms,
                                     .epoch = cmd->input_epoch,
                                     .acquire_key = r->key};
                const char *producer = cj_string(d, cj_get(d, args, "producer"));
                snprintf(c->lease.producer, sizeof(c->lease.producer), "%s",
                         producer ? producer : "script");
                if (!random_bytes(c, c->lease.id, 16)) {
                    error = CP_UNAVAILABLE;
                }
                memcpy(cmd->lease_id, c->lease.id, 16);
            }
        }
        r->input_epoch = cmd->input_epoch;
        if (o->id == CP_OP_MODE_SELECT) {
            const char *mode = cj_string(&c->document, cj_get(&c->document, args, "mode"));
            if (mode) {
                snprintf(r->transition_mode, sizeof(r->transition_mode), "%s", mode);
            }
        } else if (o->id == CP_OP_KEYBOARD_SUBMODE_SELECT) {
            strcpy(r->transition_mode, "keyboard");
        } else if (o->id == CP_OP_INPUT_MOTION_SET) {
            strcpy(r->transition_mode, "mouse");
        }
        if (error == CP_OK && o->id == CP_OP_CONTROL_RELEASE &&
            (c->lease.active || c->lease.pending) && c->lease.session != session) {
            error = CP_FORBIDDEN;
        }
        if (error == CP_OK && o->id == CP_OP_SESSION_CLOSE) {
            r->result.state = CP_RESULT_COMPLETED;
            r->result.error = CP_OK;
            close_session(c, session, CP_SAFETY_SESSION_LOST);
            return result_response(c, session, r, out);
        }
        if (error == CP_OK) {
            if (!c->callbacks.dispatch) {
                error = CP_UNAVAILABLE;
            } else {
                cp_result_t result = {.state = CP_RESULT_REJECTED,
                                      .error = CP_INTERNAL,
                                      .command_key = r->key,
                                      .uncertain_index = -1};
                error = c->callbacks.dispatch(c->callbacks.user, cmd, &result);
                if (error == CP_OK) {
                    if (!valid_result(&result) || result.state == CP_RESULT_REJECTED) {
                        /* The owner returned admission with an invalid result.
                         * Effects may already exist: preserve an unknown cleanup
                         * record, close its authority, never relabel rejection. */
                        close_session(c, session, CP_SAFETY_SESSION_LOST);
                        r->active = true;
                        r->revoked = true;
                        r->result = (cp_result_t){.state = CP_RESULT_CLEANUP_PENDING,
                                                  .error = CP_INTERNAL,
                                                  .command_key = r->key,
                                                  .uncertain_index = -1};
                        strcpy(r->result.reason, "owner_contract_invalid_outcome_unknown");
                        return result_response(c, session, r, out);
                    } else {
                        r->active = true;
                        r->result = result;
                        r->result.command_key = r->key;
                        if (o->id == CP_OP_CONTROL_ACQUIRE || o->id == CP_OP_CONTROL_RENEW) {
                            if (result.state == CP_RESULT_APPLIED) {
                                error = cp_complete(c, r->key, &result, c->now);
                            } else if (result.state != CP_RESULT_ACCEPTED &&
                                       result.state != CP_RESULT_RUNNING) {
                                error = cp_complete(c, r->key, &result, c->now);
                            }
                        } else {
                            error = cp_complete(c, r->key, &result, c->now);
                        }
                    }
                }
            }
        }
    }
    if (error == CP_OK && o &&
        (o->id == CP_OP_CONTROL_RELEASE || o->id == CP_OP_INPUT_CANCEL ||
         o->id == CP_OP_INPUT_RELEASE_ALL || o->id == CP_OP_DEVICE_LOCK)) {
        revoke_lease(c, o->id == CP_OP_DEVICE_LOCK ? CP_SAFETY_LOCKED : CP_SAFETY_CLIENT_STOP);
        if (c->callbacks.safety) {
            c->callbacks.safety(c->callbacks.user,
                                o->id == CP_OP_DEVICE_LOCK ? CP_SAFETY_LOCKED
                                                           : CP_SAFETY_CLIENT_STOP,
                                &cmd->identity);
        }
    }
    if (error != CP_OK) {
        r->active = false;
        r->result.state = CP_RESULT_REJECTED;
        r->result.error = error;
        if (o && o->id == CP_OP_CONTROL_ACQUIRE && c->lease.pending &&
            c->lease.acquire_key == r->key && c->lease.session == session &&
            c->lease.session_generation == s->generation) {
            wipe(&c->lease, sizeof(c->lease));
        }
    }
    return result_response(c, session, r, out);
}
static bool query_integer(const char *resource, const char *key, unsigned fallback,
                          unsigned maximum, unsigned *out)
{
    const char *p = strchr(resource, '?');
    *out = fallback;
    if (!p) {
        return true;
    }
    ++p;
    bool found = false;
    size_t key_length = strlen(key);
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) {
            end = p + strlen(p);
        }
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) {
            return false;
        }
        if ((size_t)(eq - p) == key_length && memcmp(p, key, key_length) == 0) {
            if (found || eq + 1 == end) {
                return false;
            }
            unsigned value = 0;
            for (const char *s = eq + 1; s < end; ++s) {
                if (*s < '0' || *s > '9' || value > (maximum - (unsigned)(*s - '0')) / 10U) {
                    return false;
                }
                value = value * 10U + (unsigned)(*s - '0');
            }
            if (value > maximum) {
                return false;
            }
            *out = value;
            found = true;
        }
        p = *end ? end + 1 : end;
    }
    return true;
}
static bool safe_resource(const char *resource)
{
    if (!resource || strlen(resource) > 256 || strstr(resource, "..") || strstr(resource, "//")) {
        return false;
    }
    for (const char *s = resource; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              strchr("/_-.?=&", *s))) {
            return false;
        }
    }
    return true;
}
static bool sensitive_keys(const cj_document_t *d)
{
    const char *const forbidden[] = {"password",    "passphrase", "credential",
                                     "private_key", "secret",     "session_token"};
    for (uint16_t i = 0; i < d->count; ++i) {
        if (d->nodes[i].key != CJ_NONE) {
            for (size_t j = 0; j < sizeof(forbidden) / sizeof(forbidden[0]); ++j) {
                if (strcmp(d->text + d->nodes[i].key, forbidden[j]) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}
static cp_error_t provider_read(cp_context_t *c, unsigned session, const char *resource,
                                char *buffer, size_t cap, size_t *length)
{
    if (!c->callbacks.read) {
        return CP_UNAVAILABLE;
    }
    cp_identity_t i = identity(c, session);
    *length = 0;
    cp_error_t error = c->callbacks.read(c->callbacks.user, &i, resource, buffer, cap, length);
    if (error != CP_OK) {
        return error;
    }
    if (!*length || *length >= cap || buffer[*length] != 0 ||
        !cj_parse(&c->document, buffer, *length) || sensitive_keys(&c->document)) {
        return CP_INTERNAL;
    }
    return CP_OK;
}
static cp_error_t state_response(cp_context_t *c, unsigned session, cp_http_response_t *out)
{
    if (!c->state_known) {
        return failure(out, CP_UNAVAILABLE);
    }
    session_t *s = &c->sessions[session];
    size_t used = 0;
    char boot[33], target[CP_TARGET_CAPACITY * 6 + 3], mode[80];
    encode(c->boot, 16, boot);
    if (!cj_escape(c->state.target_id, target, sizeof(target)) ||
        !cj_escape(c->state.mode, mode, sizeof(mode))) {
        return failure(out, CP_INTERNAL);
    }
    append(out->body, sizeof(out->body), &used,
           "{\"protocol\":{\"v\":1,\"boot_id\":\"%s\",\"now_ms\":\"%" PRIu64
           "\",\"management_generation\":\"%" PRIu64 "\",\"epoch\":\"%" PRIu64
           "\",\"transport_generation\":\"%" PRIu64 "\",\"ui_revision\":\"%" PRIu64
           "\",\"state_sequence\":\"%" PRIu64
           "\",\"target_id\":%s,\"mode\":%s,\"locked\":%s,\"release_pending\":%s,\"input_ready\":%"
           "s,\"remote_allowed\":%s",
           boot, c->now, c->state.management_generation, c->state.input_epoch,
           c->state.transport_generation, c->state.ui_revision, c->state.state_sequence, target,
           mode, c->state.locked ? "true" : "false", c->state.release_pending ? "true" : "false",
           c->state.input_ready ? "true" : "false",
           c->state.remote_allowed && c->blocked_allowance != c->state.allowance_generation
               ? "true"
               : "false");
    if ((c->lease.active || c->lease.pending) && c->lease.session == session &&
        c->lease.session_generation == s->generation) {
        char lease[33];
        encode(c->lease.id, 16, lease);
        append(out->body, sizeof(out->body), &used,
               ",\"lease_id\":\"%s\",\"lease_active\":%s,\"lease_expires_ms\":\"%" PRIu64 "\"",
               lease, c->lease.active ? "true" : "false", c->lease.expires);
    }
    append(out->body, sizeof(out->body), &used, "},\"device\":");
    size_t device_length = 0;
    cp_error_t error = provider_read(c, session, "/api/v1/state", out->body + used,
                                     sizeof(out->body) - used - 100, &device_length);
    if (error == CP_OK) {
        used += device_length;
        append(out->body, sizeof(out->body), &used, "}");
    } else if (error == CP_UNAVAILABLE || error == CP_NOT_IMPLEMENTED) {
        append(out->body, sizeof(out->body), &used, "null,\"device_reason\":\"%s\"}",
               cp_error_name(error));
    } else {
        return failure(out, error);
    }
    s->state_at = c->now;
    s->delivered_sequence = c->state.state_sequence;
    out->status = 200;
    out->length = used;
    return CP_OK;
}
static cp_error_t metadata(cp_context_t *c, unsigned session, const char *resource,
                           cp_http_response_t *out, bool config)
{
    unsigned cursor, limit;
    if (!query_integer(resource, "cursor", 0, 10000, &cursor) ||
        !query_integer(resource, "limit", 8, 8, &limit) || !limit) {
        return failure(out, CP_INVALID);
    }
    size_t used = 0, total = config ? cp_config_count() : cp_operation_count();
    if (cursor > total) {
        return failure(out, CP_INVALID);
    }
    append(out->body, sizeof(out->body), &used, "{\"v\":1,\"total\":%u,\"items\":[",
           (unsigned)total);
    unsigned emitted = 0;
    for (size_t i = cursor; i < total && emitted < limit; ++i) {
        char item[1600];
        size_t length = 0;
        if (config) {
            const cp_config_info_t *f = cp_config_at(i);
            const char *type = f->schema->type == CJ_BOOL      ? "boolean"
                               : f->schema->type == CJ_INTEGER ? "integer"
                               : f->schema->type == CJ_NUMBER  ? "number"
                               : f->schema->type == CJ_ARRAY   ? "array"
                               : f->schema->type == CJ_OBJECT  ? "object"
                                                               : "string";
            append(item, sizeof(item), &length,
                   "{\"key\":\"%s\",\"type\":\"%s\",\"default\":%s,\"scope\":\"%s\",\"apply_"
                   "policy\":\"%s\",\"persistence\":\"%s\",\"sensitive\":%s,\"runtime_"
                   "availability\":null,\"reason\":\"provider_validation_required\"}",
                   f->key, type, f->default_json, cp_scope_name((size_t)__builtin_ctzll(f->scope)),
                   f->apply_policy, f->persistence, f->sensitive ? "true" : "false");
        } else {
            const cp_operation_info_t *o = cp_operation_at(i);
            char reason[CP_REASON_CAPACITY] = {0};
            bool allowed =
                (c->principals[c->sessions[session].principal].scopes & o->scopes) == o->scopes;
            bool available = allowed && o->initial_v1_supported && c->callbacks.available &&
                             c->callbacks.available(c->callbacks.user, o->id, reason);
            append(item, sizeof(item), &length,
                   "{\"id\":%u,\"name\":\"%s\",\"available\":%s,\"reason\":\"%s\",\"input\":%s,"
                   "\"read_only\":%s}",
                   o->id, o->name, available ? "true" : "false",
                   !allowed                   ? "forbidden"
                   : !o->initial_v1_supported ? "not_implemented"
                   : available                ? "ok"
                                              : "unavailable",
                   o->input ? "true" : "false", o->read_only ? "true" : "false");
        }
        if (used + length + 100 >= sizeof(out->body)) {
            break;
        }
        if (emitted) {
            append(out->body, sizeof(out->body), &used, ",");
        }
        append(out->body, sizeof(out->body), &used, "%s", item);
        ++emitted;
    }
    append(out->body, sizeof(out->body), &used, "],\"next_cursor\":");
    if (cursor + emitted < total) {
        append(out->body, sizeof(out->body), &used, "%u", cursor + emitted);
    } else {
        append(out->body, sizeof(out->body), &used, "null");
    }
    append(out->body, sizeof(out->body), &used, "}");
    out->status = 200;
    out->length = used;
    return CP_OK;
}
static cp_error_t get_resource(cp_context_t *c, unsigned session, const char *resource,
                               cp_http_response_t *out)
{
    cp_identity_t who = identity(c, session);
    bool recording = strncmp(resource, "/api/v1/recordings", 18) == 0;
    if (!(who.scopes & (recording ? CP_SCOPE_RECORDING_READ : CP_SCOPE_VIEW))) {
        return failure(out, CP_FORBIDDEN);
    }
    if (strcmp(resource, "/api/v1/state") == 0) {
        return state_response(c, session, out);
    }
    if (strcmp(resource, "/api/v1/capabilities") == 0) {
        out->status = 200;
        out->length = (size_t)snprintf(
            out->body, sizeof(out->body),
            "{\"api_version\":1,\"catalog_sha256\":\"%s\",\"command_bytes\":%u,"
            "\"args_bytes\":%u,\"authenticated_clients\":%u,\"principals\":%u,"
            "\"results\":%u,\"websocket\":%s,\"binary_download\":%s,\"enrollment\":\"protected_"
            "local_"
            "import\",\"operations\":%u,\"configuration_fields\":%u}",
            cp_catalog_sha256(), CP_MAX_FRAME_BYTES, CP_MAX_ARGS_BYTES, CP_MAX_SESSIONS,
            CP_MAX_PRINCIPALS, CP_MAX_RESULTS, c->websocket_available ? "true" : "false",
            c->binary_download_available ? "true" : "false", (unsigned)cp_operation_count(),
            (unsigned)cp_config_count());
        return CP_OK;
    }
    if (strncmp(resource, "/api/v1/operations", 18) == 0 &&
        (resource[18] == 0 || resource[18] == '?')) {
        return metadata(c, session, resource, out, false);
    }
    if (strncmp(resource, "/api/v1/config/schema", 21) == 0 &&
        (resource[21] == 0 || resource[21] == '?')) {
        return metadata(c, session, resource, out, true);
    }
    if (strncmp(resource, "/api/v1/requests/", 17) == 0) {
        const char *p = resource + 17;
        uint64_t key = 0;
        if (!*p) {
            return failure(out, CP_INVALID);
        }
        for (; *p; ++p) {
            if (*p < '0' || *p > '9' || key > (UINT64_MAX - (unsigned)(*p - '0')) / 10U) {
                return failure(out, CP_INVALID);
            }
            key = key * 10U + (unsigned)(*p - '0');
        }
        record_t *r = find_key(c, key);
        if (r && r->session == session &&
            r->session_generation == c->sessions[session].generation) {
            return result_response(c, session, r, out);
        }
        return failure(out, CP_RESULT_UNKNOWN);
    }
    const char *const roots[] = {"/api/v1/config",
                                 "/api/v1/jobs/",
                                 "/api/v1/wifi/scans/",
                                 "/api/v1/wifi/profiles",
                                 "/api/v1/bluetooth/peers",
                                 "/api/v1/firmware/releases",
                                 "/api/v1/firmware/policy",
                                 "/api/v1/firmware/state",
                                 "/api/v1/speech/state",
                                 "/api/v1/audio/capabilities",
                                 "/api/v1/audio/state",
                                 "/api/v1/audio/sessions/",
                                 "/api/v1/recordings",
                                 "/api/v1/capture/grants",
                                 "/api/v1/clock",
                                 "/api/v1/trust",
                                 "/api/v1/imu",
                                 "/api/v1/display",
                                 "/api/v1/haptics"};
    bool known = false;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        size_t n = strlen(roots[i]);
        if (strncmp(resource, roots[i], n) == 0 && (roots[i][n - 1] == '/' || resource[n] == 0 ||
                                                    resource[n] == '?' || resource[n] == '/')) {
            known = true;
        }
    }
    if (!known) {
        return failure(out, CP_NOT_IMPLEMENTED);
    }
    if (strstr(resource, "/content")) {
        /* The HTTP owner serves authenticated binary chunks through its
         * separate streaming port; JSON/WS reads never relabel samples. */
        return failure(out, c->binary_download_available ? CP_INVALID : CP_UNAVAILABLE);
    }
    cp_error_t error =
        provider_read(c, session, resource, out->body, sizeof(out->body), &out->length);
    if (error != CP_OK) {
        return failure(out, error);
    }
    out->status = 200;
    return CP_OK;
}
static cp_error_t http_inner(cp_context_t *c, const char *method, const char *resource,
                             const cp_http_security_t *security, const char *body, size_t length,
                             cp_http_response_t *out)
{
    if (!security || !security->tls || !c->host[0] || !security->host ||
        strcmp(security->host, c->host) != 0) {
        return failure(out, CP_FORBIDDEN);
    }
    if (security->origin &&
        (strcmp(security->origin, c->origin) != 0 || strcmp(security->origin, "null") == 0)) {
        return failure(out, CP_FORBIDDEN);
    }
    if (!safe_resource(resource) || !method) {
        return failure(out, CP_INVALID);
    }
    if (length > CP_MAX_FRAME_BYTES) {
        return failure(out, CP_TOO_LARGE);
    }
    if (strcmp(resource, "/api/v1/login-challenge") == 0 && strcmp(method, "GET") == 0) {
        if (c->now >= c->challenge_until || !nonzero(c->challenge, 16)) {
            if (!random_bytes(c, c->challenge, 16)) {
                return failure(out, CP_UNAVAILABLE);
            }
            c->challenge_until = c->now + 60000U;
        }
        char nonce[33];
        encode(c->challenge, 16, nonce);
        snprintf(out->set_cookie, sizeof(out->set_cookie),
                 "__Host-control-preauth=%s; Secure; HttpOnly; SameSite=Strict; Path=/; Max-Age=60",
                 nonce);
        out->status = 200;
        out->length = (size_t)snprintf(out->body, sizeof(out->body), "{\"csrf\":\"%s\"}", nonce);
        return CP_OK;
    }
    if (strcmp(resource, "/api/v1/sessions") == 0 && strcmp(method, "POST") == 0) {
        return login(c, security, body, length, out);
    }
    bool mutation = strcmp(method, "GET") != 0;
    int session = authenticate(c, security, mutation);
    if (session < 0) {
        return failure(out, CP_UNAUTHENTICATED);
    }
    c->sessions[session].last_seen = c->now;
    if (strcmp(resource, "/api/v1/ws-ticket") == 0 && strcmp(method, "POST") == 0) {
        if (!c->websocket_available) {
            return failure(out, CP_UNAVAILABLE);
        }
        session_t *owner = &c->sessions[session];
        if (!owner->browser || length != 0) {
            return failure(out, CP_INVALID);
        }
        uint8_t secret[32];
        char ticket[65];
        if (!random_bytes(c, secret, sizeof(secret))) {
            return failure(out, CP_UNAVAILABLE);
        }
        cp_sha256(secret, sizeof(secret), owner->ws_ticket_hash);
        owner->ws_ticket_until = c->now + WS_TICKET_MS;
        encode(secret, sizeof(secret), ticket);
        wipe(secret, sizeof(secret));
        out->status = 200;
        out->length = (size_t)snprintf(out->body, sizeof(out->body),
                                       "{\"ticket\":\"%s\",\"expires_in_ms\":10000}", ticket);
        wipe(ticket, sizeof(ticket));
        return CP_OK;
    }
    if (strcmp(resource, "/api/v1/sessions/current") == 0 && strcmp(method, "GET") == 0) {
        out->status = 200;
        out->length = (size_t)snprintf(
            out->body, sizeof(out->body),
            "{\"authenticated\":true,\"media_idle_limit_ms\":5000,\"next_seq\":\"%llu\"}",
            (unsigned long long)c->sessions[session].sequence + 1U);
        return CP_OK;
    }
    if (strcmp(resource, "/api/v1/sessions/current") == 0 && strcmp(method, "DELETE") == 0) {
        close_session(c, (unsigned)session, CP_SAFETY_SESSION_LOST);
        snprintf(out->set_cookie, sizeof(out->set_cookie),
                 "__Host-control=; Secure; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        out->status = 200;
        memcpy(out->body, "{\"state\":\"completed\"}", 22);
        out->length = strlen(out->body);
        return CP_OK;
    }
    if (strcmp(resource, "/api/v1/commands") == 0 && strcmp(method, "POST") == 0) {
        return commands(c, (unsigned)session, body, length, out);
    }
    if (strcmp(method, "GET") == 0) {
        return get_resource(c, (unsigned)session, resource, out);
    }
    return failure(out, CP_NOT_IMPLEMENTED);
}
cp_error_t cp_http(cp_context_t *c, const char *method, const char *resource,
                   const cp_http_security_t *security, const char *body, size_t length,
                   uint64_t now, cp_http_response_t *out)
{
    if (!out) {
        return CP_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (!c) {
        return failure(out, CP_UNAVAILABLE);
    }
    cp_tick(c, now);
    cp_error_t error = c->clock_failed
                           ? failure(out, CP_UNAVAILABLE)
                           : http_inner(c, method, resource, security, body, length, out);
    wipe(&c->document, sizeof(c->document));
    wipe(c->canonical, sizeof(c->canonical));
    wipe(&c->command, sizeof(c->command));
    return error;
}
/* Small accessors for coordinator callbacks: do not allocate a full parser on a
 * latency-sensitive task stack. The source is already validated canonical JSON. */
static const char *skip_string(const char *p)
{
    if (*p != '"') {
        return NULL;
    }
    ++p;
    for (; *p; ++p) {
        if (*p == '"') {
            return p + 1;
        }
        if (*p == '\\') {
            if (!p[1]) {
                return NULL;
            }
            ++p;
        }
    }
    return NULL;
}
static const char *skip_value(const char *p)
{
    if (*p == '"') {
        return skip_string(p);
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        ++p;
        for (; *p; ++p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p) {
                    return NULL;
                }
                --p;
            } else if (*p == '{' || *p == '[') {
                p = skip_value(p);
                if (!p) {
                    return NULL;
                }
                --p;
            } else if (*p == close) {
                return p + 1;
            }
        }
        return NULL;
    }
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        ++p;
    }
    return p == start ? NULL : p;
}
static bool extract(const cp_command_t *cmd, const char *key, const char **start, size_t *length)
{
    if (!cmd || !key || strnlen(cmd->args_json, sizeof(cmd->args_json)) == sizeof(cmd->args_json)) {
        return false;
    }
    const char *p = cmd->args_json;
    if (*p++ != '{') {
        return false;
    }
    while (*p && *p != '}') {
        const char *end = skip_string(p);
        if (!end || *end != ':') {
            return false;
        }
        bool match = (size_t)(end - p) == strlen(key) + 2 && memcmp(p + 1, key, strlen(key)) == 0;
        p = end + 1;
        end = skip_value(p);
        if (!end) {
            return false;
        }
        if (match) {
            *start = p;
            *length = (size_t)(end - p);
            return true;
        }
        p = end;
        if (*p == ',') {
            ++p;
        } else if (*p != '}') {
            return false;
        }
    }
    return false;
}
bool cp_command_json(const cp_command_t *cmd, const char *key, char *out, size_t capacity)
{
    const char *start;
    size_t length;
    if (!out || !capacity) {
        return false;
    }
    out[0] = 0;
    if (!extract(cmd, key, &start, &length) || length >= capacity) {
        return false;
    }
    memcpy(out, start, length);
    out[length] = 0;
    return true;
}
bool cp_command_integer(const cp_command_t *cmd, const char *key, int64_t *out)
{
    char value[64];
    if (!out || !cp_command_json(cmd, key, value, sizeof(value))) {
        return false;
    }
    char *end;
    errno = 0;
    long long n = strtoll(value, &end, 10);
    if (errno || *end) {
        return false;
    }
    *out = (int64_t)n;
    return true;
}
bool cp_command_boolean(const cp_command_t *cmd, const char *key, bool *out)
{
    char value[8];
    if (!out || !cp_command_json(cmd, key, value, sizeof(value))) {
        return false;
    }
    if (strcmp(value, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}
bool cp_command_string(const cp_command_t *cmd, const char *key, char *out, size_t capacity)
{
    const char *p;
    size_t length;
    if (!out || !capacity) {
        return false;
    }
    out[0] = 0;
    if (!extract(cmd, key, &p, &length) || length < 2 || *p != '"') {
        return false;
    }
    size_t used = 0;
    const char *end = p + length - 1;
    ++p;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= end) {
                return false;
            }
            c = (unsigned char)*p++;
            if (c == 'u') {
                if (end - p < 4 || p[0] != '0' || p[1] != '0') {
                    return false;
                }
                unsigned value = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    char h = *p++;
                    unsigned v;
                    if (h >= '0' && h <= '9') {
                        v = (unsigned)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        v = (unsigned)(h - 'a' + 10);
                    } else {
                        return false;
                    }
                    value = value * 16U + v;
                }
                if (!value) {
                    return false;
                }
                c = (unsigned char)value;
            } else if (c != '\\' && c != '"' && c != '/') {
                return false;
            }
        }
        if (used + 1 >= capacity) {
            wipe(out, capacity);
            return false;
        }
        out[used++] = (char)c;
    }
    out[used] = 0;
    return true;
}

void cp_set_transport_capabilities(cp_context_t *c, bool ws, bool binary)
{
    if (c) {
        c->websocket_available = ws;
        c->binary_download_available = binary;
    }
}
static int transport_session(cp_context_t *c, const cp_identity_t *who, uint64_t now)
{
    if (!c || !who) {
        return -1;
    }
    cp_tick(c, now);
    if (c->clock_failed) {
        return -1;
    }
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        session_t *s = &c->sessions[i];
        if (!s->used) {
            continue;
        }
        cp_identity_t actual = identity(c, i);
        if (actual.browser == who->browser && actual.scopes == who->scopes &&
            equal(actual.principal_id, who->principal_id, 16) &&
            equal(actual.session_id, who->session_id, 16)) {
            s->last_seen = c->now;
            return (int)i;
        }
    }
    return -1;
}
cp_error_t cp_transport_authenticate(cp_context_t *c, const cp_http_security_t *security,
                                     bool mutation, uint64_t now, cp_identity_t *out)
{
    if (!c || !out) {
        return CP_INVALID;
    }
    wipe(out, sizeof(*out));
    cp_tick(c, now);
    if (c->clock_failed) {
        return CP_UNAVAILABLE;
    }
    if (!security || !security->tls || !security->host || !c->host[0] ||
        strcmp(security->host, c->host) ||
        (security->origin &&
         (strcmp(security->origin, c->origin) || !strcmp(security->origin, "null")))) {
        return CP_FORBIDDEN;
    }
    int index = authenticate(c, security, mutation);
    if (index < 0) {
        return CP_UNAUTHENTICATED;
    }
    c->sessions[index].last_seen = c->now;
    *out = identity(c, (unsigned)index);
    return CP_OK;
}
cp_error_t cp_transport_upgrade(cp_context_t *c, const cp_http_security_t *security,
                                const char *ticket, uint64_t now, cp_identity_t *out)
{
    cp_error_t error = cp_transport_authenticate(c, security, false, now, out);
    if (error != CP_OK) {
        return error;
    }
    if (!out->browser) {
        if (!ticket) {
            return CP_OK;
        }
        wipe(out, sizeof(*out));
        return CP_INVALID;
    }
    /* Browser cookies alone never authorize a channel. The CSRF-protected
     * issuer binds a random single-use ticket to this exact logical session. */
    if (!security->origin) {
        wipe(out, sizeof(*out));
        return CP_FORBIDDEN;
    }
    uint8_t secret[32], digest[32];
    if (!decode(ticket, secret, sizeof(secret))) {
        wipe(out, sizeof(*out));
        return CP_UNAUTHENTICATED;
    }
    cp_sha256(secret, sizeof(secret), digest);
    wipe(secret, sizeof(secret));
    error = CP_UNAUTHENTICATED;
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        session_t *s = &c->sessions[i];
        if (s->used && equal(s->id, out->session_id, 16) && s->ws_ticket_until > c->now &&
            equal(s->ws_ticket_hash, digest, sizeof(digest))) {
            wipe(s->ws_ticket_hash, sizeof(s->ws_ticket_hash));
            s->ws_ticket_until = 0;
            error = CP_OK;
            break;
        }
    }
    wipe(digest, sizeof(digest));
    if (error != CP_OK) {
        wipe(out, sizeof(*out));
    }
    return error;
}
cp_error_t cp_transport_heartbeat(cp_context_t *c, const cp_identity_t *who, uint64_t now)
{
    return transport_session(c, who, now) >= 0 ? CP_OK : CP_UNAUTHENTICATED;
}
cp_error_t cp_transport_frame(cp_context_t *c, const cp_identity_t *who, const char *frame,
                              size_t length, uint64_t now, cp_http_response_t *out)
{
    if (!out) {
        return CP_INVALID;
    }
    memset(out, 0, sizeof(*out));
    int session = transport_session(c, who, now);
    if (session < 0) {
        return failure(out, CP_UNAUTHENTICATED);
    }
    cp_error_t error = CP_INVALID;
    if (!frame || length > CP_MAX_FRAME_BYTES) {
        error = CP_TOO_LARGE;
    } else if (cj_parse(&c->document, frame, length) && c->document.nodes[0].type == CJ_OBJECT) {
        uint16_t op = cj_get(&c->document, 0, "op");
        if (op != CJ_NONE) {
            error = commands(c, (unsigned)session, frame, length, out);
        } else {
            unsigned count = 0;
            for (uint16_t t = c->document.nodes[0].child; t != CJ_NONE;
                 t = c->document.nodes[t].next) {
                ++count;
            }
            int64_t version = 0;
            const char *kind = cj_string(&c->document, cj_get(&c->document, 0, "type"));
            const char *resource = cj_string(&c->document, cj_get(&c->document, 0, "resource"));
            if (count == 2 && cj_integer(&c->document, cj_get(&c->document, 0, "v"), &version) &&
                version == 1) {
                if (kind && !strcmp(kind, "heartbeat")) {
                    out->status = 200;
                    out->length =
                        (size_t)snprintf(out->body, sizeof(out->body),
                                         "{\"authenticated\":true,\"media_idle_limit_ms\":5000}");
                    error = CP_OK;
                } else if (resource && strlen(resource) < 160 && safe_resource(resource)) {
                    char path[160];
                    strcpy(path, resource);
                    error = get_resource(c, (unsigned)session, path, out);
                }
            }
        }
    }
    wipe(&c->document, sizeof(c->document));
    wipe(c->canonical, sizeof(c->canonical));
    wipe(&c->command, sizeof(c->command));
    if (!out->length) {
        return failure(out, error);
    }
    return error;
}

cp_error_t cp_transport_close(cp_context_t *c, const cp_identity_t *who, uint64_t now)
{
    if (!c || !who) {
        return CP_INVALID;
    }
    cp_tick(c, now);
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        if (!c->sessions[i].used) {
            continue;
        }
        cp_identity_t actual = identity(c, i);
        if (actual.browser == who->browser && equal(actual.principal_id, who->principal_id, 16) &&
            equal(actual.session_id, who->session_id, 16)) {
            /* Dedicated input-channel loss does not revoke independent HTTP
             * management or file capture. Expiry/logout use close_session. */
            if ((c->lease.active || c->lease.pending) && c->lease.session == i &&
                c->lease.session_generation == c->sessions[i].generation) {
                revoke_lease(c, CP_SAFETY_STATE_CHANGED);
            } else {
                revoke_records(c, i, true, CP_STALE);
                if (c->callbacks.safety) {
                    c->callbacks.safety(c->callbacks.user, CP_SAFETY_STATE_CHANGED, &actual);
                }
            }
            return CP_OK;
        }
    }
    return CP_UNAUTHENTICATED;
}
