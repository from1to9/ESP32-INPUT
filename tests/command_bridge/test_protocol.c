// SPDX-License-Identifier: Apache-2.0
/* Reuse the actual protocol's authenticated HTTP fixture and JSON helpers.
 * Only its effect provider is replaced with the production command bridge. */
#define main protocol_fixture_unused_main
#include "../control_protocol/test_control.c"
#undef main
#include "app_command_bridge.h"

static app_command_bridge_t *bridge;
static bool bridge_locked, allow_completion;
static unsigned ordinary_effects, lock_effects, release_effects, owner_safety_calls;
static void bridge_lock(void *user) { (void)user; assert(!bridge_locked); bridge_locked = true; }
static void bridge_unlock(void *user) { (void)user; assert(bridge_locked); bridge_locked = false; }
static uint64_t bridge_time(void *user) { (void)user; return clock_ms; }
static cp_error_t bridge_dispatch(void *user, const cp_command_t *c, cp_result_t *r)
{ (void)user; return app_command_bridge_submit(bridge, c, r); }
static void bridge_safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    (void)user; ++owner_safety_calls;
    app_command_bridge_safety(bridge, reason, identity);
}
static void owner_safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{ (void)user; (void)reason; (void)identity; assert(!bridge_locked); }
static bool bridge_complete(void *user, const cp_result_t *result)
{ (void)user; (void)result; assert(!bridge_locked); return allow_completion; }
static cp_error_t owner_execute(void *user, const cp_command_t *c, cp_result_t *r)
{
    (void)user; assert(!bridge_locked);
    if (c->operation == CP_OP_DEVICE_LOCK) ++lock_effects;
    else if (c->operation == CP_OP_INPUT_RELEASE_ALL) ++release_effects;
    else ++ordinary_effects;
    r->state = CP_RESULT_APPLIED; return CP_OK;
}
static void prepare(void)
{
    reset(CP_SCOPE_ALL, false);
    void *memory = calloc(1, app_command_bridge_bytes()); assert(memory);
    app_command_bridge_port_t port = {.lock = bridge_lock, .unlock = bridge_unlock,
        .now_ms = bridge_time, .available = available, .execute = owner_execute,
        .safety = owner_safety, .read = read_resource, .complete = bridge_complete};
    assert(app_command_bridge_init(memory, app_command_bridge_bytes(), &port, &bridge) == CP_OK);
    app_command_bridge_refresh(bridge);
    free(context); context = NULL;
    cp_callbacks_t callbacks = {.user = bridge, .random = random_source,
        .available = app_command_bridge_available, .dispatch = bridge_dispatch,
        .safety = bridge_safety, .read = read_resource};
    memory = calloc(1, cp_context_size()); assert(memory);
    assert(cp_initialize(memory, cp_context_size(), &callbacks, &context) == CP_OK);
    assert(cp_http_configure(context, "device.test", "https://device.test") == CP_OK);
    uint8_t secret[32]; for (unsigned i = 0; i < 32; ++i) secret[i] = (uint8_t)(i + 11);
    cp_principal_t principal = {.scopes = CP_SCOPE_ALL, .native_only = true, .enabled = true};
    memset(principal.id, 0x41, 16); cp_sha256(secret, sizeof(secret), principal.credential_sha256);
    assert(cp_load_principal(context, &principal) == CP_OK);
    cp_set_device_state(context, &device, clock_ms);
    login_native();
}
static void add_ordinary(uint64_t key)
{
    cp_command_t c = {.operation = CP_OP_CONFIG_APPLY, .command_key = key, .deadline_ms = clock_ms + 10000};
    memcpy(c.args_json, "{}", 3); cp_result_t r;
    assert(app_command_bridge_submit(bridge, &c, &r) == CP_OK);
}
int main(void)
{
    prepare();
    allow_completion = false;
    add_ordinary(1000); app_command_bridge_service(bridge);
    assert(ordinary_effects == 1); /* Ordinary result now retained/backpressured. */
    for (unsigned i = 0; i < APP_COMMAND_CAPACITY; ++i) add_ordinary(1001U + i);
    assert(send("device.lock", "{}", true, false) == CP_OK);
    assert(owner_safety_calls == 1);
    app_command_bridge_service(bridge);
    assert(lock_effects == 1 && ordinary_effects == 1);
    /* A protocol-authorized neutral release survives ordinary deadline and
     * capability loss. Its own CLIENT_STOP callback cannot cancel the release. */
    (void)command("input.release_all", "{}", true, false, ++seq);
    clock_ms += 101;
    is_available = false; app_command_bridge_refresh(bridge);
    assert(request("POST", "/api/v1/commands", envelope) == CP_OK);
    app_command_bridge_service(bridge);
    assert(release_effects == 1 && lock_effects == 1 && ordinary_effects == 1);
    allow_completion = true;
    for (unsigned i = 0; i < APP_COMMAND_CAPACITY + 2U; ++i) app_command_bridge_service(bridge);
    assert(ordinary_effects == 1 && release_effects == 1 && lock_effects == 1);
    free(bridge); free(context);
    puts("PASS actual protocol + bridge: reserved lock/release admission, blocked result, stale deadline, unavailable ordinary provider");
}
