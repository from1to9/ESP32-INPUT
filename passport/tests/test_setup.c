#include <assert.h>
#include <stdlib.h>
#include "../main/passport_setup.c"
const char setup_html[] __asm__("_binary_setup_html_start") = "<!doctype html>fixture";
const char setup_css[] __asm__("_binary_setup_css_start") = "body{}";
const char setup_js[] __asm__("_binary_setup_js_start") = "'use strict';";
static passport_wifi_state_t fixture;
static passport_wifi_submission_t submitted;
static unsigned submissions, scan_requests, registrations, register_failure;
static bool queue_available, provider, token_available;
static const char *host, *origin, *content_type, *request_token, *input, *local_ip;
static const char session_token[] = "0123456789abcdef0123456789abcdef";
static char output[8192];
static size_t offset;
static int station_result, recv_failure;
static int64_t clock_us;
static unsigned allocation, fail_allocation;
static void *allocate(size_t size) { return ++allocation == fail_allocation ? NULL : malloc(size); }
void explicit_bzero(void *data, size_t size) { volatile unsigned char *p=data; while(size--) *p++=0; }
void passport_wifi_snapshot(passport_wifi_state_t *out) { *out=fixture; }
bool passport_wifi_setup_token(char token[33]) { if(token_available) memcpy(token,session_token,33); return token_available; }
bool passport_wifi_submit(const passport_wifi_submission_t *s,const char token[33]) { assert(!strcmp(token,session_token)); ++submissions; submitted=*s; return queue_available; }
bool passport_wifi_scan(uint32_t id,const char token[33]) { assert(id==fixture.setup_id && !strcmp(token,session_token)); ++scan_requests; return queue_available; }
void passport_wifi_set_provisioning_available(bool value) { provider=value; }
uint32_t esp_random(void) { return 123456; }
int64_t esp_timer_get_time(void) { return clock_us+=1000; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) { (void)record; return station_result; }
int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 9; }
int setup_getsockname(int fd,struct sockaddr *address,socklen_t *length)
{ assert(fd==9 && *length==sizeof(struct sockaddr_in)); struct sockaddr_in *in=(void *)address; in->sin_family=AF_INET; assert(inet_pton(AF_INET,local_ip,&in->sin_addr)==1); return 0; }
static const char *value(const char *name)
{ if(!strcmp(name,"Host"))return host; if(!strcmp(name,"Origin"))return origin; if(!strcmp(name,"Content-Type"))return content_type; if(!strcmp(name,"X-Passport-Setup"))return request_token; return ""; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *req,const char *name) { (void)req; return strlen(value(name)); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,const char *name,char *out,size_t cap) { (void)req; assert(strlen(value(name))<cap); snprintf(out,cap,"%s",value(name)); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) { (void)req; assert(type); return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status) { req->status=status; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *name,const char *content) { (void)req; assert(name && content); return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *text) { (void)req; assert(strlen(text)<sizeof(output)); strcpy(output,text); return ESP_OK; }
int httpd_req_recv(httpd_req_t *req,char *out,size_t count)
{ (void)req; if(recv_failure)return -1; if(count>13)count=13; memcpy(out,input+offset,count);offset+=count;return count; }
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *uri) { assert(handle && uri->handler); return ++registrations==register_failure ? ESP_FAIL : ESP_OK; }
static void reset(void)
{
 fixture=(passport_wifi_state_t){.initialized=true,.storage_known=true,.ap_started=true,.dhcp_running=true,.setup_open=true,.setup_id=17,.setup_seconds=120,.scan_known=true,.scan_revision=4,.scan_count=1,.ap_ip="192.168.4.1"};
 fixture.rows[0]=(passport_wifi_row_t){.ssid={.bytes="Test LAN",.length=8},.security=PW_WPA2,.rssi=-55};
 queue_available=token_available=true; host=local_ip="192.168.4.1"; origin="http://192.168.4.1"; content_type="application/json"; request_token=session_token;
 input="{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"fixture123\"}";
 station_result=ESP_ERR_WIFI_NOT_CONNECT; submissions=scan_requests=offset=0; clock_us=0; recv_failure=0;
 output[0]=0; allocation=fail_allocation=0;
}
static void submit_expect(const char *status,unsigned count)
{
 httpd_req_t req={.content_len=strlen(input),.status="200 OK"}; offset=0;
 assert(submit_post(&req)==ESP_FAIL && !strcmp(req.status,status)); assert(submissions==count);
 assert(!strstr(output,"fixture123") && !strstr(output,session_token));
 for(size_t i=0;i<sizeof(body);++i) assert(!body[i]);
}
static void test_boundaries(void)
{
 reset(); submit_expect("202 Accepted",1); assert(submitted.request_id==123456 && !strcmp(submitted.password,"fixture123")); assert(strstr(output,"queued"));
 reset(); local_ip="192.168.1.20"; submit_expect("403 Forbidden",0);
 reset(); station_result=ESP_OK; submit_expect("403 Forbidden",0);
 reset(); station_result=ESP_FAIL; submit_expect("403 Forbidden",0);
 reset(); host="attacker.test"; submit_expect("403 Forbidden",0);
 reset(); origin="http://attacker.test"; submit_expect("403 Forbidden",0);
 reset(); request_token="wrong"; submit_expect("403 Forbidden",0);
 reset(); fixture.setup_open=false; submit_expect("403 Forbidden",0);
 reset(); token_available=false; submit_expect("403 Forbidden",0);
 reset(); fixture.dhcp_running=false; submit_expect("403 Forbidden",0);
 reset(); content_type="text/plain"; submit_expect("415 Unsupported Media Type",0);
 reset(); fixture.scan_revision=5; submit_expect("409 Conflict",0);
 reset(); fixture.pending=true; submit_expect("409 Conflict",0);
 reset(); fixture.profile_count=4; submit_expect("409 Conflict",0);
 reset(); fixture.profile_count=4; fixture.rows[0].profile_id=1; submit_expect("202 Accepted",1);
 assert(submitted.row==0 && submitted.scan_revision==fixture.scan_revision);
 reset(); fixture.storage_known=false; submit_expect("409 Conflict",0);
 reset(); queue_available=false; submit_expect("503 Service Unavailable",1);
 reset(); recv_failure=1; submit_expect("408 Request Timeout",0);
 reset(); input="{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"short\"}"; submit_expect("400 Bad Request",0);
 reset(); fixture.rows[0].security=PW_UNSUPPORTED; submit_expect("400 Bad Request",0);
 reset(); fixture.rows[0].ssid.length=0; submit_expect("400 Bad Request",0);
 reset(); fixture.rows[0].security=PW_OPEN; input="{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"\"}"; submit_expect("202 Accepted",1);
 reset(); origin=""; host="192.168.4.1:80"; submit_expect("202 Accepted",1);
}
static void test_parser(void)
{
 const char *invalid[]={"{}","[]","{\"setupId\":17,\"setupId\":17}","{\"setupId\":{\"x\":1}}","{\"setupId\":-1}","{\"setupId\":17.5}","{\"setupId\":4294967296}",
  "{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"fixture123\\u0000evil\"}",
  "{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"fixture123\",\"extra\":1}",
  "{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"fixture123\",\"password\":\"second123\"}",
  "{\"setupId\":17,\"scanRevision\":4,\"row\":0,\"password\":\"fixture123\"} garbage"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){reset();input=invalid[i];submit_expect("400 Bad Request",0);}
 char huge[514];memset(huge,'x',sizeof(huge));huge[513]=0;reset();input=huge;submit_expect("413 Content Too Large",0);
 reset(); input="{\"setupId\":17}"; httpd_req_t req={.content_len=strlen(input)}; assert(scan_post(&req)==ESP_FAIL && !strcmp(req.status,"202 Accepted") && scan_requests==1 && !submissions);
}
static void test_state_and_allocation(void)
{
 reset(); fixture.scan_count=PASSPORT_WIFI_SCAN_ROWS;
 for(unsigned i=0;i<PASSPORT_WIFI_SCAN_ROWS;++i){fixture.rows[i]=fixture.rows[0];fixture.rows[i].ssid.length=32;memset(fixture.rows[i].ssid.bytes,0xff,32);}
 httpd_req_t req={0}; assert(state_get(&req)==ESP_OK); assert(strlen(output)<sizeof(response));
 cJSON *json=cJSON_Parse(output);assert(json && cJSON_GetArraySize(cJSON_GetObjectItem(json,"rows"))==16);cJSON_Delete(json);
 assert(strstr(output,session_token) && !strstr(output,"password"));
 fixture.setup_open=false; assert(state_get(&req)==ESP_OK && !strstr(output,session_token));
 cJSON_Hooks hooks={allocate,free};cJSON_InitHooks(&hooks);
 for(unsigned i=1;i<300;++i){allocation=0;fail_allocation=i;req.status="200 OK";assert(state_get(&req)==ESP_OK);assert(!strcmp(req.status,"503 Service Unavailable") || !strcmp(req.status,"200 OK"));}
 cJSON_InitHooks(NULL);
 for(unsigned i=1;i<=6;++i){register_failure=i;registrations=0;assert(passport_setup_register((void *)1)==ESP_FAIL && !provider);}
 register_failure=registrations=0;assert(passport_setup_register((void *)1)==ESP_OK && provider && registrations==6);
 reset(); assert(html_get(&req)==ESP_OK && strstr(output,"doctype"));local_ip="192.168.1.20";assert(html_get(&req)==ESP_OK && !strcmp(req.status,"403 Forbidden"));
}
int main(void){test_boundaries();test_parser();test_state_and_allocation();puts("PASS: AP ingress, Host/Origin/token/window, bounded strict JSON, scan/submit queue receipts, secret redaction, full scan response, allocation and route failure");}
