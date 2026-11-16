// Exercise the real open protocol and management adapter. SDK radio/storage
// owners are explicit fixtures; their implementations have separate suites.
#define main shared_input_cases
#include "test_control.c"
#undef main
#include "../main/passport_management.h"
#include "../main/passport_ble.h"
#include "../main/passport_wifi.h"
#include "control_registry.h"
static passport_ble_state_t fixture_ble;
static passport_wifi_state_t fixture_wifi;
static passport_name_entry_t retained[2][20];
static passport_name_catalog_t catalog[2];
static passport_name_change_t submitted;
static passport_name_result_t outcome;
static passport_wifi_command_t scan_request;
static unsigned name_submits, ble_submits;
void passport_ble_snapshot(passport_ble_state_t *s) { *s=fixture_ble; }
void passport_wifi_snapshot(passport_wifi_state_t *s) { *s=fixture_wifi; }
bool passport_wifi_request(const passport_wifi_command_t *c) { scan_request=*c;return true; }
cp_error_t passport_ble_remote_begin(const cp_command_t *c,uint64_t now,cp_result_t *out)
{ (void)now;++ble_submits;*out=(cp_result_t){.command_key=c->command_key,.state=CP_RESULT_APPLIED};return CP_OK; }
bool passport_ble_remote_poll(uint64_t now,cp_result_t *out) { (void)now;(void)out;return false; }
passport_name_catalog_t passport_names_catalog(passport_name_domain_t d) { return catalog[d]; }
passport_name_result_t passport_names_entry(passport_name_domain_t d,uint32_t revision,unsigned index,passport_name_entry_t *out)
{ if(revision!=catalog[d].revision)return PN_STALE;if(index>=catalog[d].count)return PN_NOT_FOUND;*out=retained[d][index];return PN_OK; }
bool passport_names_equal(const passport_name_key_t *a,const passport_name_key_t *b)
{
    if(a->kind!=b->kind)return false;
    if(a->kind==PN_HID_BOND)return !memcmp(a->bond,b->bond,16);
    if(a->kind==PN_WIFI_PROFILE)return a->profile==b->profile;
    return a->ap.length==b->ap.length&&a->ap.auth==b->ap.auth&&!memcmp(a->ap.ssid,b->ap.ssid,a->ap.length)&&!memcmp(a->ap.bssid,b->ap.bssid,6);
}
passport_name_result_t passport_names_lookup(const passport_name_key_t *key,char *out)
{
    passport_name_domain_t d=key->kind==PN_HID_BOND?PN_BLE:PN_WIFI;out[0]=0;
    if(!catalog[d].known)return PN_UNAVAILABLE;
    for(unsigned i=0;i<catalog[d].count;++i)if(passport_names_equal(key,&retained[d][i].key)){strcpy(out,retained[d][i].label);return PN_OK;}
    return PN_NOT_FOUND;
}
bool passport_names_text_valid(const char *s,size_t n)
{ if(!n||n>16)return false;for(size_t i=0;i<n;++i)if((unsigned char)s[i]<32||(unsigned char)s[i]>126)return false;return true; }
passport_name_result_t passport_names_submit(const passport_name_change_t *c,uint32_t *ticket)
{ submitted=*c;*ticket=++name_submits;outcome=PN_PENDING;return PN_OK; }
passport_name_result_t passport_names_result(passport_name_domain_t d,uint32_t ticket)
{ (void)d;assert(ticket==name_submits);return outcome; }
void explicit_bzero(void *data,size_t n) { volatile unsigned char *p=data;while(n--)*p++=0; }
static cp_error_t manage(const char *op,const char *args,uint64_t resource)
{
    char body[2049];++seq;
    snprintf(body,sizeof(body),"{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016x\",\"seq\":%u,\"deadline_ms\":\"%llu\",\"management_generation\":\"1\",\"resource_revision\":\"%llu\",\"op\":\"%s\",\"args\":%s}",boot,session,prefix,seq,seq,(unsigned long long)clock_ms+1500,(unsigned long long)resource,op,args);
    return http("POST","/api/v1/commands",body);
}
static void result_key(char *path)
{ parse();char key[24];field(0,"command_key",key,sizeof(key));snprintf(path,64,"/api/v1/requests/%s",key); }
int main(void)
{
    setup();passport_management_install();
    fixture_ble=(passport_ble_state_t){.synced=true,.storage_known=true,.bond_revision=7,.generation=3,.peer_count=1};
    strcpy(fixture_ble.peers[0].label,"00:00:00:00:00:01/0");fixture_ble.peers[0].bond_id[0]=1;
    fixture_wifi=(passport_wifi_state_t){.initialized=true,.storage_known=true,.revision=4,.scan_known=true,.scan_revision=9,.profile_count=1,.scan_count=16};
    fixture_wifi.profiles[0]=(passport_wifi_profile_t){.id=12,.ssid={.bytes="Cafe",.length=4},.security=PW_WPA2};
    for(unsigned i=0;i<16;++i) fixture_wifi.rows[i]=(passport_wifi_row_t){.ssid={.bytes="<script>",.length=8},.bssid={1,2,3,4,5,i},.auth=3,.security=PW_WPA2,.rssi=-40};
    catalog[0]=(passport_name_catalog_t){.known=true,.revision=2};catalog[1]=(passport_name_catalog_t){.known=true,.revision=3,.count=16};
    for(unsigned i=0;i<16;++i){retained[1][i].key=passport_wifi_ap_name_key(&fixture_wifi.rows[i]);strcpy(retained[1][i].label,"Duplicate name");}
    assert(http("GET","/api/v1/bluetooth/peers",NULL)==CP_OK);assert(strstr(request.response.body,"01000000000000000000000000000000"));
    assert(http("GET","/api/v1/wifi/profiles",NULL)==CP_OK);assert(strstr(request.response.body,"0000000c"));
    assert(http("GET","/api/v1/wifi/scans/current",NULL)==CP_OK);parse();int64_t value;assert(cj_integer(&document,cj_get(&document,0,"next_cursor"),&value)&&value==4);
    assert(http("GET","/api/v1/wifi/scans/current?cursor=12",NULL)==CP_OK);assert(strstr(request.response.body,"\"next_cursor\":0"));
    assert(http("GET","/api/v1/display-names?cursor=8",NULL)==CP_OK);parse();uint16_t rows=cj_get(&document,0,"labels");char id[65];field(document.nodes[rows].child,"id",id,sizeof(id));
    assert(http("GET","/api/v1/display-names?cursor=99",NULL)==CP_INVALID);
    uint64_t rev=((uint64_t)2<<32)|3; char args[256],path[64];
    snprintf(args,sizeof(args),"{\"kind\":\"wifi_ap\",\"id\":\"%s\"}",id);
    assert(manage("display_name.clear",args,rev-1)==CP_STALE);assert(!name_submits);
    assert(manage("display_name.clear",args,rev)==CP_OK);result_key(path);assert(name_submits==1&&submitted.clear&&submitted.key.ap.bssid[5]==8);
    assert(http("GET",path,NULL)==CP_OK);assert(strstr(request.response.body,"accepted"));outcome=PN_UNCERTAIN;tick();
    assert(http("GET",path,NULL)==CP_STORAGE_UNCERTAIN);assert(strstr(request.response.body,"label_storage_uncertain"));
    snprintf(args,sizeof(args),"{\"kind\":\"hid_bond\",\"id\":\"01000000000000000000000000000000\",\"owner_revision\":\"7\",\"label\":\"Desk\"}");
    assert(manage("display_name.set",args,rev)==CP_OK);result_key(path);outcome=PN_OK;tick();assert(http("GET",path,NULL)==CP_OK);assert(strstr(request.response.body,"applied"));
    assert(!strcmp(submitted.label,"Desk")&&submitted.key.bond[0]==1);
    assert(manage("wifi.scan.start","{}",8)==CP_STALE);
    assert(manage("wifi.scan.start","{}",9)==CP_OK);result_key(path);assert(scan_request.remote_scan_id&&scan_request.revision==9);
    fixture_wifi.scan_request_id=scan_request.remote_scan_id;fixture_wifi.scan_request_result=PW_SCAN_LOCKED;tick();
    assert(http("GET",path,NULL)==CP_LOCKED);
    assert(manage("bluetooth.target.request","{\"peer_id\":\"fixture\",\"generation\":\"3\"}",7)==CP_OK&&ble_submits==1);
    assert(manage("bluetooth.bond.forget","{\"peer_id\":\"fixture\"}",7)==CP_FORBIDDEN);
    assert(manage("wifi.profile.forget","{\"profile_id\":\"fixture\"}",4)==CP_FORBIDDEN);
    hid_engine_set_locked(&input,true);tick();assert(manage("display_name.clear",args,rev)==CP_LOCKED);
    catalog[1].known=false;assert(http("GET","/api/v1/display-names",NULL)==CP_OK);assert(strstr(request.response.body,"\"known\":false"));
    passport_control_deinit();puts("PASS: real open protocol/management adapter, typed identities, paging, stale revisions, async owner results, storage uncertainty, Lock and narrow policy");
}
