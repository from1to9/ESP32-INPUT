// SPDX-License-Identifier: Apache-2.0
#include "passport_control.h"
#include "control_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hid_engine_t input;
static passport_control_request_t request;
static passport_control_observation_t observation;
static cj_document_t document;
static uint64_t clock_ms;
static unsigned entropy, seq, reports;
static char token[65],session[33],prefix[17],boot[33],csrf[33],lease[33];
static char epoch[24],transport[24],revision[24],sequence[24];
static bool random_bytes(void *u,uint8_t *out,size_t n)
{ (void)u; while(n--) *out++=(uint8_t)++entropy; return true; }
static bool send_report(void *u,uint8_t id,const uint8_t *data,size_t n)
{ (void)u;(void)id;(void)data;(void)n;assert(input.state.release_pending || passport_control_report_authorized(clock_ms));++reports;return true; }
static void tick(void)
{
    clock_ms+=20;hid_engine_observe_transport(&input,clock_ms,false);
    passport_control_observe(&observation,clock_ms);
    hid_engine_step(&input,send_report,NULL);passport_control_service(clock_ms);
}
static void parse(void) { assert(cj_parse(&document,request.response.body,request.response.length)); }
static void field(uint16_t parent,const char *key,char *out,size_t cap)
{ const char *s=cj_string(&document,cj_get(&document,parent,key));assert(s&&strlen(s)<cap);strcpy(out,s); }
static cp_error_t http(const char *method,const char *path,const char *body)
{
    memset(&request,0,sizeof(request));strcpy(request.host,"192.0.2.1");strcpy(request.origin,"http://192.0.2.1");
    snprintf(request.authorization,sizeof(request.authorization),"Bearer %s",token);strcpy(request.csrf,csrf);
    strcpy(request.open,"1");strcpy(request.method,method);strcpy(request.resource,path);
    if(body){strcpy(request.body,body);request.length=strlen(body);}
    passport_control_execute(&request,clock_ms);return request.error;
}
static void state(void)
{
    assert(http("GET","/api/v1/state",NULL)==CP_OK);parse();
    uint16_t p=cj_get(&document,0,"protocol");
    field(p,"epoch",epoch,sizeof(epoch));field(p,"transport_generation",transport,sizeof(transport));
    field(p,"ui_revision",revision,sizeof(revision));field(p,"state_sequence",sequence,sizeof(sequence));
    uint16_t l=cj_get(&document,p,"lease_id");if(l!=CJ_NONE) field(p,"lease_id",lease,sizeof(lease));
}
static cp_error_t command(const char *op,const char *args)
{
    char body[2049];++seq;
    int n=snprintf(body,sizeof(body),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%llu\",\"op\":\"%s\",\"args\":%s,\"epoch\":\"%s\",\"transport_generation\":\"%s\",\"target_id\":\"host\",\"ui_revision\":\"%s\",\"lease_id\":\"%s\"}",boot,session,prefix,seq,seq,(unsigned long long)clock_ms+(!strcmp(op,"mouse.move_relative") || !strcmp(op,"mouse.scroll") ? 200 : 1500),op,args,epoch,transport,revision,lease);
    assert(n>0&&(size_t)n<sizeof(body));return http("POST","/api/v1/commands",body);
}
static void setup(void)
{
    passport_control_deinit();hid_engine_init(&input);hid_engine_set_link(&input,true,true);
    clock_ms=100;seq=reports=0;lease[0]=0;
    observation=(passport_control_observation_t){.transport_generation=7,.target="host",.sta_ip="192.0.2.1"};
    assert(passport_control_init(&input,random_bytes));tick();tick();assert(!input.state.release_pending);
    assert(http("POST","/api/v1/open-session","{}")==CP_OK);parse();
    field(0,"session_token",token,sizeof(token));field(0,"session_id",session,sizeof(session));
    field(0,"request_prefix",prefix,sizeof(prefix));field(0,"boot_id",boot,sizeof(boot));field(0,"csrf",csrf,sizeof(csrf));
    state();
}
static void acquire(void)
{
    state();char args[128];snprintf(args,sizeof(args),"{\"state_sequence\":\"%s\"}",sequence);
    cp_error_t e=command("control.acquire",args);if(e!=CP_OK)fprintf(stderr,"acquire: %s %s\n",cp_error_name(e),request.response.body);
    assert(e==CP_OK);assert(input.state.owner==INPUT_REMOTE&&input.state.release_pending);
    tick();tick();state();assert(passport_control_report_authorized(clock_ms));
}
int main(void)
{
    setup();acquire();
    assert(command("mode.select","{\"mode\":\"keyboard\"}")==CP_OK);tick();tick();state();
    assert(input.state.mode==INPUT_KEYBOARD && passport_control_report_authorized(clock_ms));
    assert(command("keyboard.text.enqueue","{\"text\":\"abcdefghijklmnopqrstuvwxyz012345\"}")==CP_OK);
    char replay[2049],key[24],path[64];strcpy(replay,request.body);parse();field(0,"command_key",key,sizeof(key));
    for(unsigned i=0;i<65;++i)tick();
    snprintf(path,sizeof(path),"/api/v1/requests/%s",key);assert(http("GET",path,NULL)==CP_OK);parse();
    int64_t count;assert(cj_integer(&document,cj_get(&document,0,"completed_pairs"),&count)&&count==32);
    unsigned sent=reports;assert(http("POST","/api/v1/commands",replay)==CP_OK);tick();assert(reports==sent);
    state();assert(command("keyboard.text.enqueue","{\"text\":\"Ab\"}")==CP_OK);parse();field(0,"command_key",key,sizeof(key));
    tick();hid_engine_set_locked(&input,true);tick();tick();
    snprintf(path,sizeof(path),"/api/v1/requests/%s",key);assert(http("GET",path,NULL)==CP_LOCKED);parse();
    assert(!strcmp(cj_string(&document,cj_get(&document,0,"state")),"cancelled"));
    assert(cj_integer(&document,cj_get(&document,0,"started_prefix"),&count)&&count==1);
    assert(cj_integer(&document,cj_get(&document,0,"completed_pairs"),&count)&&count==0);
    assert(cj_integer(&document,cj_get(&document,0,"uncertain_index"),&count)&&count==0);
    setup();acquire();
    assert(command("mouse.move_relative","{\"dx\":12,\"dy\":-5}")==CP_OK);
    unsigned before=reports;tick();assert(reports==before+1);
    assert(command("mouse.drag.begin","{\"button\":\"left\",\"deadman_ms\":500}")==CP_OK);tick();assert(input.mouse_buttons==1);
    clock_ms+=1001;passport_control_service(clock_ms);assert(input.state.owner==INPUT_LOCAL&&input.state.release_pending);
    tick();tick();assert(!input.state.release_pending);
    acquire();passport_control_local(clock_ms);assert(input.state.owner==INPUT_LOCAL&&input.state.release_pending);
    observation.local_busy=true;tick();tick();state();
    assert(command("control.acquire","{\"state_sequence\":\"1\"}")!=CP_OK);
    observation.local_busy=false;tick();acquire();
    hid_engine_set_locked(&input,true);tick();assert(input.state.owner==INPUT_LOCAL);
    assert(command("input.cancel","{}")==CP_LOCKED);
    hid_engine_set_locked(&input,false);tick();tick();acquire();
    observation.transport_generation++;tick();assert(input.state.owner==INPUT_LOCAL&&input.state.release_pending);
    setup();acquire();
    assert(http("POST","/api/v1/ws-ticket",NULL)==CP_OK);parse();char ticket[65];field(0,"ticket",ticket,sizeof(ticket));
    memset(&request,0,sizeof(request));request.kind=PC_WS_OPEN;strcpy(request.host,"192.0.2.1");strcpy(request.origin,"http://192.0.2.1");strcpy(request.ticket,ticket);
    passport_control_execute(&request,clock_ms);assert(request.error==CP_OK);cp_identity_t channel=request.identity;
    passport_control_close(&channel,clock_ms);assert(input.state.owner==INPUT_LOCAL&&input.state.release_pending);
    tick();tick();acquire();passport_control_network_reset(clock_ms);assert(input.state.owner==INPUT_LOCAL);
    assert(http("GET","/api/v1/state",NULL)==CP_UNAUTHENTICATED);
    setup();acquire();strcpy(observation.sta_ip,"192.0.2.2");tick();assert(input.state.owner==INPUT_LOCAL);
    assert(http("GET","/api/v1/state",NULL)==CP_UNAUTHENTICATED);
    setup();acquire();++observation.network_generation;tick();assert(input.state.owner==INPUT_LOCAL);
    assert(http("GET","/api/v1/state",NULL)==CP_UNAUTHENTICATED); // Same IP cannot preserve an old network session.
    setup();acquire();clock_ms+=3001;passport_control_service(clock_ms);assert(input.state.owner==INPUT_LOCAL&&input.state.release_pending);
    passport_control_deinit();puts("PASS: real open protocol/shared owner/HID integration, release barriers, local takeover, Lock, target/network/session/WS loss, lease and drag expiry");
}
