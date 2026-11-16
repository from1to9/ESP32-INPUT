// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control_protocol.h"
#include "control_registry.h"
#include "control_json.h"

static cp_context_t *context;
static cp_http_response_t response;
static cp_device_state_t device;
static unsigned entropy, dispatched, cancelled;
static uint64_t now = 100;
static char token[65], csrf[33], session[33], boot[33], prefix[17], auth[80];
static bool random_source(void *user, uint8_t *out, size_t size)
{ (void)user; while (size--) *out++ = (uint8_t)++entropy; return true; }
static bool available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{ (void)user; (void)op; (void)reason; return true; }
static cp_error_t dispatch(void *user, const cp_command_t *command, cp_result_t *result)
{ (void)user; (void)command; ++dispatched; *result=(cp_result_t){.state=CP_RESULT_ACCEPTED,.uncertain_index=-1}; return CP_OK; }
static void safety(void *user, cp_safety_reason_t why, const cp_identity_t *identity)
{ (void)user; (void)why; (void)identity; ++cancelled; }
static cp_error_t read_state(void *user, const cp_identity_t *identity, const char *resource, char *out, size_t cap, size_t *length)
{ (void)user; (void)identity; (void)resource; *length=(size_t)snprintf(out,cap,"{\"network_control\":\"open\"}"); return CP_OK; }
static void field(const char *json,const char *key,char *out,size_t size)
{
    cj_document_t *document=malloc(sizeof(*document)); assert(document);
    assert(cj_parse(document,json,strlen(json)));
    const char *text=cj_string(document,cj_get(document,0,key)); assert(text && strlen(text)<size);
    strcpy(out,text); free(document);
}
static cp_http_security_t security(bool browser)
{
    snprintf(auth,sizeof(auth),"Bearer %s",token);
    return (cp_http_security_t){.host="192.0.2.1",.origin=browser?"http://192.0.2.1":NULL,.authorization=auth,.csrf=csrf,.open_request="1"};
}
static cp_error_t request(const char *method,const char *path,const char *body,cp_http_security_t *sec)
{ return cp_http(context,method,path,sec,body,body?strlen(body):0,now,&response); }
static void open_session(bool browser)
{
    cp_http_security_t sec=security(browser); sec.authorization=NULL;
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_OK && !response.set_cookie[0]);
    field(response.body,"session_token",token,sizeof(token)); field(response.body,"csrf",csrf,sizeof(csrf));
    field(response.body,"session_id",session,sizeof(session)); field(response.body,"boot_id",boot,sizeof(boot));
    field(response.body,"request_prefix",prefix,sizeof(prefix));
}
static void initialize(cp_access_policy_t policy)
{
    free(context);context=NULL; now=100;dispatched=cancelled=0;
    cp_callbacks_t callbacks={.random=random_source,.available=available,.dispatch=dispatch,.safety=safety,.read=read_state};
    void *storage=calloc(1,cp_context_size());assert(storage);
    assert(cp_initialize_policy(storage,cp_context_size(),&callbacks,policy,&context)==CP_OK);
    device=(cp_device_state_t){.management_generation=1,.input_epoch=2,.transport_generation=3,.ui_revision=4,.state_sequence=5,.allowance_generation=1,.input_ready=true,.remote_allowed=true,.target_id="target",.mode="mouse"};
    cp_set_device_state(context,&device,now);
    cp_set_transport_capabilities(context,true,false);
}
static cp_error_t command(const char *op,unsigned seq,bool browser)
{
    char body[1024];
    int n=snprintf(body,sizeof(body),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"500\",\"op\":\"%s\",\"args\":{},\"epoch\":\"2\",\"transport_generation\":\"3\",\"target_id\":\"target\",\"ui_revision\":\"4\",\"management_generation\":\"1\",\"resource_revision\":\"0\"}",boot,session,prefix,seq,seq,op);
    assert(n>0 && (size_t)n<sizeof(body)); cp_http_security_t sec=security(browser);
    return request("POST","/api/v1/commands",body,&sec);
}
int main(void)
{
    initialize(CP_POLICY_ENROLLED_TLS);
    assert(cp_http_configure(context,"192.0.2.1","http://192.0.2.1")==CP_INVALID);
    assert(cp_http_configure(context,"192.0.2.1","https://192.0.2.1")==CP_OK);
    cp_http_security_t sec=security(false);
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_FORBIDDEN);

    initialize(CP_POLICY_PASSPORT_OPEN_HTTP);
    assert(cp_http_configure(context,"192.0.2.1","https://192.0.2.1")==CP_INVALID);
    assert(cp_http_configure(context,"192.0.2.1","http://192.0.2.1")==CP_OK);
    sec=security(true);sec.open_request=NULL;
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_FORBIDDEN);
    sec.open_request="1";sec.origin="http://evil.test";
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_FORBIDDEN);
    sec.origin="http://192.0.2.1";sec.host="evil.test";
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_FORBIDDEN);
    sec=security(true);
    assert(request("POST","/api/v1/open-session","{\"policy\":\"tls\"}",&sec)==CP_INVALID);
    assert(request("GET","/api/v1/login-challenge",NULL,&sec)==CP_FORBIDDEN);
    open_session(true); sec=security(true);
    assert(request("GET","/api/v1/capabilities",NULL,&sec)==CP_OK && strstr(response.body,"open_network"));
    assert(request("GET","/api/v1/config",NULL,&sec)==CP_FORBIDDEN);
    assert(request("GET","/api/v1/state",NULL,&sec)==CP_OK);
    assert(command("ui.close",1,true)==CP_FORBIDDEN && !dispatched);
    device.locked=true; cp_set_device_state(context,&device,now);
    assert(command("input.cancel",2,true)==CP_LOCKED && !dispatched);
    device.locked=false; ++device.allowance_generation; cp_set_device_state(context,&device,now);
    sec=security(true); sec.csrf=NULL;
    assert(request("POST","/api/v1/ws-ticket",NULL,&sec)==CP_UNAUTHENTICATED);
    sec=security(true); assert(request("POST","/api/v1/ws-ticket",NULL,&sec)==CP_OK);
    char ticket[65];field(response.body,"ticket",ticket,sizeof(ticket));
    cp_identity_t channel; sec.authorization=NULL;sec.csrf=NULL;
    assert(cp_transport_upgrade(context,&sec,ticket,now,&channel)==CP_OK && channel.browser);
    cp_identity_t duplicate;assert(cp_transport_upgrade(context,&sec,ticket,now,&duplicate)==CP_UNAUTHENTICATED);
    const char forbidden_read[] = "{\"v\":1,\"resource\":\"/api/v1/config\"}";
    assert(cp_transport_frame(context,&channel,forbidden_read,strlen(forbidden_read),now,&response)==CP_FORBIDDEN);
    assert(cp_transport_close(context,&channel,now)==CP_OK);
    sec=security(true); assert(request("GET","/api/v1/state",NULL,&sec)==CP_OK); // WS loss keeps read access.
    char first_auth[80];strcpy(first_auth,auth);
    open_session(false); sec=security(false);
    assert(cp_transport_upgrade(context,&sec,NULL,now,&channel)==CP_OK && !channel.browser);
    sec.authorization=NULL;
    assert(request("POST","/api/v1/open-session","{}",&sec)==CP_BUSY); // No eviction.
    sec=security(true);sec.authorization=first_auth;
    assert(request("GET","/api/v1/state",NULL,&sec)==CP_OK);
    assert(command("ui.close",1,false)==CP_FORBIDDEN);
    char first_key[24], result_path[64];field(response.body,"command_key",first_key,sizeof(first_key));
    for(unsigned n=2;n<=CP_MAX_RESULTS+1;++n) assert(command("ui.close",n,false)==CP_FORBIDDEN);
    snprintf(result_path,sizeof(result_path),"/api/v1/requests/%s",first_key);
    sec=security(false);assert(request("GET",result_path,NULL,&sec)==CP_RESULT_UNKNOWN);
    now+=60001;cp_tick(context,now);
    assert(request("GET","/api/v1/state",NULL,&sec)==CP_UNAUTHENTICATED);
    assert(cancelled);
    for(size_t i=0;i<cp_operation_count();++i) {
        const cp_operation_info_t *op=cp_operation_at(i);
        if(!strncmp(op->name,"ui.",3) || !strncmp(op->name,"config.",7) || !strncmp(op->name,"audio.",6)) assert(!cp_policy_allows(CP_POLICY_PASSPORT_OPEN_HTTP,op->id));
    }
    free(context);
    puts("PASS: explicit open policy, unchanged TLS default, Host/Origin/header, bearer+CSRF, narrow commands/reads, Lock, single-use browser WS ticket, native WS and bounded sessions");
}
