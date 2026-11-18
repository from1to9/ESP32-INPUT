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
static TickType_t ticks=100;
static bool signalled,delay_owner,stale_signal;
static bool abandon_during_random;
static unsigned entropy,registered,sent_frames;
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
    ++ticks;passport_control_http_service(ticks);assert(signalled);signalled=false;return pdTRUE;
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
static size_t append_frame(httpd_ws_type_t type,bool final,const char *payload)
{
    const uint8_t mask[]={0x12,0x34,0x56,0x78};size_t length=strlen(payload);
    assert(length<126 && wire_length+length+6<=sizeof(wire));
    wire[wire_length++]=(final?0x80:0)|(uint8_t)type;wire[wire_length++]=0x80|(uint8_t)length;
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
int main(void)
{
    assert(passport_control_http_register((void *)1)==ESP_OK&&registered==7);
    reset();
    httpd_req_t asset={.uri="/",.method=HTTP_GET};
    assert(web(&asset)==ESP_OK&&!strcmp(output,controller_html_fixture));
    host="foreign.example";assert(web(&asset)==ESP_OK&&!strcmp(asset.status,"403 Forbidden"));
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
