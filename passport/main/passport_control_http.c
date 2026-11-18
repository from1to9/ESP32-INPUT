// SPDX-License-Identifier: Apache-2.0
#include "passport_control_http.h"
#include "passport_control.h"
#include "passport_wifi.h"
#include "passport_http_address.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// One HTTP task owns request/response buffers, the HID owner only borrows them
// between READY and DONE. A timed-out request cannot be reused while executing.
typedef enum { IDLE, WRITING, READY, RUNNING, DONE, ABANDONED } mailbox_state_t;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static mailbox_state_t mailbox;
static passport_control_request_t request;
static SemaphoreHandle_t completed;
static passport_wifi_link_t wifi;
typedef struct { bool used, closed; cp_identity_t identity; } channel_t;
static channel_t channels[CP_MAX_SESSIONS];

static void wipe(void *data, size_t size) { explicit_bzero(data,size); }
static bool begin(void)
{
    portENTER_CRITICAL(&lock);
    bool ok=completed && mailbox==IDLE;
    if (ok) mailbox=WRITING;
    portEXIT_CRITICAL(&lock);
    if (ok) { (void)xSemaphoreTake(completed,0); wipe(&request,sizeof(request)); }
    return ok;
}
static void release(void)
{
    wipe(&request,sizeof(request));
    portENTER_CRITICAL(&lock); mailbox=IDLE; portEXIT_CRITICAL(&lock);
}
static bool exchange(void)
{
    const TickType_t started=xTaskGetTickCount(), limit=pdMS_TO_TICKS(1000);
    portENTER_CRITICAL(&lock); mailbox=READY; portEXIT_CRITICAL(&lock);
    for (;;) {
        TickType_t elapsed=xTaskGetTickCount()-started;
        if (elapsed<limit) (void)xSemaphoreTake(completed,limit-elapsed);
        portENTER_CRITICAL(&lock);
        bool ok=mailbox==DONE, expired=xTaskGetTickCount()-started>=limit;
        if (!ok && expired) mailbox=ABANDONED;
        portEXIT_CRITICAL(&lock);
        if (ok || expired) return ok;
        // A late notification from the previous completed request is harmless.
    }
}
void passport_control_http_service(uint64_t now)
{
    for (unsigned i=0;i<CP_MAX_SESSIONS;++i) {
        portENTER_CRITICAL(&lock);
        bool closed=channels[i].used && channels[i].closed;
        portEXIT_CRITICAL(&lock);
        if (closed) {
            passport_control_close(&channels[i].identity,now);
            portENTER_CRITICAL(&lock); memset(&channels[i],0,sizeof(channels[i])); portEXIT_CRITICAL(&lock);
        }
    }
    portENTER_CRITICAL(&lock);
    bool abandoned=mailbox==ABANDONED, ready=mailbox==READY;
    if (ready || abandoned) mailbox=RUNNING;
    portEXIT_CRITICAL(&lock);
    if (!ready && !abandoned) return;
    if (ready) passport_control_execute(&request,now);
    portENTER_CRITICAL(&lock);
    abandoned=abandoned || mailbox==ABANDONED;
    if (!abandoned) mailbox=DONE;
    portEXIT_CRITICAL(&lock);
    if (abandoned) {
        if (ready && request.kind==PC_WS_OPEN && request.error==CP_OK) passport_control_close(&request.identity,now);
        release();
    } else xSemaphoreGive(completed);
}
static bool header(httpd_req_t *req,const char *name,char *out,size_t size,bool required)
{
    size_t n=httpd_req_get_hdr_value_len(req,name);
    if (!n) return !required;
    return n<size && httpd_req_get_hdr_value_str(req,name,out,size)==ESP_OK;
}
static bool binding(httpd_req_t *req)
{
    passport_wifi_link_snapshot(&wifi);
    struct in_addr local;
    if (!passport_http_local_ipv4(httpd_req_to_sockfd(req), &local)) return false;
    char ip[16], with_port[32];
    if (!inet_ntop(AF_INET,&local,ip,sizeof(ip))) return false;
    bool sta=wifi.phase==PW_ONLINE && !strcmp(ip,wifi.sta_ip);
    bool ap=wifi.ap_started && wifi.dhcp_running && !strcmp(ip,wifi.ap_ip);
    if (!sta && !ap) return false;
    snprintf(with_port,sizeof(with_port),"%s:80",ip);
    if (!header(req,"Host",request.host,sizeof(request.host),true) ||
        (strcmp(request.host,ip) && strcmp(request.host,with_port))) return false;
    if (!header(req,"Origin",request.origin,sizeof(request.origin),false) ||
        !header(req,"Authorization",request.authorization,sizeof(request.authorization),false) ||
        !header(req,"X-CSRF-Token",request.csrf,sizeof(request.csrf),false) ||
        !header(req,"X-Passport-Open",request.open,sizeof(request.open),false)) return false;
    if (request.origin[0]) {
        char expected[48]; snprintf(expected,sizeof(expected),"http://%s",request.host);
        if (strcmp(expected,request.origin)) return false;
    }
    return true;
}
static esp_err_t error(httpd_req_t *req,const char *status,const char *body)
{
    httpd_resp_set_status(req,status); httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return httpd_resp_sendstr(req,body);
}
static esp_err_t unknown(httpd_req_t *req)
{ return error(req,"503 Service Unavailable","{\"error\":\"owner_timeout\",\"outcome\":\"unknown\"}"); }
static const char *status(unsigned code)
{
    switch (code) {
    case 200:return "200 OK";case 202:return "202 Accepted";case 400:return "400 Bad Request";
    case 401:return "401 Unauthorized";case 403:return "403 Forbidden";case 404:return "404 Not Found";
    case 408:return "408 Request Timeout";
    case 423:return "423 Locked";
    case 409:return "409 Conflict";case 413:return "413 Payload Too Large";case 422:return "422 Unprocessable Entity";
    case 429:return "429 Too Many Requests";case 501:return "501 Not Implemented";
    default:return "503 Service Unavailable";
    }
}
static esp_err_t api(httpd_req_t *req)
{
    if (!begin()) return error(req,"429 Too Many Requests","{\"state\":\"rejected\",\"error\":\"busy\"}");
    if (!binding(req)) { release(); return error(req,"403 Forbidden","{\"error\":\"origin_or_interface\"}"); }
    if (strlen(req->uri)>=sizeof(request.resource) || req->content_len>CP_MAX_FRAME_BYTES) {
        release(); return error(req,"413 Payload Too Large","{\"error\":\"request_bounds\"}");
    }
    strcpy(request.resource,req->uri);
    strcpy(request.method,req->method==HTTP_GET ? "GET" : req->method==HTTP_POST ? "POST" : "DELETE");
    if (req->method!=HTTP_GET) {
        char type[48]={0};
        if (!header(req,"Content-Type",type,sizeof(type),true) ||
            (strcasecmp(type,"application/json") && strcasecmp(type,"application/json; charset=utf-8"))) {
            release(); return error(req,"400 Bad Request","{\"error\":\"json_required\"}");
        }
    } else if (req->content_len) { release(); return error(req,"400 Bad Request","{\"error\":\"unexpected_body\"}"); }
    while (request.length<req->content_len) {
        int n=httpd_req_recv(req,request.body+request.length,req->content_len-request.length);
        if (n<=0) { release(); return ESP_FAIL; }
        request.length+=(size_t)n;
    }
    if (!exchange()) return unknown(req); // Owner clears the abandoned buffer.
    esp_err_t rc=request.response.status ? error(req,status(request.response.status),request.response.body) :
        error(req,"503 Service Unavailable","{\"state\":\"rejected\",\"error\":\"unavailable\"}");
    release(); return rc;
}
static void channel_closed(void *context)
{
    channel_t *channel=context;
    portENTER_CRITICAL(&lock); channel->closed=true; portEXIT_CRITICAL(&lock);
}
static esp_err_t websocket_open(httpd_req_t *req)
{
    if (!begin()) return ESP_FAIL;
    if (!binding(req) || req->content_len || strcmp(req->uri,"/api/v1/ws")) { release(); return ESP_FAIL; }
    char protocols[96]={0};
    if (!header(req,"Sec-WebSocket-Protocol",protocols,sizeof(protocols),true)) { release(); return ESP_FAIL; }
    if (request.origin[0]) {
        const char prefix[]="passport.v1, ";
        if (strncmp(protocols,prefix,strlen(prefix)) || strlen(protocols+strlen(prefix))!=64) { release(); return ESP_FAIL; }
        strcpy(request.ticket,protocols+strlen(prefix));
    } else if (strcmp(protocols,"passport.v1")) { release(); return ESP_FAIL; }
    channel_t *channel=NULL;
    portENTER_CRITICAL(&lock);
    for (size_t i=0;i<CP_MAX_SESSIONS;++i) if (!channels[i].used) { channel=&channels[i]; channel->used=true; break; }
    portEXIT_CRITICAL(&lock);
    if (!channel) { release(); return ESP_FAIL; }
    request.kind=PC_WS_OPEN;
    bool done=exchange();
    if (!done || request.error!=CP_OK) {
        portENTER_CRITICAL(&lock); memset(channel,0,sizeof(*channel)); portEXIT_CRITICAL(&lock);
        if (done) release();
        return ESP_FAIL;
    }
    channel->identity=request.identity;
    req->sess_ctx=channel; req->free_ctx=channel_closed;
    release(); return ESP_OK;
}
static esp_err_t websocket(httpd_req_t *req)
{
    if (req->method==HTTP_GET) return req->sess_ctx ? ESP_OK : ESP_FAIL;
    if (!begin()) return ESP_FAIL;
    channel_t *channel=req->sess_ctx;
    if (!channel) { release(); return ESP_FAIL; }
    httpd_ws_frame_t frame={0};
    if (httpd_ws_recv_frame(req,&frame,0)!=ESP_OK || frame.len>CP_MAX_FRAME_BYTES || !frame.final ||
        (frame.type!=HTTPD_WS_TYPE_TEXT && frame.type!=HTTPD_WS_TYPE_PING && frame.type!=HTTPD_WS_TYPE_PONG && frame.type!=HTTPD_WS_TYPE_CLOSE)) { release(); return ESP_FAIL; }
    frame.payload=(uint8_t *)request.body;
    // In the pinned SDK, len==0 requests a new header even on a second call.
    if (frame.len && httpd_ws_recv_frame(req,&frame,CP_MAX_FRAME_BYTES)!=ESP_OK) { release(); return ESP_FAIL; }
    if (frame.type==HTTPD_WS_TYPE_CLOSE) { release(); return ESP_FAIL; }
    request.identity=channel->identity; request.length=frame.len;
    request.kind=frame.type==HTTPD_WS_TYPE_TEXT ? PC_WS_FRAME : PC_WS_HEARTBEAT;
    if (!exchange()) return ESP_FAIL;
    esp_err_t rc=ESP_OK;
    if (frame.type==HTTPD_WS_TYPE_TEXT && request.response.status) {
        frame=(httpd_ws_frame_t){.final=true,.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t *)request.response.body,.len=request.response.length};
        rc=httpd_ws_send_frame(req,&frame);
    } else if (request.error!=CP_OK) rc=ESP_FAIL;
    else if (frame.type==HTTPD_WS_TYPE_PING) { frame.type=HTTPD_WS_TYPE_PONG; rc=httpd_ws_send_frame(req,&frame); }
    release(); return rc;
}
extern const char controller_html[] __asm__("_binary_controller_html_start");
extern const char controller_css[] __asm__("_binary_controller_css_start");
extern const char controller_js[] __asm__("_binary_controller_js_start");
static esp_err_t web(httpd_req_t *req)
{
    if (!begin()) return error(req,"429 Too Many Requests","{\"error\":\"busy\"}");
    bool allowed=binding(req) && !req->content_len;
    release();
    if (!allowed) return error(req,"403 Forbidden","{\"error\":\"origin_or_interface\"}");
    if (strcmp(req->uri,"/") && strcmp(req->uri,"/controller.css") && strcmp(req->uri,"/controller.js")) return error(req,"404 Not Found","{\"error\":\"not_found\"}");
    const char *asset=!strcmp(req->uri,"/") ? controller_html : !strcmp(req->uri,"/controller.css") ? controller_css : controller_js;
    httpd_resp_set_type(req,!strcmp(req->uri,"/") ? "text/html; charset=utf-8" : !strcmp(req->uri,"/controller.css") ? "text/css; charset=utf-8" : "text/javascript; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
    httpd_resp_set_hdr(req,"Cross-Origin-Resource-Policy","same-origin");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    return httpd_resp_sendstr(req,asset);
}
esp_err_t passport_control_http_register(httpd_handle_t server)
{
    completed=xSemaphoreCreateBinary();
    if (!completed) return ESP_ERR_NO_MEM;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=web},
        {.uri="/controller.css",.method=HTTP_GET,.handler=web},
        {.uri="/controller.js",.method=HTTP_GET,.handler=web},
        {.uri="/api/v1/ws",.method=HTTP_GET,.handler=websocket,.is_websocket=true,.handle_ws_control_frames=true,.supported_subprotocol="passport.v1",.ws_pre_handshake_cb=websocket_open},
        {.uri="/api/v1/*",.method=HTTP_GET,.handler=api},
        {.uri="/api/v1/*",.method=HTTP_POST,.handler=api},
        {.uri="/api/v1/*",.method=HTTP_DELETE,.handler=api}
    };
    for (size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        esp_err_t rc=httpd_register_uri_handler(server,&routes[i]);
        if (rc!=ESP_OK) return rc;
    }
    return ESP_OK;
}
