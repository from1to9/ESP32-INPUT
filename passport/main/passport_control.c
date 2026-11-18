// SPDX-License-Identifier: Apache-2.0
#include "passport_control.h"
#include "app_remote_input.h"
#include "control_json.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_log.h"
#endif

_Static_assert(CP_MOTION_WINDOW_CONFIG_MAX <= CP_MAX_ACTIVE_REQUESTS &&
               CP_MOTION_WINDOW_CONFIG_MAX <= APP_REMOTE_INPUT_JOBS,
               "Passport motion window exceeds the configured request or HID job capacity");

static cp_context_t *protocol;
// Written only by synchronous dispatch and consumed by that same owner's transport call.
static uint64_t new_move_key;
static app_remote_input_t *remote;
static hid_engine_t *engine;
static interaction_t interaction;
static passport_control_observation_t observed;
static uint64_t now, sequence, ui_revision, allowance;
static cp_device_state_t previous;
static cp_result_t completions[APP_REMOTE_INPUT_JOBS];
static size_t completion_count;
static bool known;
static const passport_control_provider_t *provider;
static void early_failure(const char *stage, cp_error_t error)
{
#if defined(ESP_PLATFORM) && CONFIG_PASSPORT_DIAGNOSTICS
    static unsigned logged;
    if (logged < 16) {
        ++logged;
        ESP_LOGW("passport_control_diag", "stage=%s error=%s", stage, cp_error_name(error));
    }
#else
    (void)stage; (void)error;
#endif
}
void passport_control_set_provider(const passport_control_provider_t *p) { provider=p; }

static bool observe(void *user, app_remote_input_state_t *out)
{
    (void)user;
    *out = (app_remote_input_state_t){.transport_generation=observed.transport_generation,
        .ui_revision=ui_revision,.draft_revision=1,.selection_revision=1,
        .state_sequence=sequence,.management_generation=1,.remote_allowed=known,
        .local_busy=observed.local_busy,.screen_awake=!engine->state.locked,
        .transition_busy=observed.transition,.shortcut_modifier=1};
    memcpy(out->target_id,observed.target,sizeof(out->target_id));
    return known;
}
static void changed(void *user, unsigned flags)
{
    (void)user; (void)flags;
    ++sequence; ++ui_revision;
}
static bool complete(void *user, const cp_result_t *result)
{
    (void)user;
    if (completion_count == APP_REMOTE_INPUT_JOBS) return false;
    completions[completion_count++] = *result;
    return true;
}
static bool delegate_available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    (void)user;
    if (op == CP_OP_MODE_SELECT) return true;
    snprintf(reason,CP_REASON_CAPACITY,"provider_unavailable"); return false;
}
static cp_error_t delegate(void *user, const cp_command_t *command, cp_result_t *result)
{
    (void)user;
    char mode[12];
    if (command->operation != CP_OP_MODE_SELECT) return CP_UNAVAILABLE;
    if (!cp_command_string(command,"mode",mode,sizeof(mode))) return CP_INVALID;
    if (strcmp(mode,"mouse") && strcmp(mode,"keyboard")) return CP_UNAVAILABLE;
    if (!hid_engine_set_mode(engine,!strcmp(mode,"mouse") ? INPUT_MOUSE : INPUT_KEYBOARD)) return CP_UNAVAILABLE;
    changed(NULL,APP_REMOTE_CHANGED_UI | APP_REMOTE_CHANGED_INPUT);
    *result=(cp_result_t){.command_key=command->command_key,.state=CP_RESULT_APPLIED,.uncertain_index=-1};
    return CP_OK;
}
static bool available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    (void)user;
    if (op == CP_OP_STATE_GET || op == CP_OP_CAPABILITIES_GET || op == CP_OP_OPERATIONS_LIST) return true;
    if (app_remote_input_owns(op) && cp_policy_allows(CP_POLICY_PASSPORT_OPEN_HTTP,op))
        return app_remote_input_available(remote,op,reason);
    if (provider && cp_policy_allows(CP_POLICY_PASSPORT_OPEN_HTTP,op)) return provider->available(op,reason);
    snprintf(reason,CP_REASON_CAPACITY,"provider_unavailable"); return false;
}
static cp_error_t dispatch(void *user, const cp_command_t *command, cp_result_t *result)
{
    (void)user;
    if (command->operation == CP_OP_CONTROL_ACQUIRE) {
        char producer[24];
        if (cp_command_string(command,"producer",producer,sizeof(producer)) && strcmp(producer,"script")) return CP_UNAVAILABLE;
    }
    if (app_remote_input_owns(command->operation)) {
        cp_error_t rc=app_remote_input_execute(remote,command,now,result);
        if (command->operation==CP_OP_MOUSE_MOVE_RELATIVE && rc==CP_OK && result->state==CP_RESULT_ACCEPTED)
            new_move_key=command->command_key;
        return rc;
    }
    return provider ? provider->dispatch(command,now,result) : CP_UNAVAILABLE;
}
static void safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    (void)user;
    app_remote_input_revoke(remote,reason,identity,now);
}
static cp_error_t read_resource(void *user, const cp_identity_t *identity, const char *resource,
                              char *out, size_t capacity, size_t *length)
{
    (void)user; (void)identity;
    if (strcmp(resource,"/api/v1/state")) return provider ? provider->read(resource,out,capacity,length) : CP_UNAVAILABLE;
    passport_control_view_t view; passport_control_view(&view);
    char sta[100], ap[100];
    if (!cj_escape(observed.sta_ip,sta,sizeof(sta)) || !cj_escape(observed.ap_ip,ap,sizeof(ap))) return CP_INTERNAL;
    int n=snprintf(out,capacity,"{\"network_control\":\"open\",\"controller\":\"%s\",\"sta_ip\":%s,\"ap_ip\":%s}",view.controller,sta,ap);
    if (n<0 || (size_t)n>=capacity) return CP_TOO_LARGE;
    *length=(size_t)n; return CP_OK;
}
bool passport_control_init(hid_engine_t *input, cp_random_fn random)
{
    if (!input || !random || protocol || remote) return false;
    // IDF calloc guarantees only four-byte alignment; both owners publish a
    // stricter contract. Their initializers clear their own complete objects.
    void *p=aligned_alloc(cp_context_alignment(),cp_context_size());
    void *r=aligned_alloc(app_remote_input_alignment(),app_remote_input_bytes());
#if defined(ESP_PLATFORM) && CONFIG_PASSPORT_DIAGNOSTICS
    ESP_LOGI("passport_control_diag", "init protocol=%u bytes=%zu align=%zu remote=%u bytes=%zu align=%zu",
             p!=NULL, cp_context_size(), cp_context_alignment(), r!=NULL,
             app_remote_input_bytes(), app_remote_input_alignment());
#endif
    if (!p || !r) { free(p); free(r); early_failure("allocation",CP_UNAVAILABLE); return false; }
    provider=NULL; engine=input; sequence=ui_revision=allowance=1; known=false; completion_count=0;
    memset(&observed,0,sizeof(observed)); memset(&previous,0,sizeof(previous));
    interaction_init(&interaction);
    app_remote_input_port_t port={.engine=engine,.interaction=&interaction,.observe=observe,
        .changed=changed,.complete=complete,.delegate_available=delegate_available,.delegate=delegate};
    cp_callbacks_t callbacks={.random=random,.available=available,.dispatch=dispatch,.safety=safety,.read=read_resource};
    cp_error_t rc=app_remote_input_init(r,app_remote_input_bytes(),&port,&remote);
    if (rc!=CP_OK) early_failure("remote_init",rc);
    else {
        rc=cp_initialize_policy(p,cp_context_size(),&callbacks,CP_POLICY_PASSPORT_OPEN_HTTP,&protocol);
        if (rc!=CP_OK) early_failure("protocol_init",rc);
    }
    if (rc!=CP_OK) {
        free(p); free(r); protocol=NULL; remote=NULL; engine=NULL; return false;
    }
    cp_set_transport_capabilities(protocol,true,false);
    cp_set_motion_window_max(protocol,CP_MOTION_WINDOW_CONFIG_MAX);
    cp_set_motion_window_ordered(protocol,true);
    return true;
}
void passport_control_deinit(void)
{
    if (protocol) cp_shutdown(protocol,now);
    provider=NULL; free(protocol); free(remote); protocol=NULL; remote=NULL; engine=NULL; known=false;
}
static void publish(void)
{
    app_remote_input_view_t view={0}; app_remote_input_snapshot(remote,&view);
    cp_device_state_t state={.management_generation=1,.input_epoch=engine->state.epoch,
        .transport_generation=observed.transport_generation,.ui_revision=ui_revision,
        .state_sequence=sequence,.allowance_generation=allowance,.locked=engine->state.locked,
        .input_ready=engine->ready && !observed.transition,.release_pending=engine->state.release_pending,
        .local_busy=observed.local_busy,.remote_allowed=known,.remote_owner=view.active || view.pending};
    memcpy(state.target_id,observed.target,sizeof(state.target_id));
    strcpy(state.mode,engine->state.mode==INPUT_MOUSE ? "mouse" : "keyboard");
    memcpy(state.owner_session,view.owner_session,sizeof(state.owner_session));
    // Only meaningful state changes invalidate a freshly observed acquire/renew.
    state.state_sequence=previous.state_sequence;
    if (memcmp(&state,&previous,sizeof(state))) ++sequence;
    state.state_sequence=sequence; previous=state;
    cp_set_device_state(protocol,&state,now);
}
void passport_control_observe(const passport_control_observation_t *state, uint64_t time)
{
    if (!protocol || !state) return;
    now=time;
    if (known && (observed.network_generation!=state->network_generation ||
        strcmp(observed.sta_ip,state->sta_ip) || strcmp(observed.ap_ip,state->ap_ip)))
        passport_control_network_reset(now);
    if ((observed.local_busy && !state->local_busy) || (previous.locked && !engine->state.locked)) ++allowance;
    observed=*state; known=true;
    passport_control_service(now);
}
void passport_control_service(uint64_t time)
{
    if (!protocol) return;
    now=time; app_remote_input_service(remote,now); publish();
    // Completion is outside dispatch; publish the resulting owner/epoch first.
    for (size_t i=0;i<completion_count;++i) (void)cp_complete(protocol,completions[i].command_key,&completions[i],now);
    completion_count=0;
    cp_result_t result;
    if (provider && provider->poll(now,&result)) (void)cp_complete(protocol,result.command_key,&result,now);
}
uint64_t passport_control_execute(passport_control_request_t *request, uint64_t time)
{
    now=time; new_move_key=0;
    memset(&request->response,0,sizeof(request->response));
    request->error=CP_UNAVAILABLE;
    if (!protocol) { early_failure("execute_without_protocol",request->error); return 0; }
    passport_control_service(now);
    cp_http_security_t sec={.host=request->host,.origin=request->origin[0] ? request->origin : NULL,
        .authorization=request->authorization[0] ? request->authorization : NULL,
        .csrf=request->csrf[0] ? request->csrf : NULL,.open_request=request->open[0] ? request->open : NULL,
        .expiry_state=request->expiry_state};
    if (request->kind==PC_HTTP || request->kind==PC_WS_OPEN) {
        char expected[48]; snprintf(expected,sizeof(expected),"http://%s",request->host);
        request->error=cp_http_configure(protocol,request->host,expected);
        if (request->error!=CP_OK) { early_failure("http_configure",request->error); return 0; }
    }
    switch (request->kind) {
    case PC_HTTP: request->error=cp_http(protocol,request->method,request->resource,&sec,request->body,request->length,now,&request->response); break;
    case PC_WS_OPEN: request->error=cp_transport_upgrade(protocol,&sec,request->ticket[0] ? request->ticket : NULL,now,&request->identity); break;
    case PC_WS_FRAME: request->error=cp_transport_frame_motion(protocol,&request->identity,request->body,request->length,now,
        &request->response,&request->motion); break;
    case PC_WS_TERMINAL: request->error=cp_transport_motion_result(protocol,&request->identity,
        request->motion.accepted_key,now,&request->response); break;
    case PC_WS_HEARTBEAT: request->error=cp_transport_heartbeat(protocol,&request->identity,now); break;
    }
    passport_control_service(now);
    bool inline_move=request->kind==PC_HTTP || (request->kind==PC_WS_FRAME && request->identity.browser &&
        (request->motion.mode==CP_TERMINAL_DISABLED || request->motion.ordered));
    uint64_t key=inline_move && request->error==CP_OK && request->response.status==202 ? new_move_key : 0;
    new_move_key=0;
    return key;
}
bool passport_control_terminal_ready(const cp_identity_t *identity, uint64_t key, uint64_t time)
{
    // Missing records also wake the HTTP task so the channel reports loss.
    return cp_transport_motion_result(protocol,identity,key,time,NULL)!=CP_BUSY;
}
void passport_control_finish_move(passport_control_request_t *request, uint64_t key, uint64_t time)
{
    // A retry or neutral cleanup can still be pending after one HID step.
    // Leave the original acceptance intact until the record is terminal.
    if (!cp_result_is_terminal(protocol,key)) return;
    if (request->kind==PC_WS_FRAME && request->motion.mode==CP_TERMINAL_MOVE_WINDOW) {
        request->error=cp_transport_motion_inline_result(protocol,&request->identity,key,time,&request->response);
        request->motion.accepted_key=0;
        if (request->error!=CP_OK) request->response.status=0;
        return;
    }
    snprintf(request->resource,sizeof(request->resource),"/api/v1/requests/%" PRIu64,key);
    if (request->kind==PC_WS_FRAME) {
        // Read under the authenticated channel identity that submitted the move.
        request->length=(size_t)snprintf(request->body,sizeof(request->body),
            "{\"v\":1,\"resource\":\"%s\"}",request->resource);
    } else {
        strcpy(request->method,"GET");
        request->length=0;
    }
    (void)passport_control_execute(request,time);
}
void passport_control_close(const cp_identity_t *identity, uint64_t time)
{ if (protocol) { now=time; (void)cp_transport_close(protocol,identity,now); passport_control_service(now); } }
void passport_control_network_reset(uint64_t time)
{ if (protocol) { now=time; cp_shutdown(protocol,now); passport_control_service(now); } }
void passport_control_local(uint64_t time)
{
    if (!protocol) return;
    now=time; observed.local_busy=true; cp_physical_override(protocol,engine->state.locked,now);
    app_remote_input_revoke(remote,CP_SAFETY_LOCAL_OVERRIDE,NULL,now); passport_control_service(now);
}
bool passport_control_expire_movement(uint64_t time)
{ return app_remote_input_expire_movement_report(remote,time); }
bool passport_control_report_authorized(uint64_t time)
{ return engine && (engine->state.owner!=INPUT_REMOTE || app_remote_input_report_authorized(remote,time)); }
void passport_control_view(passport_control_view_t *out)
{
    *out=(passport_control_view_t){.available=protocol!=NULL};
    if (!remote) return;
    app_remote_input_view_t view; app_remote_input_snapshot(remote,&view);
    out->active=view.active || view.pending;
    if (out->active) for (size_t i=0;i<4;++i) snprintf(out->controller+i*2,3,"%02x",view.owner_session[i]);
}
