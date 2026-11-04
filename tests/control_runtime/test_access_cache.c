// SPDX-License-Identifier: Apache-2.0
/* Actual server cache gates, without an HTTP task tick between policy changes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "control_test_sdk.h"
#include "control_registry.h"
#include "../../firmware/main/app_control_server.h"
typedef struct { cp_callbacks_t callbacks; bool stop_requested; } server_t;
static server_t server, *s_server = &server;
static portMUX_TYPE s_lock;
static cp_session_view_t s_sessions[CP_MAX_SESSIONS];
static size_t s_session_count;
static cp_principal_t principal;
static bool present = true;
static unsigned critical;
static uint64_t clock_ms = 100;
void test_enter(void) { (void)&s_lock; assert(!critical); ++critical; }
void test_leave(void) { assert(critical); --critical; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static bool current(void *user, const uint8_t id[16], cp_principal_t *out)
{ (void)user; assert(!critical); *out = principal; return present && !memcmp(id, principal.id, 16); }
#include "access_cache_fixture.inc"
int main(void)
{
    server.callbacks.principal = current;
    principal = (cp_principal_t){.enabled = true, .scopes = CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL};
    memset(principal.id, 0x11, 16); memset(principal.credential_sha256, 0x22, 32);
    s_session_count = 1;
    cp_session_view_t *v = &s_sessions[0]; v->authenticated = true; v->last_authenticated_ms = clock_ms;
    v->identity.scopes = principal.scopes; v->identity.browser = true;
    memcpy(v->identity.principal_id, principal.id, 16); memset(v->identity.session_id, 0x33, 16);
    memcpy(v->credential_sha256, principal.credential_sha256, 32);
    assert(app_control_server_identity_alive(&v->identity) && app_control_server_client_connected(principal.id));
    assert(app_control_server_session_alive(v->identity.session_id, principal.id));
    principal.enabled = false;
    assert(!app_control_server_identity_alive(&v->identity) && !app_control_server_client_connected(principal.id));
    principal.enabled = true; principal.scopes = CP_SCOPE_VIEW;
    assert(!app_control_server_session_alive(v->identity.session_id, principal.id));
    principal.scopes = v->identity.scopes; principal.credential_sha256[0] ^= 1;
    assert(!app_control_server_identity_alive(&v->identity));
    principal.credential_sha256[0] ^= 1; present = false;
    assert(!app_control_server_client_connected(principal.id)); present = true;
    clock_ms += 4999; assert(app_control_server_client_connected(principal.id));
    ++clock_ms; assert(!app_control_server_client_connected(principal.id));
    clock_ms = 100; server.stop_requested = true;
    assert(!app_control_server_identity_alive(&v->identity)); server.stop_requested = false;
    s_server = NULL; assert(!app_control_server_client_connected(principal.id));
    assert(!critical);
    puts("PASS: actual cached session authority rejects changed credentials/scopes/revoke/expiry before the next protocol tick");
    return 0;
}
