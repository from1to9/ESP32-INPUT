// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control_protocol.h"
#include "control_registry.h"
#include "control_json.h"
#include "control_media.h"
#include "control_registry_internal.h"

static unsigned random_seed, dispatches, safety_input, safety_session, safety_lock;
static cp_context_t *context;
static cp_http_response_t response;
static cp_command_t last_command;
static cp_device_state_t device;
static uint64_t clock_ms;
static char credential[65], client_id[33], session_id[33], token[65], prefix[17], boot[33],
    csrf[33];
static unsigned seq;
static bool is_available = true, redaction_failure;
static unsigned dispatch_override;
static bool random_source(void *u, uint8_t *out, size_t length)
{
    (void)u;
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)(++random_seed);
    }
    return true;
}
static bool available(void *u, cp_operation_id_t id, char reason[CP_REASON_CAPACITY])
{
    (void)u;
    (void)id;
    (void)reason;
    return is_available;
}
static cp_error_t dispatch(void *u, const cp_command_t *cmd, cp_result_t *out)
{
    (void)u;
    ++dispatches;
    last_command = *cmd;
    *out = (cp_result_t){
        .state = CP_RESULT_ACCEPTED, .error = CP_OK, .job_id = 123, .uncertain_index = -1};
    if (dispatch_override == 1) {
        memset(out->reason, 'x', sizeof(out->reason));
    }
    if (dispatch_override == 2) {
        out->state = CP_RESULT_FAILED;
        out->error = CP_UNAVAILABLE;
    }
    return CP_OK;
}
static void safety(void *u, cp_safety_reason_t why, const cp_identity_t *i)
{
    (void)u;
    (void)i;
    if (why == CP_SAFETY_SESSION_LOST || why == CP_SAFETY_REVOKED) {
        ++safety_session;
    } else if (why == CP_SAFETY_LOCKED) {
        ++safety_lock;
    } else {
        ++safety_input;
    }
}
static cp_error_t read_resource(void *u, const cp_identity_t *who, const char *resource, char *out,
                                size_t capacity, size_t *length)
{
    (void)u;
    (void)who;
    (void)resource;
    int n = snprintf(out, capacity,
                     redaction_failure ? "{\"password\":\"synthetic\"}"
                                       : "{\"wifi_backend\":\"available\",\"measurement\":null}");
    *length = (size_t)n;
    return CP_OK;
}
static void hex(const uint8_t *bytes, size_t size, char *out)
{
    for (size_t i = 0; i < size; ++i) {
        sprintf(out + i * 2, "%02x", bytes[i]);
    }
}
static void field(const char *json, const char *key, char *out, size_t capacity)
{
    cj_document_t *d = malloc(sizeof(*d));
    assert(cj_parse(d, json, strlen(json)));
    const char *v = cj_string(d, cj_get(d, 0, key));
    assert(v && strlen(v) < capacity);
    strcpy(out, v);
    free(d);
}
static cp_http_security_t native_security(void)
{
    static char auth[80];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    return (cp_http_security_t){.tls = true, .host = "device.test", .authorization = auth};
}
static cp_error_t request(const char *method, const char *path, const char *body)
{
    cp_http_security_t security = native_security();
    return cp_http(context, method, path, &security, body, body ? strlen(body) : 0, clock_ms,
                   &response);
}
static void reset(cp_scope_mask_t scopes, bool browser)
{
    free(context);
    context = NULL;
    random_seed = dispatches = safety_input = safety_session = safety_lock = 0;
    is_available = true;
    dispatch_override = 0;
    redaction_failure = false;
    clock_ms = 100;
    seq = 0;
    cp_callbacks_t callbacks = {.random = random_source,
                                .available = available,
                                .dispatch = dispatch,
                                .safety = safety,
                                .read = read_resource};
    void *storage = calloc(1, cp_context_size());
    assert(storage);
    assert(cp_initialize(storage, cp_context_size(), &callbacks, &context) == CP_OK);
    assert(cp_http_configure(context, "device.test", "https://device.test") == CP_OK);
    uint8_t secret[32];
    for (unsigned i = 0; i < 32; ++i) {
        secret[i] = (uint8_t)(i + 11);
    }
    hex(secret, 32, credential);
    cp_principal_t principal = {.scopes = scopes, .native_only = !browser, .enabled = true};
    memset(principal.id, 0x41, 16);
    hex(principal.id, 16, client_id);
    cp_sha256(secret, 32, principal.credential_sha256);
    assert(cp_load_principal(context, &principal) == CP_OK);
    device = (cp_device_state_t){.management_generation = 1,
                                 .input_epoch = 2,
                                 .transport_generation = 3,
                                 .ui_revision = 4,
                                 .state_sequence = 5,
                                 .allowance_generation = 1,
                                 .input_ready = true,
                                 .remote_allowed = true};
    strcpy(device.target_id, "test-target");
    strcpy(device.mode, "mouse");
    cp_set_device_state(context, &device, clock_ms);
    memset(token, 0, sizeof(token));
    memset(csrf, 0, sizeof(csrf));
}
static void login_native(void)
{
    char body[180];
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"credential\":\"%s\"}", client_id,
             credential);
    cp_http_security_t security = {.tls = true, .host = "device.test"};
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms,
                   &response) == CP_OK);
    field(response.body, "session_id", session_id, sizeof(session_id));
    field(response.body, "session_token", token, sizeof(token));
    field(response.body, "request_prefix", prefix, sizeof(prefix));
    field(response.body, "boot_id", boot, sizeof(boot));
    field(response.body, "csrf", csrf, sizeof(csrf));
    assert(request("GET", "/api/v1/state", NULL) == CP_OK);
}
static char envelope[CP_MAX_FRAME_BYTES + 1];
static const char *command(const char *op, const char *args, bool input, bool lease,
                           unsigned number)
{
    char lease_id[33] = "";
    if (lease) {
        assert(request("GET", "/api/v1/state", NULL) == CP_OK);
        cj_document_t *d = malloc(sizeof(*d));
        assert(cj_parse(d, response.body, response.length));
        const char *id = cj_string(d, cj_get(d, cj_get(d, 0, "protocol"), "lease_id"));
        assert(id);
        strcpy(lease_id, id);
        free(d);
    }
    int n = snprintf(envelope, sizeof(envelope),
                     "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%"
                     "016llx\",\"seq\":%u,\"deadline_ms\":\"%llu\",\"op\":\"%s\",\"args\":%s",
                     boot, session_id, prefix, (unsigned long long)number, number,
                     (unsigned long long)(clock_ms + 100), op, args);
    assert(n > 0);
    size_t used = (size_t)n;
    if (input) {
        n = snprintf(envelope + used, sizeof(envelope) - used,
                     ",\"epoch\":\"%llu\",\"transport_generation\":\"%llu\",\"target_id\":\"test-"
                     "target\",\"ui_revision\":\"%llu\"",
                     (unsigned long long)device.input_epoch,
                     (unsigned long long)device.transport_generation,
                     (unsigned long long)device.ui_revision);
        used += (size_t)n;
        if (lease) {
            n = snprintf(envelope + used, sizeof(envelope) - used, ",\"lease_id\":\"%s\"",
                         lease_id);
            used += (size_t)n;
        }
    } else {
        n = snprintf(envelope + used, sizeof(envelope) - used,
                     ",\"management_generation\":\"1\",\"resource_revision\":\"0\"");
        used += (size_t)n;
    }
    assert(used + 2 < sizeof(envelope));
    strcpy(envelope + used, "}");
    return envelope;
}
static cp_error_t send(const char *op, const char *args, bool input, bool lease)
{
    return request("POST", "/api/v1/commands", command(op, args, input, lease, ++seq));
}
static void acquire(void)
{
    assert(send("control.acquire", "{\"state_sequence\":\"5\"}", true, false) == CP_OK);
    assert(response.status == 202 && last_command.lease_expires_ms == clock_ms + 3000);
    assert(last_command.operation == CP_OP_CONTROL_ACQUIRE);
    device.remote_owner = true;
    memcpy(device.owner_session, last_command.identity.session_id, 16);
    ++device.input_epoch;
    cp_set_device_state(context, &device, clock_ms);
    cp_result_t result = {.state = CP_RESULT_APPLIED, .error = CP_OK, .uncertain_index = -1};
    assert(cp_complete(context, last_command.command_key, &result, clock_ms) == CP_OK);
}
static void test_json_sha(void)
{
    uint8_t hash[32];
    char text[65];
    cp_sha256("abc", 3, hash);
    hex(hash, 32, text);
    assert(strcmp(text, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    cp_sha256("", 0, hash);
    hex(hash, 32, text);
    assert(strcmp(text, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    cj_document_t *d = malloc(sizeof(*d));
    const char *bad[] = {"{\"a\":1,\"a\":2}",
                         "{\"a\":1,\"\\u0061\":2}",
                         "[01]",
                         "1e999",
                         "1e-999",
                         "{\"a\":\"\\u0000\"}",
                         "{\"a\":\"\\ud800\"}",
                         "{\"a\":NaN}",
                         "{\"a\":true,}",
                         "[] trailing"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!cj_parse(d, bad[i], strlen(bad[i])));
    }
    const char *good = "{\"z\":1.0,\"a\":\"\\uD83D\\uDE00\"}";
    assert(cj_parse(d, good, strlen(good)));
    char canonical[100];
    assert(cj_canonical(d, 0, canonical, sizeof(canonical), NULL));
    assert(strncmp(canonical, "{\"a\":", 5) == 0);
    assert(strstr(canonical, "\"z\":1}"));
    free(d);
}
static void test_auth_and_scopes(void)
{
    reset(CP_SCOPE_ALL, false);
    cp_http_security_t security = {.tls = false, .host = "device.test"};
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, &response) ==
           CP_FORBIDDEN);
    security.tls = true;
    security.host = "device.test.attacker";
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, &response) ==
           CP_FORBIDDEN);
    login_native();
    security = native_security();
    security.origin = "https://evil.test";
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, &response) ==
           CP_FORBIDDEN);
    assert(request("GET", "/api/v1/state", NULL) == CP_OK);
    reset(CP_SCOPE_VIEW, false);
    login_native();
    assert(send("wifi.scan.start", "{}", false, false) == CP_FORBIDDEN && dispatches == 0);
    assert(strstr(response.body, "\"next_seq\":2"));
    assert(send("config.stage", "{\"patch\":{\"display.brightness_percent\":50}}", false, false) ==
           CP_FORBIDDEN);
    assert(dispatches == 0);
}
static void test_replay_and_typed_bounds(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    acquire();
    assert(send("mouse.move_relative", "{\"dx\":127,\"dy\":-127}", true, true) == CP_OK);
    unsigned before = dispatches;
    char original[CP_MAX_FRAME_BYTES + 1];
    strcpy(original, envelope);
    assert(request("POST", "/api/v1/commands", original) == CP_OK && dispatches == before);
    assert(send("mouse.move_relative", "{\"dx\":128,\"dy\":0}", true, true) == CP_INVALID &&
           dispatches == before);
    assert(strstr(response.body, "\"next_seq\":4"));
    const char *gap = command("mouse.click", "{\"button\":\"left\"}", true, true, seq + 2);
    assert(request("POST", "/api/v1/commands", gap) == CP_SEQUENCE_GAP && dispatches == before);
    strcpy(envelope, original);
    char *number = strstr(envelope, "\"dx\":127");
    assert(number);
    number[5] = '2';
    assert(request("POST", "/api/v1/commands", envelope) == CP_REQUEST_CONFLICT);
    assert(dispatches == before && safety_input > 0);
    assert(request("GET", "/api/v1/state", NULL) ==
           CP_OK); /* Auth remains useful for scoped reads. */
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(send("config.stage", "{\"patch\":{\"display.brightness_percent\":55}}", false, false) ==
           CP_INVALID);
    assert(send("config.stage",
                "{\"patch\":{\"selection.cell_degrees\":6,\"selection.hysteresis_degrees\":3}}",
                false, false) == CP_INVALID);
    assert(send("device.unlock", "{}", false, false) == CP_INVALID);
    assert(send("audio.playback.pause", "{}", false, false) == CP_NOT_IMPLEMENTED);
    assert(send("wifi.join", "{\"ssid\":\"synthetic\",\"password\":\"short\",\"remember\":true}",
                false, false) == CP_INVALID);
}
static void test_takeover_domains_and_lease(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(
        send(
            "audio.capture.start",
            "{\"source\":\"local_microphone\",\"sample_profile\":\"pcm16_mono_16k\",\"consent_id\":"
            "\"11111111111111111111111111111111\",\"duration_seconds\":10,\"byte_limit\":10000}",
            false, false) == CP_OK);
    uint64_t media = last_command.command_key;
    acquire();
    assert(send("mouse.click", "{\"button\":\"left\"}", true, true) == CP_OK);
    uint64_t click = last_command.command_key;
    cp_physical_override(context, false, clock_ms);
    assert(safety_input > 0 && safety_session == 0 && safety_lock == 0);
    assert(request("GET", "/api/v1/state", NULL) == CP_OK);
    cp_result_t result = {.state = CP_RESULT_COMPLETED, .error = CP_OK, .uncertain_index = -1};
    assert(cp_complete(context, media, &result, clock_ms) == CP_OK);
    result.state = CP_RESULT_SENT;
    assert(cp_complete(context, click, &result, clock_ms) == CP_STALE);
    assert(send("wifi.scan.start", "{}", false, false) ==
           CP_OK); /* Management survives physical takeover. */
    assert(send("control.acquire", "{\"state_sequence\":\"5\"}", true, false) == CP_FORBIDDEN);
    ++device.allowance_generation;
    device.remote_owner = false;
    cp_set_device_state(context, &device, clock_ms);
    acquire();
    clock_ms += 3001;
    cp_tick(context, clock_ms);
    assert(safety_session == 0 && request("GET", "/api/v1/state", NULL) == CP_OK);
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(send("wifi.scan.start", "{}", false, false) == CP_OK);
    media = last_command.command_key;
    cp_physical_override(context, true, clock_ms);
    assert(safety_lock > 0 && safety_session == 0);
    assert(request("GET", "/api/v1/state", NULL) == CP_OK);
    result.state = CP_RESULT_APPLIED;
    assert(cp_complete(context, media, &result, clock_ms) == CP_STALE);
}
static void test_browser_csrf_and_private_read(void)
{
    reset(CP_SCOPE_ALL, true);
    cp_http_security_t security = {
        .tls = true, .host = "device.test", .origin = "https://device.test"};
    assert(cp_http(context, "GET", "/api/v1/login-challenge", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    char challenge[33], cookie[256];
    field(response.body, "csrf", challenge, sizeof(challenge));
    strcpy(cookie, response.set_cookie);
    char body[180];
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"credential\":\"%s\"}", client_id,
             credential);
    security.cookie = cookie;
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms,
                   &response) == CP_FORBIDDEN);
    security.csrf = challenge;
    assert(cp_http(context, "POST", "/api/v1/sessions", &security, body, strlen(body), clock_ms,
                   &response) == CP_OK);
    assert(!strstr(response.body, "session_token"));
    strcpy(cookie, response.set_cookie);
    field(response.body, "csrf", csrf, sizeof(csrf));
    field(response.body, "session_id", session_id, sizeof(session_id));
    field(response.body, "request_prefix", prefix, sizeof(prefix));
    field(response.body, "boot_id", boot, sizeof(boot));
    security.cookie = cookie;
    security.csrf = NULL;
    const char *cmd = command("wifi.scan.start", "{}", false, false, 1);
    assert(cp_http(context, "POST", "/api/v1/commands", &security, cmd, strlen(cmd), clock_ms,
                   &response) == CP_UNAUTHENTICATED);
    security.csrf = csrf;
    assert(cp_http(context, "POST", "/api/v1/commands", &security, cmd, strlen(cmd), clock_ms,
                   &response) == CP_OK);
    cp_set_transport_capabilities(context, true, true);
    assert(cp_http(context, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms, &response) ==
           CP_OK);
    char ticket[65];
    field(response.body, "ticket", ticket, sizeof(ticket));
    cp_identity_t upgraded;
    cp_http_security_t other = security;
    assert(cp_http(context, "GET", "/api/v1/login-challenge", &other, NULL, 0, clock_ms,
                   &response) == CP_OK);
    char challenge2[33], cookie2[256];
    field(response.body, "csrf", challenge2, sizeof(challenge2));
    strcpy(cookie2, response.set_cookie);
    other.cookie = cookie2;
    other.csrf = challenge2;
    assert(cp_http(context, "POST", "/api/v1/sessions", &other, body, strlen(body), clock_ms,
                   &response) == CP_OK);
    strcpy(cookie2, response.set_cookie);
    assert(cp_transport_upgrade(context, &other, ticket, clock_ms, &upgraded) ==
           CP_UNAUTHENTICATED);
    cp_http_security_t wrong = security;
    wrong.origin = "https://evil.test";
    assert(cp_transport_upgrade(context, &wrong, ticket, clock_ms, &upgraded) == CP_FORBIDDEN);
    wrong = security;
    wrong.origin = NULL;
    assert(cp_transport_upgrade(context, &wrong, ticket, clock_ms, &upgraded) == CP_FORBIDDEN);
    assert(cp_transport_upgrade(context, &security, ticket, clock_ms, &upgraded) == CP_OK);
    assert(cp_transport_upgrade(context, &security, ticket, clock_ms, &upgraded) ==
           CP_UNAUTHENTICATED);
    redaction_failure = true;
    assert(cp_http(context, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, &response) ==
           CP_INTERNAL);
    assert(!strstr(response.body, "synthetic"));
}

typedef struct {
    char credential[65], client_id[33], session_id[33], token[65], prefix[17], boot[33], csrf[33];
    unsigned sequence;
} client_state_t;
static client_state_t save_client(void)
{
    client_state_t c;
    strcpy(c.credential, credential);
    strcpy(c.client_id, client_id);
    strcpy(c.session_id, session_id);
    strcpy(c.token, token);
    strcpy(c.prefix, prefix);
    strcpy(c.boot, boot);
    strcpy(c.csrf, csrf);
    c.sequence = seq;
    return c;
}
static void restore_client(const client_state_t *c)
{
    strcpy(credential, c->credential);
    strcpy(client_id, c->client_id);
    strcpy(session_id, c->session_id);
    strcpy(token, c->token);
    strcpy(prefix, c->prefix);
    strcpy(boot, c->boot);
    strcpy(csrf, c->csrf);
    seq = c->sequence;
}
static void second_client(cp_scope_mask_t scopes)
{
    uint8_t secret[32];
    for (unsigned i = 0; i < 32; ++i) {
        secret[i] = (uint8_t)(i + 77);
    }
    cp_principal_t p = {.scopes = scopes, .native_only = true, .enabled = true};
    memset(p.id, 0x42, 16);
    cp_sha256(secret, 32, p.credential_sha256);
    assert(cp_load_principal(context, &p) == CP_OK);
    hex(secret, 32, credential);
    hex(p.id, 16, client_id);
    seq = 0;
    login_native();
}
static void test_competing_sessions_and_owned_rollback(void)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        reset(CP_SCOPE_ALL, false);
        login_native();
        acquire();
        client_state_t owner = save_client();
        second_client(scenario == 3 ? CP_SCOPE_VIEW : CP_SCOPE_ALL);
        unsigned before_dispatch = dispatches, before_safety = safety_input;
        cp_error_t result;
        if (scenario == 1) {
            result = send("control.acquire", "{\"state_sequence\":\"invalid\"}", true, false);
        } else if (scenario == 2) {
            command("control.acquire", "{\"state_sequence\":\"5\"}", true, false, ++seq);
            clock_ms += 101;
            result = request("POST", "/api/v1/commands", envelope);
        } else {
            result = send("control.acquire", "{\"state_sequence\":\"5\"}", true, false);
        }
        assert(result == (scenario == 0   ? CP_OWNER_BUSY
                          : scenario == 1 ? CP_INVALID
                          : scenario == 2 ? CP_EXPIRED
                                          : CP_FORBIDDEN));
        assert(dispatches == before_dispatch && safety_input == before_safety);
        restore_client(&owner);
        assert(request("GET", "/api/v1/state", NULL) == CP_OK);
        assert(strstr(response.body, "\"lease_active\":true"));
        assert(send("mouse.click", "{\"button\":\"left\"}", true, true) == CP_OK);
        assert(dispatches == before_dispatch + 1 && safety_input == before_safety);
    }
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(send("control.acquire", "{\"state_sequence\":\"5\"}", true, false) == CP_OK);
    uint64_t old = last_command.command_key;
    cp_result_t failed = {.state = CP_RESULT_FAILED, .error = CP_INTERNAL, .uncertain_index = -1};
    assert(cp_complete(context, old, &failed, clock_ms) == CP_OK);
    assert(send("control.acquire", "{\"state_sequence\":\"5\"}", true, false) == CP_OK);
    assert(last_command.command_key != old);
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(send("control.acquire", "{\"state_sequence\":\"5\"}", true, false) == CP_OK);
    old = last_command.command_key;
    cp_physical_override(context, false, clock_ms);
    ++device.input_epoch;
    ++device.allowance_generation;
    cp_set_device_state(context, &device, clock_ms);
    second_client(CP_SCOPE_ALL);
    acquire();
    unsigned before = safety_input;
    assert(cp_complete(context, old, &failed, clock_ms) == CP_OK);
    assert(safety_input == before && request("GET", "/api/v1/state", NULL) == CP_OK);
    assert(strstr(response.body, "\"lease_active\":true"));
    assert(send("mouse.click", "{\"button\":\"left\"}", true, true) == CP_OK);
}
static void test_result_budget_and_neutral_lane(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(
        send(
            "audio.capture.start",
            "{\"source\":\"local_microphone\",\"sample_profile\":\"pcm16_mono_16k\",\"consent_id\":"
            "\"11111111111111111111111111111111\",\"duration_seconds\":10,\"byte_limit\":10000}",
            false, false) == CP_OK);
    char pinned[CP_MAX_FRAME_BYTES + 1];
    strcpy(pinned, envelope);
    cp_result_t done = {.state = CP_RESULT_APPLIED, .error = CP_OK, .uncertain_index = -1};
    for (unsigned i = 0; i < 70; ++i) {
        assert(send("wifi.scan.start", "{}", false, false) == CP_OK);
        assert(cp_complete(context, last_command.command_key, &done, clock_ms) == CP_OK);
    }
    unsigned before = dispatches;
    assert(request("POST", "/api/v1/commands", pinned) == CP_OK && dispatches == before);
    const char *old = command("wifi.scan.start", "{}", false, false, 2);
    assert(request("POST", "/api/v1/commands", old) == CP_RESULT_UNKNOWN && dispatches == before);
    reset(CP_SCOPE_ALL, false);
    login_native();
    for (unsigned i = 0; i < CP_MAX_ACTIVE_REQUESTS; ++i) {
        assert(send("wifi.scan.start", "{}", false, false) == CP_OK);
    }
    assert(send("wifi.scan.start", "{}", false, false) == CP_BUSY);
    command("input.release_all", "{}", true, false, ++seq);
    char neutral[CP_MAX_FRAME_BYTES + 1];
    strcpy(neutral, envelope);
    clock_ms += 101;
    is_available = false;
    assert(request("POST", "/api/v1/commands", neutral) ==
           CP_OK); /* Neutral release outlives the ordinary deadline/full lane. */
    assert(last_command.operation == CP_OP_INPUT_RELEASE_ALL);
    assert(strstr(response.body, "\"started_prefix\":null"));
    reset(CP_SCOPE_ALL, false);
    login_native();
    clock_ms += 28800001U;
    assert(request("GET", "/api/v1/state", NULL) == CP_UNAUTHENTICATED && safety_session > 0);
    reset(CP_SCOPE_ALL, false);
    login_native();
    clock_ms = 99;
    assert(request("GET", "/api/v1/state", NULL) == CP_UNAVAILABLE);
}

static void test_exact_decimal_configuration(void)
{
    cj_document_t *d = malloc(sizeof(*d));
    char json[400], canonical[400];
    size_t length;
    const char *valid[] = {"0.25", "4.01", "25e-2", "0.2500", "4.0100", "1e2"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        snprintf(json, sizeof(json), "{\"patch\":{\"pointer.gain_x_hid_per_degree\":%s}}",
                 valid[i]);
        assert(cj_parse(d, json, strlen(json)) &&
               cs_validate(d, 0, cp_argument_schema(CP_OP_CONFIG_STAGE)));
        assert(cj_canonical(d, 0, canonical, sizeof(canonical), &length));
        assert(cj_parse(d, canonical, length) &&
               cs_validate(d, 0, cp_argument_schema(CP_OP_CONFIG_STAGE)));
    }
    const char *invalid[] = {"0.24", "100.01", "4.001", "0.2500000000000000001",
                             "4.01000000000000000001"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        snprintf(json, sizeof(json), "{\"patch\":{\"pointer.gain_x_hid_per_degree\":%s}}",
                 invalid[i]);
        assert(cj_parse(d, json, strlen(json)) &&
               !cs_validate(d, 0, cp_argument_schema(CP_OP_CONFIG_STAGE)));
    }
    int64_t integer;
    assert(cj_parse(d, "2.000000000000000001", 20) && !cj_integer(d, 0, &integer));
    assert(cj_parse(d, "9223372036854775807.0", 21) && cj_integer(d, 0, &integer) &&
           integer == INT64_MAX);
    assert(cj_parse(d, "-9223372036854775808.0", 22) && cj_integer(d, 0, &integer) &&
           integer == INT64_MIN);
    free(d);
}
static void test_explicit_keyboard_submode_transition(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    acquire();
    strcpy(device.mode, "keyboard");
    cp_set_device_state(context, &device, clock_ms);
    assert(send("keyboard.submode.select", "{\"submode\":\"touch\"}", true, true) == CP_OK);
    uint64_t key = last_command.command_key;
    ++device.input_epoch;
    ++device.ui_revision;
    device.release_pending = true;
    cp_set_device_state(context, &device, clock_ms);
    cp_result_t applied = {.state = CP_RESULT_APPLIED, .uncertain_index = -1};
    assert(
        cp_complete(context, key, &applied, clock_ms) ==
        CP_OK); /* Explicit model change, same proven lease/owner; neutral barrier stays visible. */
    device.release_pending = false;
    cp_set_device_state(context, &device, clock_ms);
    assert(request("GET", "/api/v1/state", NULL) == CP_OK &&
           strstr(response.body, "\"lease_active\":true"));
    assert(send("keyboard.submode.select", "{\"submode\":\"motion\"}", true, true) == CP_OK);
    key = last_command.command_key;
    ++device.input_epoch;
    cp_set_device_state(context, &device, clock_ms);
    ++device.input_epoch;
    cp_set_device_state(context, &device, clock_ms);
    assert(cp_complete(context, key, &applied, clock_ms) ==
           CP_STALE); /* An unrelated second epoch cannot reuse the transition exemption. */
    assert(send("keyboard.submode.select", "{\"submode\":\"touch\"}", true, true) == CP_OK);
    key = last_command.command_key;
    ++device.input_epoch;
    strcpy(device.mode, "mouse");
    cp_set_device_state(context, &device, clock_ms);
    assert(cp_complete(context, key, &applied, clock_ms) == CP_STALE);
}
static void test_owner_result_integrity(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    dispatch_override = 2;
    assert(send("wifi.scan.start", "{}", false, false) == CP_UNAVAILABLE);
    assert(strstr(response.body, "\"state\":\"failed\"") &&
           !strstr(response.body, "\"state\":\"rejected\""));
    reset(CP_SCOPE_ALL, false);
    login_native();
    dispatch_override = 1;
    assert(send("wifi.scan.start", "{}", false, false) == CP_INTERNAL);
    assert(strstr(response.body, "cleanup_pending") && strstr(response.body, "outcome_unknown") &&
           strstr(response.body, "\"next_seq\":null"));
    assert(request("GET", "/api/v1/state", NULL) == CP_UNAUTHENTICATED);
    reset(CP_SCOPE_ALL, false);
    login_native();
    assert(send("wifi.scan.start", "{}", false, false) == CP_OK);
    cp_result_t bad = {.state = CP_RESULT_APPLIED, .uncertain_index = -1};
    memset(bad.object_id, 'x', sizeof(bad.object_id));
    assert(cp_complete(context, last_command.command_key, &bad, clock_ms) == CP_INVALID);
}
static void test_transport_and_media(void)
{
    reset(CP_SCOPE_ALL, false);
    login_native();
    cp_http_security_t security = native_security();
    cp_identity_t who;
    security.tls = false;
    assert(cp_transport_authenticate(context, &security, true, clock_ms, &who) == CP_FORBIDDEN);
    security.tls = true;
    assert(cp_transport_authenticate(context, &security, true, clock_ms, &who) == CP_OK);
    cp_set_transport_capabilities(context, true, true);
    assert(request("GET", "/api/v1/capabilities", NULL) == CP_OK &&
           strstr(response.body, "\"websocket\":true"));
    assert(cp_transport_frame(context, &who, "{\"v\":1,\"type\":\"heartbeat\"}", 26, clock_ms,
                              &response) == CP_OK);
    cp_session_view_t sessions[CP_MAX_SESSIONS];
    assert(cp_sessions_snapshot(context, sessions) == 1 && sessions[0].authenticated);
    cp_physical_override(context, false, clock_ms);
    assert(cp_transport_heartbeat(context, &who, clock_ms) == CP_OK);
    cp_identity_t changed = who;
    changed.scopes = CP_SCOPE_VIEW;
    assert(cp_transport_heartbeat(context, &changed, clock_ms) == CP_UNAUTHENTICATED);
    assert(cp_revoke_principal(context, who.principal_id, clock_ms) == CP_OK);
    assert(cp_transport_heartbeat(context, &who, clock_ms) == CP_UNAUTHENTICATED &&
           cp_sessions_snapshot(context, sessions) == 0);
    cp_media_header_t h = {.kind = CP_MEDIA_CAPTURE,
                           .audio_generation = 2,
                           .consumer_generation = 3,
                           .sequence = 1,
                           .sample_rate_hz = 16000,
                           .channels = 1,
                           .frame_ms = 20,
                           .bytes = 640};
    memset(h.job_id, 1, 16);
    uint8_t pcm[640] = {0}, packet[CP_MEDIA_FRAME_BYTES];
    size_t length;
    assert(cp_media_encode(&h, pcm, packet, sizeof(packet), &length) && length == 704);
    cp_media_header_t decoded;
    assert(cp_media_decode(packet, length, &decoded) && decoded.sample_rate_hz == 16000 &&
           decoded.consumer_generation == 3);
    assert(!cp_media_decode(packet, length - 1, &decoded));
    packet[6] = 1;
    assert(!cp_media_decode(packet, length, &decoded));
    packet[6] = 0;
    packet[61] = 15;
    assert(!cp_media_decode(packet, length, &decoded));
    h.sequence = 0;
    assert(!cp_media_encode(&h, pcm, packet, sizeof(packet), &length));
}
static void test_explicit_motion_recenter_schema_lease_and_replay(void)
{
    const cp_operation_info_t *op=cp_operation_find("input.motion.recenter");
    assert(op && op->id==148 && op->input && op->scopes==CP_SCOPE_INPUT_CONTROL &&
        !op->safety && !op->ui_context && !op->read_only);
    reset(CP_SCOPE_ALL,false); login_native(); acquire();
    unsigned before=dispatches;
    assert(send("input.motion.recenter","{\"timeout_ms\":1000}",true,true)==CP_INVALID &&
        dispatches==before);
    assert(send("input.motion.recenter","{}",true,false)==CP_FORBIDDEN && dispatches==before);
    uint64_t epoch=device.input_epoch;
    assert(send("input.motion.recenter","{}",true,true)==CP_OK && dispatches==before+1 &&
        last_command.operation==CP_OP_INPUT_MOTION_RECENTER);
    char duplicate[CP_MAX_FRAME_BYTES+1]; strcpy(duplicate,envelope);
    cp_result_t applied={.state=CP_RESULT_APPLIED,.error=CP_OK,.uncertain_index=-1};
    strcpy(applied.reason,"motion_reference_reset");
    assert(cp_complete(context,last_command.command_key,&applied,clock_ms)==CP_OK &&
        device.input_epoch==epoch);
    assert(request("POST","/api/v1/commands",duplicate)==CP_OK && dispatches==before+1 &&
        strstr(response.body,"motion_reference_reset"));
    const char *expired=command("input.motion.recenter","{}",true,true,++seq);
    char saved[CP_MAX_FRAME_BYTES+1]; strcpy(saved,expired);
    clock_ms+=101;
    assert(request("POST","/api/v1/commands",saved)==CP_EXPIRED && dispatches==before+1);
    reset(CP_SCOPE_VIEW,false); login_native(); before=dispatches;
    assert(send("input.motion.recenter","{}",true,false)==CP_FORBIDDEN && dispatches==before);
}

static void test_context_storage_contract(void)
{
    const size_t alignment = cp_context_alignment(), bytes = cp_context_size();
    assert(alignment >= 8 && !(alignment & (alignment - 1U)));
    uint8_t *raw = malloc(bytes + alignment + 8), *before = malloc(bytes);
    assert(raw && before);
    uintptr_t address = ((uintptr_t)raw + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
    uint8_t *aligned = (uint8_t *)address, *weak = aligned + 4;
    assert((uintptr_t)weak % 4U == 0 && (uintptr_t)weak % alignment != 0);
    memset(weak, 0xa5, bytes);
    memcpy(before, weak, bytes);
    cp_callbacks_t callbacks = {.random = random_source};
    cp_context_t *created = (cp_context_t *)aligned;
    assert(cp_initialize(weak, bytes, &callbacks, &created) == CP_INVALID && !created);
    assert(!memcmp(weak, before, bytes)); /* Reject before touching misaligned storage. */
    created = (cp_context_t *)aligned;
    assert(cp_initialize(aligned, bytes - 1, &callbacks, &created) == CP_INVALID && !created);
    assert(cp_initialize(aligned, bytes, &callbacks, &created) == CP_OK && created == (void *)aligned);
    free(before);
    free(raw);
}
int main(void)
{
    test_context_storage_contract();
    test_explicit_motion_recenter_schema_lease_and_replay();
    test_json_sha();
    test_exact_decimal_configuration();
    test_transport_and_media();
    test_owner_result_integrity();
    test_explicit_keyboard_submode_transition();
    test_auth_and_scopes();
    test_replay_and_typed_bounds();
    test_takeover_domains_and_lease();
    test_browser_csrf_and_private_read();
    test_competing_sessions_and_owned_rollback();
    test_result_budget_and_neutral_lane();
    free(context);
    puts("PASS: production protocol JSON/schema/auth/session/replay/lease/domain separation and "
         "privacy boundaries");
    return 0;
}
