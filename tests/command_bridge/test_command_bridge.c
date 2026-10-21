// SPDX-License-Identifier: Apache-2.0
#include "app_command_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    app_command_bridge_t *bridge;
    uint64_t now;
    bool locked, completion_allowed, revoke_during_read, revoke_at_clock;
    unsigned effects, reads, completions, safeties, global_locks;
    cp_result_t result;
    cp_identity_t identity;
} fixture_t;
static const cp_operation_info_t operation = {.id = 3, .input = false};
size_t cp_operation_count(void) { return 1; }
const cp_operation_info_t *cp_operation_at(size_t i) { assert(!i); return &operation; }
static void lock(void *context) { fixture_t *f = context; assert(!f->locked); f->locked = true; }
static void unlock(void *context) { fixture_t *f = context; assert(f->locked); f->locked = false; }
static uint64_t now(void *context)
{
    fixture_t *f = context;
    if (f->revoke_at_clock) {
        f->revoke_at_clock = false;
        app_command_bridge_safety(f->bridge, CP_SAFETY_REVOKED, &f->identity);
    }
    return f->now;
}
static bool available(void *context, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{ assert(!((fixture_t *)context)->locked); reason[0] = 0; return op == 3; }
static cp_error_t execute(void *context, const cp_command_t *c, cp_result_t *r)
{
    fixture_t *f = context; assert(!f->locked); assert(c->operation == 3);
    ++f->effects; r->state = CP_RESULT_APPLIED; return CP_OK;
}
static bool complete(void *context, const cp_result_t *r)
{
    fixture_t *f = context; assert(!f->locked);
    if (!f->completion_allowed) return false;
    f->result = *r; ++f->completions; return true;
}
static void safety(void *context, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    fixture_t *f = context; assert(!f->locked); ++f->safeties;
    if (reason == CP_SAFETY_LOCKED && !identity) ++f->global_locks;
}
static cp_error_t read_value(void *context, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    fixture_t *f = context; assert(!f->locked); assert(!strcmp(resource, "/api/v1/config"));
    ++f->reads;
    int n = snprintf(json, capacity, "{\"principal\":%u}", identity->principal_id[0]);
    assert(n > 0); *length = (size_t)n;
    if (f->revoke_during_read) app_command_bridge_safety(f->bridge, CP_SAFETY_REVOKED, identity);
    return CP_OK;
}
static fixture_t *create(void)
{
    fixture_t *f = calloc(1, sizeof(*f)); assert(f);
    f->now = 100; f->completion_allowed = true;
    f->identity.principal_id[0] = 1; f->identity.session_id[0] = 2;
    app_command_bridge_port_t port = {.user = f, .lock = lock, .unlock = unlock, .now_ms = now,
        .available = available, .execute = execute, .safety = safety, .read = read_value, .complete = complete};
    void *memory = malloc(app_command_bridge_bytes()); assert(memory);
    assert(app_command_bridge_init(memory, app_command_bridge_bytes(), &port, &f->bridge) == CP_OK);
    app_command_bridge_refresh(f->bridge); return f;
}
static void destroy(fixture_t *f) { free(f->bridge); free(f); }
static cp_command_t command(fixture_t *f, uint64_t key)
{
    cp_command_t c = {.operation = 3, .command_key = key, .deadline_ms = 1000, .identity = f->identity};
    memcpy(c.args_json, "{}", 3); return c;
}
static void queue(fixture_t *f, uint64_t key)
{
    cp_command_t c = command(f, key); cp_result_t r;
    assert(app_command_bridge_submit(f->bridge, &c, &r) == CP_OK && r.state == CP_RESULT_ACCEPTED);
    memset(&c, 0xa5, sizeof(c)); /* Accepted payload must already be an owned copy. */
}
static void bounded_queue_and_exact_once_after_completion_backpressure(void)
{
    fixture_t *f = create(); cp_result_t r;
    for (uint64_t i = 1; i <= APP_COMMAND_CAPACITY; ++i) queue(f, i);
    cp_command_t c = command(f, 99);
    assert(app_command_bridge_submit(f->bridge, &c, &r) == CP_BUSY);
    f->completion_allowed = false;
    for (unsigned i = 0; i < 100; ++i) app_command_bridge_service(f->bridge);
    assert(f->effects == 1 && !f->completions);
    f->completion_allowed = true;
    for (unsigned i = 0; i < APP_COMMAND_CAPACITY; ++i) app_command_bridge_service(f->bridge);
    assert(f->effects == APP_COMMAND_CAPACITY && f->completions == APP_COMMAND_CAPACITY);
    assert(f->result.command_key == APP_COMMAND_CAPACITY && f->result.state == CP_RESULT_APPLIED);
    destroy(f);
}
static void expired_and_revoked_commands_never_execute(void)
{
    fixture_t *f = create(); queue(f, 1); f->now = 1000;
    app_command_bridge_service(f->bridge);
    assert(!f->effects && f->result.error == CP_EXPIRED);
    f->now = 100; queue(f, 2);
    app_command_bridge_safety(f->bridge, CP_SAFETY_REVOKED, &f->identity);
    app_command_bridge_service(f->bridge);
    assert(!f->effects && f->result.error == CP_STALE && f->safeties == 1);
    /* Safety stays live while an already executed command's result is retained. */
    queue(f, 3); f->completion_allowed = false; app_command_bridge_service(f->bridge);
    for (unsigned i = 0; i < 4; ++i) app_command_bridge_safety(f->bridge, CP_SAFETY_REVOKED, &f->identity);
    app_command_bridge_safety(f->bridge, CP_SAFETY_LOCKED, NULL);
    app_command_bridge_service(f->bridge);
    assert(f->global_locks == 1 && f->effects == 1);
    destroy(f);
}
static void reads_require_exact_identity_recent_completion_and_no_revocation(void)
{
    fixture_t *f = create(); char json[80]; size_t length;
    assert(app_command_bridge_read(f->bridge, &f->identity, "/api/v1/config", json, sizeof(json), &length) == CP_BUSY);
    app_command_bridge_service(f->bridge);
    assert(app_command_bridge_read(f->bridge, &f->identity, "/api/v1/config", json, sizeof(json), &length) == CP_OK);
    assert(!strcmp(json, "{\"principal\":1}"));
    cp_identity_t other = f->identity; other.principal_id[0] = 9;
    assert(app_command_bridge_read(f->bridge, &other, "/api/v1/config", json, sizeof(json), &length) == CP_BUSY);
    app_command_bridge_service(f->bridge);
    assert(app_command_bridge_read(f->bridge, &other, "/api/v1/config", json, sizeof(json), &length) == CP_OK);
    assert(!strcmp(json, "{\"principal\":9}"));
    f->now += APP_COMMAND_READ_MAX_AGE_MS + 1;
    assert(app_command_bridge_read(f->bridge, &f->identity, "/api/v1/config", json, sizeof(json), &length) == CP_BUSY);
    f->revoke_during_read = true; app_command_bridge_service(f->bridge);
    assert(app_command_bridge_read(f->bridge, &f->identity, "/api/v1/config", json, sizeof(json), &length) == CP_BUSY);
    destroy(f);
}
static void local_takeover_preserves_independent_management_commands(void)
{
    fixture_t *f = create(); queue(f, 1);
    for (unsigned i = 0; i < 12; ++i)
        app_command_bridge_safety(f->bridge, CP_SAFETY_LOCAL_OVERRIDE, NULL);
    app_command_bridge_service(f->bridge);
    assert(f->effects == 1 && f->result.state == CP_RESULT_APPLIED);
    destroy(f);
}
static void dequeued_command_remains_revocable_at_clock_boundary(void)
{
    fixture_t *f = create(); queue(f, 1); f->revoke_at_clock = true;
    app_command_bridge_service(f->bridge);
    assert(f->effects == 0 && f->safeties == 1 && f->result.error == CP_STALE);
    destroy(f);
}
int main(void)
{
    dequeued_command_remains_revocable_at_clock_boundary();
    local_takeover_preserves_independent_management_commands();
    bounded_queue_and_exact_once_after_completion_backpressure();
    expired_and_revoked_commands_never_execute();
    reads_require_exact_identity_recent_completion_and_no_revocation();
    puts("PASS command bridge: bounded admission, retained results, safety priority, expiry, identity-scoped reads");
}
