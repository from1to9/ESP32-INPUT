// SPDX-License-Identifier: Apache-2.0
#include "app_command_bridge.h"
#include <string.h>

#define SAFETY_CAPACITY 4U
typedef struct {
    cp_command_t command;
    bool cancelled;
} queued_command_t;
typedef struct {
    bool used, done;
    queued_command_t queued;
    cp_result_t result;
} priority_command_t;
typedef struct {
    bool used, pending, working, ready;
    cp_identity_t identity;
    char resource[APP_COMMAND_RESOURCE_BYTES];
    char json[CP_MAX_RESPONSE_BYTES + 1U];
    size_t length;
    cp_error_t error;
    uint64_t completed_ms;
} read_slot_t;
typedef struct { cp_safety_reason_t reason; bool global; cp_identity_t identity; } safety_t;
struct app_command_bridge {
    app_command_bridge_port_t port;
    queued_command_t commands[APP_COMMAND_CAPACITY];
    uint8_t head, count;
    priority_command_t priority[APP_COMMAND_SAFETY_CAPACITY];
    queued_command_t flight;
    bool admitting;
    bool available[APP_COMMAND_OPERATIONS];
    safety_t safety[SAFETY_CAPACITY];
    uint8_t safety_count;
    bool safety_overflow, overflow_locked, overflow_management;
    cp_result_t retained_result;
    bool result_pending;
    read_slot_t reads[APP_COMMAND_READS];
};
static void wipe(void *pointer, size_t bytes)
{
    volatile unsigned char *p = pointer;
    while (bytes--) *p++ = 0;
}
static bool bounded_resource(const char *resource)
{
    if (!resource) return false;
    for (unsigned i = 0; i < APP_COMMAND_RESOURCE_BYTES; ++i)
        if (!resource[i]) return true;
    return false;
}
static bool same_identity(const cp_identity_t *a, const cp_identity_t *b)
{
    return a->scopes == b->scopes && a->browser == b->browser &&
        !memcmp(a->principal_id, b->principal_id, CP_ID_BYTES) &&
        !memcmp(a->session_id, b->session_id, CP_ID_BYTES);
}
static bool same_session(const cp_identity_t *a, const cp_identity_t *b)
{
    return !memcmp(a->principal_id, b->principal_id, CP_ID_BYTES) &&
        !memcmp(a->session_id, b->session_id, CP_ID_BYTES);
}
size_t app_command_bridge_bytes(void) { return sizeof(app_command_bridge_t); }
size_t app_command_bridge_alignment(void) { return _Alignof(app_command_bridge_t); }
cp_error_t app_command_bridge_init(void *memory, size_t bytes,
    const app_command_bridge_port_t *port, app_command_bridge_t **out)
{
    if (!memory || bytes < sizeof(app_command_bridge_t) || !out || !port ||
        (uintptr_t)memory % _Alignof(app_command_bridge_t) ||
        !port->lock || !port->unlock || !port->now_ms || !port->available ||
        !port->execute || !port->safety || !port->read || !port->complete) return CP_INVALID;
    app_command_bridge_t *b = memory;
    memset(b, 0, sizeof(*b)); b->port = *port; *out = b;
    return CP_OK;
}
bool app_command_bridge_available(void *context, cp_operation_id_t operation,
    char reason[CP_REASON_CAPACITY])
{
    app_command_bridge_t *b = context;
    bool available = false;
    if (b && operation < APP_COMMAND_OPERATIONS) {
        b->port.lock(b->port.user); available = b->available[operation]; b->port.unlock(b->port.user);
    }
    if (reason) {
        static const char message[] = "provider_unavailable";
        if (available) reason[0] = 0; else memcpy(reason, message, sizeof(message));
    }
    return available;
}
static const cp_operation_info_t *operation_info(cp_operation_id_t id)
{
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *op = cp_operation_at(i);
        if (op && op->id == id) return op;
    }
    return NULL;
}
cp_error_t app_command_bridge_submit(void *context, const cp_command_t *command, cp_result_t *result)
{
    app_command_bridge_t *b = context;
    if (!b || !command || !result || !command->command_key ||
        command->operation >= APP_COMMAND_OPERATIONS ||
        !memchr(command->args_json, 0, sizeof(command->args_json))) return CP_INVALID;
    uint64_t now = b->port.now_ms(b->port.user);
    const cp_operation_info_t *op = operation_info(command->operation);
    bool priority = op && op->safety;
    if (!priority && command->deadline_ms <= now) return CP_EXPIRED;
    cp_error_t error = CP_OK;
    b->port.lock(b->port.user);
    if (!priority && !b->available[command->operation]) error = CP_UNAVAILABLE;
    else if (priority) {
        unsigned slot = 0;
        while (slot < APP_COMMAND_SAFETY_CAPACITY && b->priority[slot].used) ++slot;
        if (slot == APP_COMMAND_SAFETY_CAPACITY) error = CP_BUSY;
        else b->priority[slot] = (priority_command_t){.used = true, .queued = {.command = *command}};
    } else if (b->count == APP_COMMAND_CAPACITY) error = CP_BUSY;
    else {
        unsigned tail = (unsigned)(b->head + b->count) % APP_COMMAND_CAPACITY;
        b->commands[tail] = (queued_command_t){.command = *command}; ++b->count;
    }
    if (error == CP_OK) *result = (cp_result_t){.state = CP_RESULT_ACCEPTED,
        .command_key = command->command_key, .uncertain_index = -1};
    b->port.unlock(b->port.user);
    return error;
}
static bool cancelled_by(cp_operation_id_t id, bool input_only)
{
    const cp_operation_info_t *op = operation_info(id);
    /* Reducing authority remains admissible after the corresponding protocol
     * callback revokes the lease. It must not cancel its own queued stop. */
    return op && !op->safety && (!input_only || op->input);
}
static bool input_only_safety(cp_safety_reason_t reason)
{
    return reason == CP_SAFETY_LOCAL_OVERRIDE || reason == CP_SAFETY_LEASE_EXPIRED ||
        reason == CP_SAFETY_STATE_CHANGED || reason == CP_SAFETY_CLIENT_STOP;
}
void app_command_bridge_safety(void *context, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    app_command_bridge_t *b = context; if (!b) return;
    if (reason == CP_SAFETY_LOCKED) identity = NULL; /* Device lock is global, including queued managers. */
    b->port.lock(b->port.user);
    if (b->safety_count < SAFETY_CAPACITY) {
        safety_t *s = &b->safety[b->safety_count++];
        *s = (safety_t){.reason = reason, .global = !identity};
        if (identity) s->identity = *identity;
    } else {
        b->safety_overflow = true;
        if (!input_only_safety(reason)) b->overflow_management = true;
        if (reason == CP_SAFETY_LOCKED) b->overflow_locked = true;
    }
    /* An admitted effect is never dispatched after its queued revocation. */
    for (unsigned n = 0; n < b->count; ++n) {
        queued_command_t *c = &b->commands[(b->head + n) % APP_COMMAND_CAPACITY];
        if (cancelled_by(c->command.operation, !b->overflow_management && input_only_safety(reason)) &&
            (!identity || same_session(identity, &c->command.identity) || b->safety_overflow)) c->cancelled = true;
    }
    if (b->admitting && cancelled_by(b->flight.command.operation,
            !b->overflow_management && input_only_safety(reason)) &&
        (!identity || same_session(identity, &b->flight.command.identity) || b->safety_overflow))
        b->flight.cancelled = true;
    for (unsigned i = 0; i < APP_COMMAND_READS; ++i) {
        read_slot_t *r = &b->reads[i];
        if ((b->overflow_management || !input_only_safety(reason)) &&
            (!identity || same_session(identity, &r->identity) || b->safety_overflow)) {
            r->ready = false; r->pending = false;
            if (!r->working) wipe(r, sizeof(*r));
        }
    }
    b->port.unlock(b->port.user);
}
cp_error_t app_command_bridge_read(void *context, const cp_identity_t *identity,
    const char *resource, char *json, size_t capacity, size_t *length)
{
    app_command_bridge_t *b = context;
    if (!b || !identity || !resource || !json || !length ||
        !bounded_resource(resource)) return CP_INVALID;
    *length = 0; uint64_t now = b->port.now_ms(b->port.user);
    cp_error_t error = CP_BUSY; int reusable = -1;
    b->port.lock(b->port.user);
    for (unsigned i = 0; i < APP_COMMAND_READS; ++i) {
        read_slot_t *r = &b->reads[i];
        bool recent = r->ready && now >= r->completed_ms &&
            now - r->completed_ms <= APP_COMMAND_READ_MAX_AGE_MS;
        if (r->used && same_identity(identity, &r->identity) && !strcmp(resource, r->resource)) {
            if (recent) {
                error = r->error;
                if (error == CP_OK) {
                    if (r->length >= capacity) error = CP_TOO_LARGE;
                    else { memcpy(json, r->json, r->length + 1); *length = r->length; }
                }
            } else if (!r->pending && !r->working) { r->ready = false; r->pending = true; }
            b->port.unlock(b->port.user); return error;
        }
        if (!r->working && !r->pending && (!r->used || !recent) && reusable < 0) reusable = (int)i;
    }
    if (reusable >= 0) {
        read_slot_t *r = &b->reads[reusable]; wipe(r, sizeof(*r));
        r->used = r->pending = true; r->identity = *identity;
        memcpy(r->resource, resource, strlen(resource) + 1);
    }
    b->port.unlock(b->port.user); return error;
}
void app_command_bridge_refresh(app_command_bridge_t *b)
{
    if (!b) return;
    bool available[APP_COMMAND_OPERATIONS] = {false};
    char reason[CP_REASON_CAPACITY];
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *op = cp_operation_at(i);
        if (op && op->id < APP_COMMAND_OPERATIONS)
            available[op->id] = b->port.available(b->port.user, op->id, reason);
    }
    b->port.lock(b->port.user); memcpy(b->available, available, sizeof(available)); b->port.unlock(b->port.user);
}
static void service_safety(app_command_bridge_t *b)
{
    safety_t safety[SAFETY_CAPACITY]; unsigned count; bool overflow, locked, management;
    b->port.lock(b->port.user);
    count = b->safety_count; overflow = b->safety_overflow;
    locked = b->overflow_locked; management = b->overflow_management;
    memcpy(safety, b->safety, count * sizeof(*safety));
    b->safety_count = 0; b->safety_overflow = b->overflow_locked = b->overflow_management = false;
    wipe(b->safety, sizeof(b->safety)); b->port.unlock(b->port.user);
    if (overflow) b->port.safety(b->port.user,
        locked ? CP_SAFETY_LOCKED : management ? CP_SAFETY_REVOKED : CP_SAFETY_LOCAL_OVERRIDE, NULL);
    for (unsigned i = 0; i < count; ++i)
        b->port.safety(b->port.user, safety[i].reason, safety[i].global ? NULL : &safety[i].identity);
    wipe(safety, sizeof(safety));
}
static cp_result_t execute_flight(app_command_bridge_t *b)
{
    uint64_t now = b->port.now_ms(b->port.user);
    /* A revocation arriving at any boundary after dequeue is still able to
     * cancel flight. Drain it before the final admission linearization below. */
    service_safety(b);
    b->port.lock(b->port.user);
    bool cancelled = b->flight.cancelled;
    b->admitting = false;
    b->port.unlock(b->port.user);
    /* No external clock/provider call lies between final admission and effect.
     * Safety after this point belongs to the owning manager's cancellation
     * protocol. The bridge never holds its cross-task lock over effects. */
    cp_result_t result = {.state = CP_RESULT_REJECTED, .uncertain_index = -1};
    const cp_operation_info_t *op = operation_info(b->flight.command.operation);
    cp_error_t error = cancelled ? CP_STALE :
        !(op && op->safety) && b->flight.command.deadline_ms <= now ? CP_EXPIRED : CP_OK;
    if (error == CP_OK) error = b->port.execute(b->port.user, &b->flight.command, &result);
    if (error != CP_OK) { result.state = CP_RESULT_REJECTED; result.error = error; }
    result.command_key = b->flight.command.command_key;
    b->port.lock(b->port.user); wipe(&b->flight, sizeof(b->flight)); b->port.unlock(b->port.user);
    return result;
}
void app_command_bridge_service(app_command_bridge_t *b)
{
    if (!b) return;
    service_safety(b);
    /* Safety commands and their completion identities have capacity independent
     * of ordinary work, matching the protocol's complete retained-result bound.
     * A retained ordinary result cannot block a new lock/release/stop effect. */
    for (unsigned i = 0; i < APP_COMMAND_SAFETY_CAPACITY; ++i) {
        b->port.lock(b->port.user);
        bool done = b->priority[i].used && b->priority[i].done;
        cp_result_t retained = b->priority[i].result;
        b->port.unlock(b->port.user);
        if (done && b->port.complete(b->port.user, &retained)) {
            b->port.lock(b->port.user); wipe(&b->priority[i], sizeof(b->priority[i])); b->port.unlock(b->port.user);
        }
    }
    int priority = -1;
    b->port.lock(b->port.user);
    for (unsigned i = 0; i < APP_COMMAND_SAFETY_CAPACITY; ++i)
        if (b->priority[i].used && !b->priority[i].done) { priority = (int)i; break; }
    if (priority >= 0) { b->flight = b->priority[priority].queued; b->admitting = true; }
    b->port.unlock(b->port.user);
    if (priority >= 0) {
        cp_result_t result = execute_flight(b);
        b->port.lock(b->port.user);
        b->priority[priority].result = result; b->priority[priority].done = true;
        b->port.unlock(b->port.user);
    }
    if (b->result_pending) {
        if (!b->port.complete(b->port.user, &b->retained_result)) return;
        b->result_pending = false;
    }
    bool have = false;
    b->port.lock(b->port.user);
    if (b->count) {
        b->flight = b->commands[b->head]; wipe(&b->commands[b->head], sizeof(b->flight));
        b->head = (uint8_t)((b->head + 1U) % APP_COMMAND_CAPACITY); --b->count;
        b->admitting = have = true;
    }
    b->port.unlock(b->port.user);
    if (have) {
        cp_result_t result = execute_flight(b);
        if (!b->port.complete(b->port.user, &result)) {
            b->retained_result = result; b->result_pending = true;
        }
    }
    read_slot_t *read = NULL;
    b->port.lock(b->port.user);
    for (unsigned i = 0; i < APP_COMMAND_READS; ++i)
        if (b->reads[i].pending && !b->reads[i].working) { read = &b->reads[i]; read->working = true; break; }
    b->port.unlock(b->port.user);
    if (read) {
        size_t bytes = 0;
        cp_error_t error = b->port.read(b->port.user, &read->identity, read->resource,
            read->json, sizeof(read->json), &bytes);
        if (error == CP_OK && (bytes > CP_MAX_RESPONSE_BYTES || read->json[bytes] != 0)) error = CP_INTERNAL;
        uint64_t completed = b->port.now_ms(b->port.user);
        b->port.lock(b->port.user);
        if (read->pending) {
            read->error = error; read->length = error == CP_OK ? bytes : 0;
            if (error != CP_OK) wipe(read->json, sizeof(read->json));
            read->completed_ms = completed; read->ready = true; read->pending = read->working = false;
        } else wipe(read, sizeof(*read));
        b->port.unlock(b->port.user);
    }
}
