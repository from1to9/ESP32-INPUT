// SPDX-License-Identifier: Apache-2.0
/* The SDK lock is the only stub in the production server's result-key allocator. */
static unsigned s_lock;
static uint64_t s_command_key;
#define portENTER_CRITICAL(lock) do { assert(!*(lock)); ++*(lock); } while (0)
#define portEXIT_CRITICAL(lock) do { assert(*(lock) == 1); --*(lock); } while (0)
#include "server_command_key.inc"
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL

typedef struct dual_transport dual_transport_t;
typedef struct {
    dual_transport_t *owner;
    cp_context_t *context;
    cp_http_response_t *response;
    cp_http_security_t security;
    unsigned seed, sequence;
    char cookie[256], csrf[33], auth[72], session[33], boot[33], prefix[17];
} listener_t;

struct dual_transport {
    fixture_t input;
    app_command_bridge_t *bridge;
    listener_t listeners[2], *interleave;
    bool locked;
    uint64_t bridge_now;
    cp_error_t expected_execute_error;
    uint64_t keys[32], interleaved_key;
    unsigned executed;
    cp_command_t oldest;
};

static void dual_field(const char *body, const char *name, char *out, size_t capacity)
{
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "\"%s\":\"", name);
    const char *begin = strstr(body, prefix);
    assert(begin);
    begin += strlen(prefix);
    const char *end = strchr(begin, '"');
    assert(end && (size_t)(end - begin) < capacity);
    memcpy(out, begin, (size_t)(end - begin));
    out[end - begin] = 0;
}

static void dual_publish(dual_transport_t *test)
{
    fixture_t *f = &test->input;
    app_remote_input_view_t view;
    app_remote_input_snapshot(f->remote, &view);
    cp_device_state_t state = {.input_epoch = f->engine.state.epoch,
        .transport_generation = f->state.transport_generation,
        .management_generation = f->state.management_generation, .ui_revision = f->state.ui_revision,
        .state_sequence = f->state.state_sequence, .input_ready = true, .remote_allowed = true, .allowance_generation = 1,
        .remote_owner = f->engine.state.owner == INPUT_REMOTE, .release_pending = f->engine.state.release_pending};
    memcpy(state.owner_session, view.owner_session, sizeof(state.owner_session));
    strcpy(state.target_id, f->state.target_id);
    strcpy(state.mode, "mouse");
    for (unsigned i = 0; i < 2; ++i) {
        if (test->listeners[i].context) cp_set_device_state(test->listeners[i].context, &state, f->now);
    }
}

static uint64_t dual_send(listener_t *listener, const char *operation, const char *args)
{
    fixture_t *f = &listener->owner->input;
    char body[1024], key[32];
    unsigned sequence = ++listener->sequence;
    int n = snprintf(body, sizeof(body),
        "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":%u,"
        "\"deadline_ms\":\"%" PRIu64 "\",\"epoch\":\"%" PRIu64 "\",\"transport_generation\":\"%" PRIu64 "\","
        "\"ui_revision\":\"%" PRIu64 "\",\"target_id\":\"%s\",\"op\":\"%s\",\"args\":%s}",
        listener->boot, listener->session, listener->prefix, sequence, sequence,
        f->now + 100, f->engine.state.epoch, f->state.transport_generation, f->state.ui_revision,
        f->state.target_id, operation, args);
    assert(n > 0 && (size_t)n < sizeof(body));
    cp_error_t error = cp_http(listener->context, "POST", "/api/v1/commands", &listener->security,
        body, (size_t)n, f->now, listener->response);
    if (error != CP_OK) fprintf(stderr, "dual %s %s #%u => %s\n",
        listener->security.tls ? "TLS" : "HTTP", operation, sequence, listener->response->body);
    assert(error == CP_OK);
    assert(listener->response->status == 202);
    dual_field(listener->response->body, "command_key", key, sizeof(key));
    return strtoull(key, NULL, 10);
}

static bool dual_random(void *user, uint8_t *out, size_t size)
{
    listener_t *listener = user;
    for (size_t i = 0; i < size; ++i) out[i] = (uint8_t)listener->seed++;
    return true;
}

static cp_error_t dual_dispatch(void *user, const cp_command_t *command, cp_result_t *result)
{
    dual_transport_t *test = ((listener_t *)user)->owner;
    if (test->interleave) {
        // Simulate the second HTTP task acquiring the bridge lock while the
        // first task is between protocol validation and queue admission.
        listener_t *other = test->interleave;
        test->interleave = NULL;
        test->interleaved_key = dual_send(other, "control.release", "{}");
    }
    return app_command_bridge_submit(test->bridge, command, result);
}

static bool dual_available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{ (void)user; (void)op; reason[0] = 0; return true; }

static void dual_open(dual_transport_t *test, unsigned slot, unsigned seed)
{
    listener_t *listener = &test->listeners[slot];
    bool tls = slot == 0;
    const char *origin = tls ? "https://device.test" : "http://device.test";
    *listener = (listener_t){.owner = test, .seed = seed,
        .security = {.tls = tls, .host = "device.test", .origin = tls ? NULL : origin}};
    void *memory = calloc(1, cp_context_size());
    listener->response = calloc(1, sizeof(*listener->response));
    assert(memory && listener->response);
    cp_callbacks_t callbacks = {.user = listener, .random = dual_random, .available = dual_available,
        .dispatch = dual_dispatch, .next_command_key = next_command_key};
    assert(cp_initialize_policy(memory, cp_context_size(), &callbacks,
        tls ? CP_POLICY_ENROLLED_TLS : CP_POLICY_ENROLLED_HTTP, &listener->context) == CP_OK);
    assert(cp_http_configure(listener->context, "device.test", origin) == CP_OK);
    cp_principal_t principal = {.enabled = true, .native_only = tls, .scopes = CP_SCOPE_ALL, .id = {1}};
    uint8_t secret[32];
    memset(secret, 0x44, sizeof(secret));
    cp_sha256(secret, sizeof(secret), principal.credential_sha256);
    assert(cp_load_principal(listener->context, &principal) == CP_OK);
    dual_publish(test);
    if (!tls) {
        assert(cp_http(listener->context, "GET", "/api/v1/login-challenge", &listener->security,
            NULL, 0, test->input.now, listener->response) == CP_OK);
        strcpy(listener->cookie, listener->response->set_cookie);
        dual_field(listener->response->body, "csrf", listener->csrf, sizeof(listener->csrf));
        listener->security.cookie = listener->cookie;
        listener->security.csrf = listener->csrf;
    }
    const char body[] = "{\"client_id\":\"01000000000000000000000000000000\","
        "\"credential\":\"4444444444444444444444444444444444444444444444444444444444444444\"}";
    assert(cp_http(listener->context, "POST", "/api/v1/sessions", &listener->security,
        body, sizeof(body) - 1, test->input.now, listener->response) == CP_OK);
    if (tls) {
        memcpy(listener->auth, "Bearer ", 7);
        dual_field(listener->response->body, "session_token", listener->auth + 7, sizeof(listener->auth) - 7);
        listener->security.authorization = listener->auth;
    } else {
        strcpy(listener->cookie, listener->response->set_cookie);
        dual_field(listener->response->body, "csrf", listener->csrf, sizeof(listener->csrf));
    }
    dual_field(listener->response->body, "session_id", listener->session, sizeof(listener->session));
    dual_field(listener->response->body, "boot_id", listener->boot, sizeof(listener->boot));
    dual_field(listener->response->body, "request_prefix", listener->prefix, sizeof(listener->prefix));
}

static void dual_lock(void *user) { dual_transport_t *d = user; assert(!d->locked); d->locked = true; }
static void dual_unlock(void *user) { dual_transport_t *d = user; assert(d->locked); d->locked = false; }
static uint64_t dual_clock(void *user)
{
    dual_transport_t *test = user;
    return test->bridge_now ? test->bridge_now : test->input.now;
}
static void dual_safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{ (void)user; (void)reason; (void)identity; }
static cp_error_t dual_read(void *user, const cp_identity_t *identity, const char *resource, char *out,
                            size_t size, size_t *length)
{ (void)user; (void)identity; (void)resource; (void)out; (void)size; (void)length; return CP_UNAVAILABLE; }
static cp_error_t dual_execute(void *user, const cp_command_t *command, cp_result_t *result)
{
    dual_transport_t *test = user;
    assert(!test->locked && command->admission_order);
    assert(test->executed < sizeof(test->keys) / sizeof(test->keys[0]));
    test->keys[test->executed++] = command->command_key;
    if (test->executed == 1) test->oldest = *command;
    cp_error_t error = app_remote_input_execute(test->input.remote, command, test->input.now, result);
    assert(error == test->expected_execute_error);
    return error;
}
static unsigned dual_deliver(dual_transport_t *test, const cp_result_t *result)
{
    unsigned matches = 0;
    for (unsigned i = 0; i < 2; ++i) {
        cp_context_t *context = test->listeners[i].context;
        cp_error_t error = cp_complete(context, result->command_key, result, test->input.now);
        assert(error == CP_OK || error == CP_RESULT_UNKNOWN);
        matches += error == CP_OK;
    }
    assert(matches <= 1);
    return matches;
}

static bool dual_complete(void *user, const cp_result_t *result)
{
    (void)dual_deliver(user, result);
    return true;
}

static void dual_close(listener_t *listener)
{
    cp_shutdown(listener->context, listener->owner->input.now);
    free(listener->context);
    free(listener->response);
    listener->context = NULL;
}

static void dual_listener_order_and_restart(void)
{
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        dual_transport_t test = {0};
        setup(&test.input, INPUT_MOUSE);
        void *bridge = calloc(1, app_command_bridge_bytes());
        assert(bridge);
        app_command_bridge_port_t port = {.user = &test, .lock = dual_lock, .unlock = dual_unlock,
            .now_ms = dual_clock, .available = dual_available, .execute = dual_execute,
            .safety = dual_safety, .read = dual_read, .complete = dual_complete};
        assert(app_command_bridge_init(bridge, app_command_bridge_bytes(), &port, &test.bridge) == CP_OK);
        app_command_bridge_refresh(test.bridge);
        unsigned tls_seed = reverse ? 0x10 : 0x70;
        unsigned http_seed = reverse ? 0x70 : 0x10;
        dual_open(&test, 0, tls_seed);
        dual_open(&test, 1, http_seed);
        listener_t *tls = &test.listeners[0], *http = &test.listeners[1];
        uint64_t old_tls = dual_send(tls, "control.release", "{}");
        --tls->sequence;
        assert(dual_send(tls, "control.release", "{}") == old_tls);
        app_command_bridge_service(test.bridge);
        uint64_t old_http = dual_send(http, "control.release", "{}");
        app_command_bridge_service(test.bridge);
        assert(old_http == old_tls + 1 && test.keys[0] == old_tls && test.keys[1] == old_http);

        test.interleave = http;
        uint64_t overtaken = dual_send(tls, "control.release", "{}");
        assert(test.interleaved_key == overtaken + 1); // Later protocol key reached the bridge first.
        dual_publish(&test);
        assert(cp_http(http->context, "GET", "/api/v1/state", &http->security,
            NULL, 0, test.input.now, http->response) == CP_OK);
        char args[80];
        snprintf(args, sizeof(args), "{\"state_sequence\":\"%" PRIu64 "\"}", test.input.state.state_sequence);
        uint64_t acquired = dual_send(http, "control.acquire", args);
        app_command_bridge_service(test.bridge);
        assert(test.executed == 3 && test.keys[2] == test.interleaved_key);
        app_command_bridge_service(test.bridge);
        assert(test.executed == 5 && test.keys[3] == overtaken && test.keys[4] == acquired);
        step(&test.input);
        step(&test.input);
        dual_publish(&test);
        cp_result_t result = last_result(&test.input, acquired);
        assert(result.state == CP_RESULT_APPLIED && dual_deliver(&test, &result) == 1);
        assert(hid_engine_enqueue_mouse_buttons(&test.input.engine, test.input.engine.state.epoch,
            INPUT_REMOTE, 1) == HID_ENGINE_ENQUEUE_OK);
        step(&test.input);
        assert(test.input.engine.mouse_buttons == 1);
        dual_publish(&test);
        uint64_t release = dual_send(http, "control.release", "{}");
        app_command_bridge_service(test.bridge);
        step(&test.input);
        step(&test.input);
        assert(!test.input.engine.mouse_buttons && !test.input.engine.state.release_pending);
        assert(test.input.engine.state.owner == INPUT_LOCAL);
        result = last_result(&test.input, release);
        assert(result.state == CP_RESULT_APPLIED && dual_deliver(&test, &result) == 1);

        unsigned slot = reverse ? 0 : 1;
        listener_t *restarted = &test.listeners[slot];
        uint64_t old_key = reverse ? old_tls : old_http;
        dual_close(restarted);
        // Even a repeated random seed cannot recycle a result key.
        dual_open(&test, slot, reverse ? tls_seed : http_seed);
        uint64_t replacement = dual_send(restarted, "control.release", "{}");
        assert(replacement > release && replacement != old_key);
        cp_result_t old_completion = {.state = CP_RESULT_APPLIED, .command_key = old_key, .uncertain_index = -1};
        assert(cp_complete(restarted->context, old_key, &old_completion, test.input.now) == CP_RESULT_UNKNOWN);
        app_command_bridge_service(test.bridge);
        assert(test.keys[test.executed - 1] == replacement);

        // Exercise the exact old envelope after evicting its bounded result.
        for (unsigned i = 0; i < 17; ++i) {
            test.input.now += 1000;
            (void)dual_send(restarted, "control.release", "{}");
            app_command_bridge_service(test.bridge);
        }
        assert(app_remote_input_execute(test.input.remote, &test.oldest, test.input.now, &result) == CP_STALE);
        assert(!strcmp(result.reason, "old_command_key"));
        dual_close(http);
        dual_close(tls);
        free(bridge);
        finish(&test.input);
    }
    puts("PASS: TLS/HTTP dual listeners, reordered admission, neutral release, context replacement and old completion isolation");
}

static void browser_motion_expiry_stage(void)
{
    dual_transport_t test = {0};
    fixture_t *input = &test.input;
    setup(input, INPUT_MOUSE);
    void *bridge = calloc(1, app_command_bridge_bytes());
    assert(bridge);
    app_command_bridge_port_t port = {.user = &test, .lock = dual_lock, .unlock = dual_unlock,
        .now_ms = dual_clock, .available = dual_available, .execute = dual_execute,
        .safety = dual_safety, .read = dual_read, .complete = dual_complete};
    assert(app_command_bridge_init(bridge, app_command_bridge_bytes(), &port, &test.bridge) == CP_OK);
    app_command_bridge_refresh(test.bridge);
    dual_open(&test, 0, 0x10);
    dual_open(&test, 1, 0x90);
    listener_t *http = &test.listeners[1];
    assert(cp_http(http->context, "GET", "/api/v1/state?view=protocol", &http->security,
        NULL, 0, input->now, http->response) == CP_OK);
    char args[80];
    snprintf(args, sizeof(args), "{\"state_sequence\":\"%" PRIu64 "\"}", input->state.state_sequence);
    const uint64_t acquired = dual_send(http, "control.acquire", args);
    app_command_bridge_service(test.bridge);
    step(input);
    step(input);
    dual_publish(&test);
    cp_result_t result = last_result(input, acquired);
    assert(result.state == CP_RESULT_APPLIED && dual_deliver(&test, &result) == 1);
    assert(cp_http(http->context, "GET", "/api/v1/state?view=protocol", &http->security,
        NULL, 0, input->now, http->response) == CP_OK);
    char lease[33];
    dual_field(http->response->body, "lease_id", lease, sizeof(lease));

    cp_set_transport_capabilities(http->context, true, false);
    cp_set_motion_terminal_available(http->context, true);
    assert(cp_http(http->context, "POST", "/api/v1/ws-ticket", &http->security,
        NULL, 0, input->now, http->response) == CP_OK);
    char ticket[65];
    dual_field(http->response->body, "ticket", ticket, sizeof(ticket));
    cp_identity_t identity;
    assert(cp_transport_upgrade(http->context, &http->security, ticket, input->now, &identity) == CP_OK);
    cp_motion_terminal_t motion = {.enabled = true};
    const size_t before = input->reports_count;
    const char *stages[] = {"protocol", "submit", "dequeue", "input"};
    for (size_t i = 0; i <= sizeof(stages) / sizeof(stages[0]); ++i) {
        const bool succeeds = i == sizeof(stages) / sizeof(stages[0]);
        const uint64_t deadline = input->now + 100;
        if (i == 0) input->now = deadline;
        test.bridge_now = i == 1 ? deadline : 0;
        char body[1024], key[32], observed[CP_REASON_CAPACITY], expected[CP_REASON_CAPACITY];
        const unsigned sequence = ++http->sequence;
        int n = snprintf(body, sizeof(body),
            "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":%u,"
            "\"deadline_ms\":\"%" PRIu64 "\",\"epoch\":\"%" PRIu64 "\",\"transport_generation\":\"%" PRIu64 "\","
            "\"target_id\":\"%s\",\"lease_id\":\"%s\",\"op\":\"mouse.move_relative\",\"args\":{\"dx\":5,\"dy\":0}}",
            http->boot, http->session, http->prefix, sequence, sequence, deadline, input->engine.state.epoch,
            input->state.transport_generation, input->state.target_id, lease);
        assert(n > 0 && (size_t)n < sizeof(body));
        const cp_error_t admission = cp_transport_frame_motion(http->context, &identity, body,
            (size_t)n, input->now, http->response, &motion);
        assert(admission == (i < 2 ? CP_EXPIRED : CP_OK));
        dual_field(http->response->body, "command_key", key, sizeof(key));
        const uint64_t command_key = strtoull(key, NULL, 10);
        assert(motion.accepted_key == (i < 2 ? 0 : command_key));
        if (i >= 2) {
            if (!succeeds) input->now = deadline;
            test.bridge_now = i == 3 ? deadline - 1 : 0;
            test.expected_execute_error = i == 3 ? CP_EXPIRED : CP_OK;
            app_command_bridge_service(test.bridge);
            if (succeeds) step(input);
            if (succeeds) {
                result = last_result(input, command_key);
                assert(result.state == CP_RESULT_SENT && dual_deliver(&test, &result) == 1);
            }
            assert(cp_transport_motion_result(http->context, &identity, command_key, input->now,
                http->response) == CP_OK);
            char terminal_key[32];
            dual_field(http->response->body, "command_key", terminal_key, sizeof(terminal_key));
            assert(!strcmp(key, terminal_key));
            assert(strstr(http->response->body, "\"motion_terminal\":true"));
        }
        dual_field(http->response->body, "reason", observed, sizeof(observed));
        if (succeeds) assert(!strcmp(observed, "transport_accepted_reports"));
        else {
            snprintf(expected, sizeof(expected), "%s now=%" PRIu64 " deadline=%" PRIu64,
                     stages[i], deadline, deadline);
            assert(!strcmp(observed, expected) && http->response->status == 400);
            assert(strstr(http->response->body, "\"state\":\"rejected\",\"error\":\"expired\""));
        }
        assert(input->reports_count == before + (size_t)succeeds);
        char path[80], retained[CP_REASON_CAPACITY], next[32];
        snprintf(path, sizeof(path), "/api/v1/requests/%s", key);
        assert(cp_http(http->context, "GET", path, &http->security, NULL, 0,
            input->now, http->response) == (succeeds ? CP_OK : CP_EXPIRED));
        dual_field(http->response->body, "reason", retained, sizeof(retained));
        assert(!strcmp(observed, retained));
        snprintf(next, sizeof(next), "\"next_seq\":%u,", sequence + 1);
        assert(strstr(http->response->body, next));
        test.bridge_now = 0;
    }
    dual_close(http);
    dual_close(&test.listeners[0]);
    free(bridge);
    finish(input);
    puts("PASS: browser move expiry receipts at protocol, bridge submit, dequeue and input checks");
}
