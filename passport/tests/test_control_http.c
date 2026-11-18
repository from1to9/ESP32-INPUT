const char motion_js_fixture[] __asm__("_binary_motion_window_mjs_start")="export class MotionWindow {}";
const char controller_html_fixture[] __asm__("_binary_controller_html_start")="<!doctype html>fixture";
const char controller_css_fixture[] __asm__("_binary_controller_css_start")="body{}";
const char controller_js_fixture[] __asm__("_binary_controller_js_start")="'use strict';";
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include "../main/passport_control_http.c"
#include "control_json.h"
static hid_engine_t engine;
static passport_wifi_link_t network;
static const char *host,*origin,*type,*authorization,*csrf,*open_header,*subprotocol,*input,*local_ip;
static int socket_family=AF_INET;
static bool socket_short,socket_error;
static char output[8192],token[65],csrf_token[33],auth[80],ticket[65],protocol_header[96];
static size_t offset;
static const char *expiry_header="";
static TickType_t ticks=100;
static bool signalled,delay_owner,stale_signal;
static bool abandon_during_random;
static unsigned entropy,registered,sent_frames;
static bool congested;
static bool exercise_moves,skip_hid,fail_hid,timeout_move;
static uint64_t timed_out_key,accepted_moves;
static unsigned failed_reports;
static int last_dx;
// Compile the Passport report callback against a clock and the two NimBLE
// buffer-ownership paths. Advancing the clock during allocation distinguishes
// the service tick from the final handoff to the stack.
static uint8_t keyboard[8],mouse[4];
static uint16_t keyboard_handle=1,mouse_handle=2,connection=9;
struct os_mbuf { const uint8_t *data; size_t length; };
static struct os_mbuf report_buffer;
static unsigned allocation_ms, expired_buffers;
static uint64_t now_ms(void) { return ticks; }
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,size_t length)
{
    assert(!report_buffer.data);
    report_buffer=(struct os_mbuf){data,length};
    ticks+=allocation_ms;
    return &report_buffer;
}
static void os_mbuf_free_chain(struct os_mbuf *b)
{ assert(b==&report_buffer && b->data);b->data=NULL;++expired_buffers; }
static int ble_gatts_notify_custom(uint16_t conn,uint16_t handle,struct os_mbuf *b)
{
    assert(conn==connection && b==&report_buffer && b->data);
    const uint8_t *data=b->data;b->data=NULL;
    if (fail_hid) { ++failed_reports; return -1; }
    if (handle==mouse_handle && b->length>=3 && data[1]) {
        ++accepted_moves; last_dx=(int8_t)data[1];
    }
    return 0;
}
#include "ble_submit.inc"
static void service_hid(void)
{
    hid_engine_observe_transport(&engine,ticks,congested);
    if (!skip_hid) (void)hid_engine_step_with_submit(&engine,send_report,NULL);
    passport_control_service(ticks);
}
static httpd_ws_type_t sent_type;
static size_t sent_length;
static uint8_t wire[4096];
static size_t wire_length,wire_at;
static unsigned wire_reads;
static bool random_bytes(void *u,uint8_t *out,size_t n)
{
    (void)u;
    if(abandon_during_random) {
        assert(mailbox==RUNNING);mailbox=ABANDONED;abandon_during_random=false;
        assert(!begin()); // The timed-out HTTP task cannot reuse in-flight data.
    }
    while(n--)*out++=(uint8_t)++entropy;return true;
}
void explicit_bzero(void *p,size_t n) { volatile uint8_t *b=p;while(n--)*b++=0; }
void passport_wifi_link_snapshot(passport_wifi_link_t *out) { *out=network; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &signalled; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int xSemaphoreGive(SemaphoreHandle_t s) { assert(s==&signalled);signalled=true;return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t wait)
{
    assert(s==&signalled);
    if(signalled) { signalled=false;return pdTRUE; }
    if(!wait)return 0;
    if(stale_signal) { stale_signal=false;++ticks;return pdTRUE; }
    if(delay_owner) { ticks+=wait;return 0; }
    ++ticks;
    uint64_t key=passport_control_http_service(ticks);
    if(exercise_moves)service_hid();
    if(key) {
        assert(mailbox==RUNNING && !signalled && !begin());
        if(timeout_move){timed_out_key=key;ticks+=wait;return 0;}
        passport_control_http_finish_move(key,ticks);
    }
    assert(signalled);signalled=false;return pdTRUE;
}
int httpd_req_to_sockfd(httpd_req_t *req) { (void)req;return 9; }
int setup_getsockname(int fd,struct sockaddr *address,socklen_t *length)
{
 assert(fd==9&&*length==sizeof(struct sockaddr_storage));
 if(socket_error)return -1;
 if(socket_family==AF_INET6){
  struct sockaddr_in6 in={.sin6_family=AF_INET6};assert(inet_pton(AF_INET6,local_ip,&in.sin6_addr)==1);
  memcpy(address,&in,sizeof(in));*length=sizeof(in);
 }else{
  struct sockaddr_in in={.sin_family=socket_family};assert(inet_pton(AF_INET,local_ip,&in.sin_addr)==1);
  memcpy(address,&in,sizeof(in));*length=sizeof(in);
 }
 if(socket_short)*length=1;
 return 0;
}
static const char *value(const char *name)
{
    if(!strcmp(name,"X-Passport-Expired-State"))return expiry_header;
    if(!strcmp(name,"Host"))return host;if(!strcmp(name,"Origin"))return origin;
    if(!strcmp(name,"Content-Type"))return type;if(!strcmp(name,"Authorization"))return authorization;
    if(!strcmp(name,"X-CSRF-Token"))return csrf;if(!strcmp(name,"X-Passport-Open"))return open_header;
    if(!strcmp(name,"Sec-WebSocket-Protocol"))return subprotocol;return "";
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *req,const char *name) { (void)req;return strlen(value(name)); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,const char *name,char *out,size_t cap)
{ (void)req;assert(strlen(value(name))<cap);strcpy(out,value(name));return ESP_OK; }
int httpd_req_recv(httpd_req_t *req,char *out,size_t count)
{ (void)req;if(count>17)count=17;memcpy(out,input+offset,count);offset+=count;return (int)count; }
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *s) { req->status=s;return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *s) { (void)req;assert(!strcmp(s,"application/json") || strstr(s,"charset=utf-8"));return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *name,const char *s)
{ (void)req;assert(name&&s&&strncmp(name,"Access-Control-",15));return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *s) { (void)req;assert(strlen(s)<sizeof(output));strcpy(output,s);return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *uri)
{ assert(server&&uri->handler);if(uri->is_websocket)assert(uri->ws_pre_handshake_cb==websocket_open);++registered;return ESP_OK; }
// The SDK dispatcher consumes the first frame byte before invoking the handler.
// Keep the pinned receive/unmask implementation; substitute its socket boundary.
#undef ESP_LOGW
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define HTTPD_RECV_OPT_BLOCKING 1
#define HTTPD_RECV_OPT_NONE 0
#define HTTPD_WS_MASK_BIT 0x80
#define HTTPD_WS_LENGTH_BITS 0x7f
#define ESP_ERR_INVALID_SIZE 0x104
struct httpd_req_aux { httpd_ws_type_t ws_type; bool ws_final; uint8_t mask_key[4]; };
static esp_err_t httpd_ws_check_req(httpd_req_t *req) { assert(req->sess_ctx && req->aux);return ESP_OK; }
static int httpd_recv_with_opt(httpd_req_t *req,char *out,size_t length,int option)
{
    (void)req;(void)option;++wire_reads;
    if(length>wire_length-wire_at)return -1;
    memcpy(out,wire+wire_at,length);wire_at+=length;return (int)length;
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "sdk_ws_receive.inc"
#pragma GCC diagnostic pop
esp_err_t httpd_ws_send_frame(httpd_req_t *req,httpd_ws_frame_t *frame)
{ (void)req;++sent_frames;sent_type=frame->type;sent_length=frame->len;assert(frame->len<sizeof(output));memcpy(output,frame->payload,frame->len);output[frame->len]=0;return ESP_OK; }
static void *live_socket_context;
static void (*queued_work)(void *);
void *httpd_sess_get_ctx(httpd_handle_t server,int fd)
{ assert(server==listener && fd==9); return live_socket_context; }
esp_err_t httpd_queue_work(httpd_handle_t server,void (*work)(void *),void *arg)
{ assert(server==listener && !queued_work && !arg); queued_work=work; return ESP_OK; }
esp_err_t httpd_ws_send_frame_async(httpd_handle_t server,int fd,httpd_ws_frame_t *frame)
{ assert(server==listener && fd==9 && live_socket_context); return httpd_ws_send_frame(NULL,frame); }
static void run_push(void)
{
    void (*work)(void *)=queued_work;
    assert(work); queued_work=NULL; work(NULL);
}
static size_t append_frame(httpd_ws_type_t type,bool final,const char *payload)
{
    const uint8_t mask[]={0x12,0x34,0x56,0x78};size_t length=strlen(payload);
    assert(length<=CP_MAX_FRAME_BYTES && wire_length+length+8<=sizeof(wire));
    wire[wire_length++]=(final?0x80:0)|(uint8_t)type;
    wire[wire_length++]=0x80|(length<126 ? (uint8_t)length : 126);
    if(length>=126){wire[wire_length++]=(uint8_t)(length>>8);wire[wire_length++]=(uint8_t)length;}
    memcpy(wire+wire_length,mask,sizeof(mask));wire_length+=sizeof(mask);
    for(size_t i=0;i<length;++i)wire[wire_length++]=(uint8_t)payload[i]^mask[i%4];
    return wire_length;
}
static esp_err_t receive_frame(httpd_req_t *req)
{
    assert(wire_at<wire_length);uint8_t first=wire[wire_at++];
    struct httpd_req_aux aux={.ws_type=(httpd_ws_type_t)(first&0x0f),.ws_final=(first&0x80)!=0};
    req->aux=&aux;req->method=HTTP_POST;
    esp_err_t result=websocket(req);req->aux=NULL;return result;
}
static void extract(const char *key,char *out,size_t capacity)
{
    cj_document_t *d=malloc(sizeof(*d));assert(d&&cj_parse(d,output,strlen(output)));
    const char *s=cj_string(d,cj_get(d,0,key));assert(s&&strlen(s)<capacity);strcpy(out,s);free(d);
}
static void reset(void)
{
    passport_control_deinit();hid_engine_init(&engine);ticks+=20;assert(passport_control_init(&engine,random_bytes));
    network=(passport_wifi_link_t){.ap_started=true,.dhcp_running=true,.ap_ip="192.168.4.1"};
    passport_control_observation_t observed={.ap_ip="192.168.4.1"};passport_control_observe(&observed,ticks);
    mailbox=IDLE;signalled=delay_owner=stale_signal=false;memset(channels,0,sizeof(channels));
    host=local_ip="192.168.4.1";origin="http://192.168.4.1";type="application/json";open_header="1";
    socket_family=AF_INET;socket_short=socket_error=false;
    authorization=csrf=subprotocol="";input="{}";offset=0;
}
static void call(const char *path,int method,const char *body,const char *expected)
{
    input=body;offset=0;httpd_req_t req={.uri=path,.method=method,.content_len=strlen(body),.status="200 OK"};
    assert(api(&req)==ESP_OK);if(strcmp(req.status,expected))fprintf(stderr,"%s: %s %s\n",path,req.status,output);
    assert(!strcmp(req.status,expected));
}
static void session_open(void)
{
    call("/api/v1/open-session",HTTP_POST,"{}","200 OK");extract("session_token",token,sizeof(token));extract("csrf",csrf_token,sizeof(csrf_token));
    snprintf(auth,sizeof(auth),"Bearer %s",token);authorization=auth;csrf=csrf_token;
}
static void test_move_reply(bool ws)
{
    reset();exercise_moves=true;
    hid_engine_set_link(&engine,true,true);
    service_hid();service_hid();
    passport_control_observation_t observed={.ap_ip="192.168.4.1",.target="host",.transport_generation=7};
    passport_control_observe(&observed,ticks);
    session_open();
    char sid[33],prefix[17],boot[33],envelope[1024],base[512],key[24],path[64];
    extract("session_id",sid,sizeof(sid));extract("request_prefix",prefix,sizeof(prefix));extract("boot_id",boot,sizeof(boot));
    call("/api/v1/state",HTTP_GET,"","200 OK");
    cj_document_t d;assert(cj_parse(&d,output,strlen(output)));
    uint16_t state=cj_get(&d,0,"protocol");
    snprintf(envelope,sizeof(envelope),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":1,"
        "\"deadline_ms\":\"%u\",\"epoch\":\"%s\",\"transport_generation\":\"7\",\"target_id\":\"host\","
        "\"op\":\"control.acquire\",\"args\":{\"state_sequence\":\"%s\"}}",boot,sid,prefix,1,ticks+1500,
        cj_string(&d,cj_get(&d,state,"epoch")),cj_string(&d,cj_get(&d,state,"state_sequence")));
    call("/api/v1/commands",HTTP_POST,envelope,"202 Accepted");
    service_hid();call("/api/v1/state",HTTP_GET,"","200 OK");
    assert(cj_parse(&d,output,strlen(output)));state=cj_get(&d,0,"protocol");
    char owned_lease[33];strcpy(owned_lease,cj_string(&d,cj_get(&d,state,"lease_id")));
    snprintf(base,sizeof(base),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"epoch\":\"%s\","
        "\"transport_generation\":\"7\",\"target_id\":\"host\",\"lease_id\":\"%s\",\"op\":\"mouse.move_relative\"",
        boot,sid,cj_string(&d,cj_get(&d,state,"epoch")),cj_string(&d,cj_get(&d,state,"lease_id")));
    if(ws) {
        // Browser frames share the HTTP-acquired lease and the same HID owner.
        call("/api/v1/ws-ticket",HTTP_POST,"","200 OK");extract("ticket",ticket,sizeof(ticket));
        snprintf(protocol_header,sizeof(protocol_header),"passport.v1, %s",ticket);subprotocol=protocol_header;
        authorization=csrf="";
        httpd_req_t channel={.uri="/api/v1/ws",.method=HTTP_GET};
        assert(websocket_open(&channel)==ESP_OK && websocket(&channel)==ESP_OK);
        for(unsigned seq=2;seq<=4;seq++){
            skip_hid=seq==3;
            unsigned before=accepted_moves;
            snprintf(envelope,sizeof(envelope),"%s,\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%u\","
                "\"args\":{\"dx\":%u,\"dy\":0}}",base,prefix,seq,seq,seq==4?ticks:ticks+150,seq);
            append_frame(HTTPD_WS_TYPE_TEXT,true,envelope);
            assert(receive_frame(&channel)==ESP_OK && sent_type==HTTPD_WS_TYPE_TEXT);
            extract("command_key",key,sizeof(key));
            if(seq==2)assert(strstr(output,"\"state\":\"sent\"") && accepted_moves==before+1);
            if(seq==3){
                assert(strstr(output,"\"state\":\"accepted\"") && accepted_moves==before);
                skip_hid=false;service_hid();authorization=auth;csrf=csrf_token;
                snprintf(path,sizeof(path),"/api/v1/requests/%s",key);
                call(path,HTTP_GET,"","200 OK");assert(strstr(output,"\"state\":\"sent\"") && accepted_moves==before+1);
            }
            if(seq==4)assert(strstr(output,"\"error\":\"expired\"") && strstr(output,owned_lease) && accepted_moves==before);
        }
        live_socket_context=channel.sess_ctx;
        wire_at=wire_length=0;
        append_frame(HTTPD_WS_TYPE_TEXT,true,"{\"v\":1,\"type\":\"motion_window\",\"motion_window\":2,\"motion_ordered\":true}");
        assert(receive_frame(&channel)==ESP_OK && strstr(output,"\"motion_ordered\":true"));
        for(unsigned first=5;first<=7;first+=2){
            unsigned before=accepted_moves;
            size_t boundaries[2];
            const unsigned deadline=ticks+(first==7?3:4);
            for(unsigned j=0;j<2;j++){
                unsigned seq=first+j;
                snprintf(envelope,sizeof(envelope),"%s,\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%u\","
                    "\"args\":{\"dx\":%d,\"dy\":0}}",base,prefix,seq,seq,j ? deadline : ticks+150,j?-9:12);
                boundaries[j]=append_frame(HTTPD_WS_TYPE_TEXT,true,envelope);
            }
            char keys[2][24];
            for(unsigned j=0;j<2;j++){
                allocation_ms=j;
                unsigned freed=expired_buffers;
                assert(receive_frame(&channel)==ESP_OK && wire_at==boundaries[j] && mailbox==IDLE);
                assert(cj_parse(&d,output,strlen(output)));
                int64_t next=0;assert(cj_integer(&d,cj_get(&d,0,"next_seq"),&next) && next==first+j+1);
                int64_t command_seq=0;
                assert(cj_integer(&d,cj_get(&d,0,"command_seq"),&command_seq) && command_seq==first+j);
                assert(cj_get(&d,0,"motion_terminal")==CJ_NONE);
                extract("command_key",keys[j],sizeof(keys[j]));
                bool expired=first+j==8;
                assert(expired_buffers==freed+expired && !report_buffer.data);
                if(j){
                    assert(ticks==deadline-!expired);
                    if(expired){
                        char reason[64];snprintf(reason,sizeof(reason),"send now=%u deadline=%u",ticks,deadline);
                        assert(strstr(output,reason));
                    }
                    printf("seq %u: BLE handoff clock %u, deadline %u, %s\n",first+j,ticks,deadline,
                           expired?"expired without notify":"notify accepted");
                }
                allocation_ms=0;
                assert(strstr(output,expired?"\"error\":\"expired\"":"\"state\":\"sent\""));
                assert(accepted_moves==before+j+!expired && last_dx==(j&&!expired?-9:12));
            }
            assert(strcmp(keys[0],keys[1]) && sent_frames>=2);
            printf("queued pair %u/%u, ordered complete frame boundaries, distinct keys, %u real reports\n",
                   first,first+1,(unsigned)(accepted_moves-before));
        }
        wire_at=wire_length=0;
        // The production page requests three slots with keyed admissions.
        append_frame(HTTPD_WS_TYPE_TEXT,true,"{\"v\":1,\"type\":\"motion_window\",\"motion_window\":3}");
        assert(receive_frame(&channel)==ESP_OK && strstr(output,"\"motion_window\":3") && !strstr(output,"motion_ordered"));
        for (unsigned seq=9;seq<=10;++seq) {
            congested=seq==10;
            service_hid();
            call("/api/v1/state?view=protocol",HTTP_GET,"","200 OK");
            assert(!strstr(output,"\"device\":"));
            assert(cj_parse(&d,output,strlen(output)));state=cj_get(&d,0,"protocol");
            uint64_t prior=strtoull(cj_string(&d,cj_get(&d,state,"lease_expires_ms")),NULL,10);
            snprintf(envelope,sizeof(envelope),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\","
                "\"seq\":%u,\"deadline_ms\":\"%u\",\"epoch\":\"%s\",\"transport_generation\":\"7\",\"target_id\":\"host\","
                "\"lease_id\":\"%s\",\"op\":\"control.renew\",\"args\":{\"state_sequence\":\"%s\"}}",
                boot,sid,prefix,seq,seq,ticks+1500,cj_string(&d,cj_get(&d,state,"epoch")),
                owned_lease,cj_string(&d,cj_get(&d,state,"state_sequence")));
            append_frame(HTTPD_WS_TYPE_TEXT,true,envelope);
            assert(receive_frame(&channel)==ESP_OK);
            extract("command_key",key,sizeof(key));
            assert(!strstr(output,"\"motion_terminal\":true"));
            assert(strstr(output,"\"state\":\"accepted\""));
            if(congested){
                passport_control_http_service(ticks);assert(!queued_work);
                congested=false;service_hid();
            }
            passport_control_http_service(ticks);assert(queued_work);run_push();
            assert(strstr(output,"\"motion_terminal\":true"));
            assert(cj_parse(&d,output,strlen(output)));
            int64_t ack=0;
            assert(cj_integer(&d,cj_get(&d,0,"command_seq"),&ack) && ack==seq);
            assert(cj_integer(&d,cj_get(&d,0,"next_seq"),&ack) && ack==seq+1);
            assert(!strcmp(cj_string(&d,cj_get(&d,0,"command_key")),key));
            assert(strstr(output,"\"state\":\"applied\"") && strstr(output,"\"lease_active\":true") && strstr(output,owned_lease));
            assert(strtoull(cj_string(&d,cj_get(&d,0,"lease_expires_ms")),NULL,10)>prior);
        }
        for(unsigned seq=11;seq<=12;++seq){
            if(seq==12){
                append_frame(HTTPD_WS_TYPE_TEXT,true,"{\"v\":1,\"type\":\"motion_window\",\"motion_window\":2}");
                assert(receive_frame(&channel)==ESP_OK && strstr(output,"\"motion_window\":2") && !strstr(output,"motion_ordered"));
            }
            skip_hid=seq==11;
            snprintf(envelope,sizeof(envelope),"%s,\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%u\","
                "\"args\":{\"dx\":4,\"dy\":0}}",base,prefix,seq,seq,ticks+150);
            append_frame(HTTPD_WS_TYPE_TEXT,true,envelope);
            unsigned before=accepted_moves;
            assert(receive_frame(&channel)==ESP_OK && strstr(output,"\"state\":\"accepted\""));
            assert(accepted_moves==before+(seq==12));
            extract("command_key",key,sizeof(key));
            skip_hid=false;service_hid();
            assert(accepted_moves==before+1);
            passport_control_http_service(ticks);
            assert(queued_work);
            if(seq==12)break;
            run_push();
            char terminal_key[24];extract("command_key",terminal_key,sizeof(terminal_key));
            assert(!strcmp(key,terminal_key) && strstr(output,"\"command_seq\":11") &&
                   strstr(output,"\"motion_terminal\":true") && strstr(output,"\"state\":\"sent\""));
        }
        channel.free_ctx(channel.sess_ctx);passport_control_http_service(ticks);
        live_socket_context=NULL;
        unsigned sent_before=sent_frames;run_push();assert(sent_frames==sent_before && !delivery.channel);
        call("/api/v1/state",HTTP_GET,"","200 OK");assert(!strstr(output,owned_lease));
        exercise_moves=false;
        wire_at=wire_length=wire_reads=sent_frames=0;
        return;
    }
    enum { INLINE=2, PENDING, ABANDONED_MOVE, EXPIRED, EXPIRED_WITH_STATE, TRANSPORT_FAILURE };
    for(unsigned seq=INLINE;seq<=TRANSPORT_FAILURE;++seq){
        skip_hid=seq==PENDING;timeout_move=seq==ABANDONED_MOVE;fail_hid=seq==TRANSPORT_FAILURE;
        const bool expired=seq==EXPIRED || seq==EXPIRED_WITH_STATE;
        expiry_header=seq==EXPIRED ? "" : "1";
        uint64_t before=accepted_moves;
        snprintf(envelope,sizeof(envelope),"%s,\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%u\","
            "\"args\":{\"dx\":%u,\"dy\":0}}",base,prefix,seq,seq,expired?ticks:ticks+150,seq);
        const char *status=skip_hid?"202 Accepted":timeout_move?"503 Service Unavailable":"200 OK";
        if(expired)status="400 Bad Request";
        if(fail_hid)status="202 Accepted";
        call("/api/v1/commands",HTTP_POST,envelope,status);
        if(timeout_move){
            assert(strstr(output,"unknown") && mailbox==ABANDONED && !begin());
            passport_control_http_finish_move(timed_out_key,ticks);
            assert(mailbox==IDLE && accepted_moves==before+1);
            timeout_move=false;
            call("/api/v1/commands",HTTP_POST,envelope,"200 OK");
        }
        if(skip_hid){
            assert(strstr(output,"accepted") && accepted_moves==before);
            extract("command_key",key,sizeof(key));snprintf(path,sizeof(path),"/api/v1/requests/%s",key);
            skip_hid=false;service_hid();
            call(path,HTTP_GET,"","200 OK");
        }
        if(seq<=ABANDONED_MOVE){
            assert(strstr(output,"\"state\":\"sent\"") && accepted_moves==before+1 && last_dx==(int)seq);
            char retained[sizeof(output)];strcpy(retained,output);
            call("/api/v1/commands",HTTP_POST,envelope,"200 OK");
            assert(!strcmp(output,retained) && accepted_moves==before+1);
        }else{
            assert(accepted_moves==before && !strstr(output,"\"state\":\"sent\""));
            if(expired) {
                assert(strstr(output,"expired") && cj_parse(&d,output,strlen(output)));
                uint16_t snapshot=cj_get(&d,0,"protocol");
                if(seq==EXPIRED_WITH_STATE) {
                    assert(snapshot!=CJ_NONE && !strcmp(cj_string(&d,cj_get(&d,snapshot,"target_id")),"host"));
                    assert(cj_get(&d,snapshot,"lease_id")!=CJ_NONE && cj_get(&d,snapshot,"now_ms")!=CJ_NONE);
                    assert(!strcmp(owned_lease,cj_string(&d,cj_get(&d,snapshot,"lease_id"))));
                    char original_key[24];extract("command_key",original_key,sizeof(original_key));
                    call("/api/v1/commands",HTTP_POST,envelope,"400 Bad Request");
                    extract("command_key",key,sizeof(key));assert(!strcmp(original_key,key));
                    assert(!strstr(output,"\"protocol\""));
                } else assert(snapshot==CJ_NONE);
            }
            if(fail_hid) {
                assert(failed_reports==1 && strstr(output,"\"state\":\"accepted\""));
                extract("command_key",key,sizeof(key));snprintf(path,sizeof(path),"/api/v1/requests/%s",key);
                fail_hid=false;service_hid();service_hid();
                call(path,HTTP_GET,"","400 Bad Request");
                assert(strstr(output,"\"state\":\"cancelled\"") && accepted_moves==before);
            }
        }
    }
    expiry_header="";fail_hid=exercise_moves=false;
    // A fresh session cannot observe an old owner's retained receipt.
    session_open();
    extract("session_id",sid,sizeof(sid));extract("request_prefix",prefix,sizeof(prefix));
    expiry_header="1";
    snprintf(envelope,sizeof(envelope),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\","
        "\"seq\":1,\"deadline_ms\":\"%u\",\"op\":\"mouse.move_relative\",\"args\":{\"dx\":1,\"dy\":0}}",
        boot,sid,prefix,1,ticks);
    call("/api/v1/commands",HTTP_POST,envelope,"400 Bad Request");
    assert(cj_parse(&d,output,strlen(output)));
    state=cj_get(&d,0,"protocol");assert(state!=CJ_NONE && cj_get(&d,state,"lease_id")==CJ_NONE);
    assert(!strstr(output,owned_lease));expiry_header="";
    call(path,HTTP_GET,"","409 Conflict");
    assert(strstr(output,"result_unknown") && !strstr(output,"command_key"));
}
int main(void)
{
    assert(passport_control_http_register((void *)1)==ESP_OK&&registered==8);
    test_move_reply(true);test_move_reply(false);reset();
    httpd_req_t asset={.uri="/",.method=HTTP_GET};
    assert(web(&asset)==ESP_OK&&!strcmp(output,controller_html_fixture));
    asset.uri="/motion_window.mjs";assert(web(&asset)==ESP_OK&&!strcmp(output,motion_js_fixture));
    asset.uri="/";host="foreign.example";assert(web(&asset)==ESP_OK&&!strcmp(asset.status,"403 Forbidden"));
    reset();host="evil.test";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();origin="http://evil.test";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();local_ip="192.168.1.3";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();socket_family=AF_INET6;local_ip="::ffff:192.168.4.1";session_open();
    assert(web(&asset)==ESP_OK&&!strcmp(output,controller_html_fixture));
    reset();socket_family=AF_INET6;local_ip="::ffff:192.168.1.3";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();socket_family=AF_INET6;local_ip="fd00::1";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();socket_family=AF_UNSPEC;call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();socket_short=true;call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();socket_error=true;call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();type="text/plain";call("/api/v1/open-session",HTTP_POST,"{}","400 Bad Request");
    reset();open_header="";call("/api/v1/open-session",HTTP_POST,"{}","403 Forbidden");
    reset();network.phase=PW_ONLINE;strcpy(network.sta_ip,"192.168.1.3");host=local_ip="192.168.1.3";origin="http://192.168.1.3";session_open();
    reset();network.phase=PW_ONLINE;strcpy(network.sta_ip,"192.168.1.3");host="192.168.1.3";local_ip="::ffff:192.168.1.3";socket_family=AF_INET6;origin="http://192.168.1.3";session_open();
    call("/api/v1/state",HTTP_GET,"","200 OK");
    call("/api/v1/operations?cursor=20&limit=4",HTTP_GET,"","200 OK");assert(strstr(output,"control.acquire"));
    reset();delay_owner=true;call("/api/v1/open-session",HTTP_POST,"{}","503 Service Unavailable");assert(mailbox==ABANDONED&&!begin());
    call("/api/v1/open-session",HTTP_POST,"{}","429 Too Many Requests");assert(strstr(output,"rejected")&&!strstr(output,"unknown"));
    passport_control_http_service(ticks);assert(mailbox==IDLE);delay_owner=false;
    stale_signal=true;session_open();assert(!stale_signal&&mailbox==IDLE);
    call("/api/v1/ws-ticket",HTTP_POST,"","200 OK");extract("ticket",ticket,sizeof(ticket));snprintf(protocol_header,sizeof(protocol_header),"passport.v1, %s",ticket);subprotocol=protocol_header;authorization=csrf="";
    httpd_req_t req={.uri="/api/v1/ws",.method=HTTP_GET};assert(websocket_open(&req)==ESP_OK&&req.sess_ctx);assert(websocket(&req)==ESP_OK);
    httpd_req_t duplicate={.uri="/api/v1/ws",.method=HTTP_GET};assert(websocket_open(&duplicate)==ESP_FAIL&&!duplicate.sess_ctx);
    const char state_frame[]="{\"v\":1,\"resource\":\"/api/v1/state\"}";
    size_t boundaries[]={append_frame(HTTPD_WS_TYPE_PING,true,""),append_frame(HTTPD_WS_TYPE_PING,true,"probe"),
        append_frame(HTTPD_WS_TYPE_PONG,true,""),append_frame(HTTPD_WS_TYPE_PONG,true,"reply"),
        append_frame(HTTPD_WS_TYPE_TEXT,true,""),append_frame(HTTPD_WS_TYPE_TEXT,true,state_frame)};
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[0] && wire_reads==2);
    assert(sent_frames==1 && sent_type==HTTPD_WS_TYPE_PONG && sent_length==0);
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[1]);
    assert(sent_frames==2 && sent_type==HTTPD_WS_TYPE_PONG && sent_length==5 && !strcmp(output,"probe"));
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[2] && sent_frames==2);
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[3] && sent_frames==2);
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[4]);
    assert(sent_frames==3 && sent_type==HTTPD_WS_TYPE_TEXT && strstr(output,"\"error\":\"invalid\""));
    assert(receive_frame(&req)==ESP_OK && wire_at==boundaries[5]);
    assert(sent_frames==4 && sent_type==HTTPD_WS_TYPE_TEXT && strstr(output,"protocol"));
    append_frame(HTTPD_WS_TYPE_TEXT,false,state_frame);
    assert(receive_frame(&req)==ESP_FAIL);req.free_ctx(req.sess_ctx);passport_control_http_service(ticks);assert(!channels[0].used);
    authorization=auth;csrf=csrf_token;call("/api/v1/state",HTTP_GET,"","200 OK");
    call("/api/v1/sessions/current",HTTP_DELETE,"","200 OK");call("/api/v1/state",HTTP_GET,"","401 Unauthorized");
    reset();assert(begin());strcpy(request.host,host);strcpy(request.origin,origin);strcpy(request.open,"1");
    strcpy(request.method,"POST");strcpy(request.resource,"/api/v1/open-session");strcpy(request.body,"{}");request.length=2;
    mailbox=READY;abandon_during_random=true;passport_control_http_service(ticks);
    assert(mailbox==IDLE&&!abandon_during_random);
    for(size_t i=0;i<sizeof(request);++i)assert(!((unsigned char *)&request)[i]);
    session_open();call("/api/v1/open-session",HTTP_POST,"{}","429 Too Many Requests"); // The lost response was executed once.
    passport_control_deinit();puts("PASS: production HTTP/WS transport and pinned SDK receive, empty/nonempty PING/PONG and next-frame boundaries, empty-text rejection, session close, AP+STA binding, pre-handshake tickets and mailbox timeout/backpressure");
}
