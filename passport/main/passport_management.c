// SPDX-License-Identifier: Apache-2.0
#include "passport_management.h"
#include "passport_ble.h"
#include "passport_wifi.h"
#include "passport_names.h"
#include "control_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One NimBLE task owns these copied snapshots and response workspace. Network
// handlers never borrow owner records or perform storage/radio operations.
static passport_ble_state_t ble;
static passport_wifi_state_t wifi;
static passport_name_entry_t entry;
static passport_name_change_t change;
static struct { uint64_t key; uint32_t ticket; } names[2];
static struct { uint64_t key, deadline; bool cancel; } scan;
static passport_name_catalog_t catalogs[2];
static struct { char *data; size_t capacity, used; bool good; } json;
static void add(const char *format, ...) __attribute__((format(printf,1,2)));
static void add(const char *format, ...)
{
    if (!json.good) return;
    va_list args; va_start(args,format);
    int n=vsnprintf(json.data+json.used,json.capacity-json.used,format,args); va_end(args);
    if (n<0 || (size_t)n>=json.capacity-json.used) json.good=false;
    else json.used+=(size_t)n;
}
static void string(const char *value)
{
    char escaped[65*6+3];
    if (!cj_escape(value,escaped,sizeof(escaped))) { json.good=false; return; }
    add("%s",escaped);
}
static void hex(const uint8_t *bytes, size_t size, char *out)
{ for (size_t i=0;i<size;++i) snprintf(out+2*i,3,"%02x",bytes[i]); out[2*size]=0; }
static void id(const passport_name_key_t *key, char out[65])
{
    if (key->kind==PN_HID_BOND) hex(key->bond,16,out);
    else if (key->kind==PN_WIFI_PROFILE) snprintf(out,65,"%08x",(unsigned)key->profile);
    else {
        uint8_t bytes[41]={key->ap.length,0,key->ap.auth}, hash[32];
        memcpy(bytes+3,key->ap.bssid,6); memcpy(bytes+9,key->ap.ssid,key->ap.length);
        cp_sha256(bytes,9+key->ap.length,hash); hex(hash,sizeof(hash),out);
    }
}
static const char *kind(passport_name_kind_t k)
{ return k==PN_HID_BOND ? "hid_bond" : k==PN_WIFI_PROFILE ? "wifi_profile" : "wifi_ap"; }
static passport_name_key_t bond_key(unsigned i)
{
    passport_name_key_t key={.kind=PN_HID_BOND}; memcpy(key.bond,ble.peers[i].bond_id,16); return key;
}
static bool labelable_bond(const passport_name_key_t *key)
{ for (unsigned i=0;i<16;++i) if (key->bond[i]) return true; return false; }
static void refresh(void)
{
    passport_ble_snapshot(&ble); passport_wifi_snapshot(&wifi);
    catalogs[PN_BLE]=passport_names_catalog(PN_BLE); catalogs[PN_WIFI]=passport_names_catalog(PN_WIFI);
}
static uint64_t revision(void)
{ return ((uint64_t)catalogs[PN_BLE].revision<<32)|catalogs[PN_WIFI].revision; }
static void label(const passport_name_key_t *key)
{
    char text[PASSPORT_NAME_BYTES+1]={0}; passport_name_result_t e=passport_names_lookup(key,text);
    add(",\"label_known\":%s,\"label\":",e==PN_OK || e==PN_NOT_FOUND ? "true" : "false");
    string(e==PN_OK ? text : "");
}
static bool cursor(const char *path,const char *root,unsigned max,unsigned *out)
{
    size_t n=strlen(root); if (strncmp(path,root,n)) return false;
    const char *tail=path+n;
    if (!*tail) { *out=0; return true; }
    if (strncmp(tail,"?cursor=",8)) return false;
    tail+=8; if (!*tail) return false;
    unsigned value=0;
    while (*tail) { if (*tail<'0' || *tail>'9' || value>max/10+1) return false; value=value*10+(unsigned)(*tail++-'0'); }
    if (value>max) return false;
    *out=value; return true;
}
static cp_error_t read(const char *path,char *out,size_t capacity,size_t *length)
{
    refresh(); json.data=out; json.capacity=capacity; json.used=0; json.good=true;
    unsigned start=0;
    if (!strcmp(path,"/api/v1/bluetooth/peers")) {
        add("{\"known\":%s,\"revision\":\"%u\",\"generation\":\"%u\",\"updating\":%s,\"connected\":%s,\"encrypted\":%s,\"bonded\":%s,\"keyboard_subscribed\":%s,\"mouse_subscribed\":%s,\"ready\":%s,\"pairing\":%s,\"handshake\":%s,\"advertising\":%s,\"window_seconds\":%u,\"selected_id\":\"%s\",\"current\":",
            ble.storage_known?"true":"false",(unsigned)ble.bond_revision,(unsigned)ble.generation,ble.updating?"true":"false",ble.connected?"true":"false",ble.encrypted?"true":"false",ble.bonded?"true":"false",ble.keyboard_subscribed?"true":"false",ble.mouse_subscribed?"true":"false",ble.ready?"true":"false",ble.pairing?"true":"false",ble.handshake?"true":"false",ble.advertising?"true":"false",ble.window_seconds,ble.selected_id);
        string(ble.current); add(",\"message\":"); string(ble.result); add(",\"capacity\":3,\"peers\":[");
        for (unsigned i=0;ble.storage_known && i<ble.peer_count;++i) {
            passport_name_key_t key=bond_key(i); char identity[65]; id(&key,identity);
            add("%s{\"id\":\"%s\",\"labelable\":%s,\"current\":%s,\"original\":",i?",":"",identity,labelable_bond(&key)?"true":"false",ble.connected && !strcmp(ble.current,ble.peers[i].label)?"true":"false");
            string(ble.peers[i].label); label(&key); add("}");
        }
        add("]}");
    } else if (!strcmp(path,"/api/v1/wifi/profiles")) {
        add("{\"known\":%s,\"revision\":\"%u\",\"phase\":%u,\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"ap_active\":%s,\"capacity\":4,\"message\":",wifi.storage_known?"true":"false",(unsigned)wifi.revision,wifi.phase,wifi.sta_ip,wifi.ap_ip,wifi.ap_started && wifi.dhcp_running?"true":"false");
        string(wifi.result); add(",\"profiles\":[");
        for (unsigned i=0;wifi.storage_known && i<wifi.profile_count;++i) {
            const passport_wifi_profile_t *p=&wifi.profiles[i]; passport_name_key_t key={.kind=PN_WIFI_PROFILE,.profile=p->id};
            char identity[65],ssid[65]; id(&key,identity); hex(p->ssid.bytes,p->ssid.length,ssid);
            add("%s{\"id\":\"%s\",\"ssid_hex\":\"%s\",\"security\":%u,\"current\":%s",i?",":"",identity,ssid,p->security,wifi.phase==PW_ONLINE && p->id==wifi.active_profile?"true":"false");
            label(&key); add("}");
        } add("]}");
    } else if (cursor(path,"/api/v1/wifi/scans/current",PASSPORT_WIFI_SCAN_ROWS,&start)) {
        unsigned end=start+4; if (end>wifi.scan_count) end=wifi.scan_count;
        add("{\"known\":%s,\"scanning\":%s,\"generation\":%u,\"truncated\":%s,\"next_cursor\":%u,\"records\":[",wifi.scan_known?"true":"false",wifi.scanning?"true":"false",(unsigned)wifi.scan_revision,wifi.scan_truncated?"true":"false",wifi.scan_known && end<wifi.scan_count ? end : 0);
        for (unsigned i=start;wifi.scan_known && i<end;++i) {
            const passport_wifi_row_t *r=&wifi.rows[i]; passport_name_key_t key=passport_wifi_ap_name_key(r);
            char identity[65],ssid[65],bssid[13],name[49]={0}; id(&key,identity); hex(r->ssid.bytes,r->ssid.length,ssid); hex(r->bssid,6,bssid);
            passport_name_result_t e=passport_wifi_scan_label(&wifi,r,name); uint32_t profile=0;
            unsigned matches=passport_wifi_profile_matches(&wifi,r,&profile);
            add("%s{\"id\":\"%s\",\"index\":%u,\"ssid_hex\":\"%s\",\"bssid\":\"%s\",\"auth\":%u,\"security\":%u,\"rssi\":%d,\"current\":%s,\"saved_matches\":%u,\"labelable\":%s,\"label_known\":%s,\"label\":",i>start?",":"",identity,i,ssid,bssid,r->auth,r->security,r->rssi,r->current?"true":"false",matches,r->ssid.length && !matches?"true":"false",e==PN_OK || e==PN_NOT_FOUND?"true":"false");
            string(e==PN_OK ? name : ""); add("}");
        } add("]}");
    } else if (cursor(path,"/api/v1/display-names",23,&start)) {
        bool known=catalogs[0].known && catalogs[1].known;
        unsigned total=catalogs[0].count+catalogs[1].count,end=start+8; if (end>total) end=total;
        add("{\"known\":%s,\"revision\":\"%llu\",\"count\":%u,\"hid_capacity\":3,\"profile_capacity\":4,\"ap_capacity\":16,\"next_cursor\":%u,\"labels\":[",known?"true":"false",(unsigned long long)revision(),total,known && end<total?end:0);
        for (unsigned i=start;known && i<end;++i) {
            passport_name_domain_t d=i<catalogs[0].count ? PN_BLE : PN_WIFI;
            unsigned index=d==PN_BLE?i:i-catalogs[0].count;
            if (passport_names_entry(d,catalogs[d].revision,index,&entry)!=PN_OK) return CP_STALE;
            char identity[65]; id(&entry.key,identity);
            add("%s{\"kind\":\"%s\",\"id\":\"%s\",\"label\":",i>start?",":"",kind(entry.key.kind),identity); string(entry.label);
            if (entry.key.kind==PN_SCAN_AP) { char ssid[65]; hex(entry.key.ap.ssid,entry.key.ap.length,ssid); add(",\"ssid_hex\":\"%s\"",ssid); }
            add("}");
        } add("]}");
    } else return CP_INVALID;
    if (!json.good) return CP_TOO_LARGE;
    *length=json.used; return CP_OK;
}
static cp_error_t name_error(passport_name_result_t e)
{
    switch (e) {
    case PN_OK:return CP_OK; case PN_STALE:case PN_NOT_FOUND:return CP_STALE;
    case PN_INVALID:return CP_INVALID; case PN_LOCKED:return CP_LOCKED;
    case PN_FULL:case PN_BUSY:return CP_BUSY; case PN_UNCERTAIN:return CP_STORAGE_UNCERTAIN;
    case PN_RESULT_UNKNOWN:return CP_RESULT_UNKNOWN; default:return CP_UNAVAILABLE;
    }
}
static bool available(cp_operation_id_t op,char reason[CP_REASON_CAPACITY])
{
    switch(op) {
    case CP_OP_BLUETOOTH_PEERS_LIST: case CP_OP_WIFI_PROFILES_LIST: case CP_OP_WIFI_SCAN_RESULTS:
    case CP_OP_DISPLAY_NAME_LIST: case CP_OP_DISPLAY_NAME_SET: case CP_OP_DISPLAY_NAME_CLEAR:
    case CP_OP_BLUETOOTH_PAIRING_OPEN: case CP_OP_BLUETOOTH_PAIRING_CLOSE: case CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE:
    case CP_OP_BLUETOOTH_TARGET_REQUEST: case CP_OP_BLUETOOTH_DISCONNECT: case CP_OP_WIFI_SCAN_START: case CP_OP_WIFI_SCAN_CANCEL:return true;
    default:snprintf(reason,CP_REASON_CAPACITY,"provider_unavailable");return false;
    }
}
static cp_error_t name_command(const cp_command_t *c,cp_result_t *out)
{
    char wanted[65],type[20],owner[24];
    if (!cp_command_string(c,"kind",type,sizeof(type)) || !cp_command_string(c,"id",wanted,sizeof(wanted))) return CP_INVALID;
    if (strcmp(type,"hid_bond") && strcmp(type,"wifi_profile") && strcmp(type,"wifi_ap")) return CP_UNAVAILABLE;
    passport_name_domain_t d=!strcmp(type,"hid_bond") ? PN_BLE : PN_WIFI;
    if (names[d].key) return CP_BUSY;
    if (!catalogs[0].known || !catalogs[1].known) return CP_UNAVAILABLE;
    if (c->resource_revision!=revision()) return CP_STALE;
    memset(&change,0,sizeof(change)); change.clear=c->operation==CP_OP_DISPLAY_NAME_CLEAR; change.revision=catalogs[d].revision;
    bool found=false;
    if (change.clear) {
        for (unsigned i=0;i<catalogs[d].count;++i) {
            if (passport_names_entry(d,catalogs[d].revision,i,&entry)!=PN_OK) return CP_STALE;
            char identity[65]; id(&entry.key,identity);
            if (!strcmp(kind(entry.key.kind),type) && !strcmp(identity,wanted)) { change.key=entry.key; found=true; break; }
        }
    } else if (!strcmp(type,"wifi_ap")) {
        int64_t generation,index;
        if (!cp_command_integer(c,"scan_generation",&generation) || !cp_command_integer(c,"scan_index",&index) || index<0 || index>=PASSPORT_WIFI_SCAN_ROWS) return CP_INVALID;
        if (!wifi.scan_known || wifi.scanning || generation!=wifi.scan_revision || (unsigned)index>=wifi.scan_count) return CP_STALE;
        change.key=passport_wifi_ap_name_key(&wifi.rows[index]); char identity[65]; id(&change.key,identity);
        uint32_t ignored;
        if (!change.key.ap.length || passport_wifi_profile_matches(&wifi,&wifi.rows[index],&ignored)) return CP_INVALID;
        found=!strcmp(identity,wanted); change.scan_revision=wifi.scan_revision;
    } else {
        if (!cp_command_string(c,"owner_revision",owner,sizeof(owner))) return CP_INVALID;
        char actual[24]; snprintf(actual,sizeof(actual),"%u",(unsigned)(d==PN_BLE ? ble.bond_revision : wifi.revision));
        if (strcmp(owner,actual) || (d==PN_BLE ? !ble.storage_known : !wifi.storage_known)) return CP_STALE;
        unsigned count=d==PN_BLE ? ble.peer_count : wifi.profile_count;
        for (unsigned i=0;i<count;++i) {
            passport_name_key_t key=d==PN_BLE ? bond_key(i) : (passport_name_key_t){.kind=PN_WIFI_PROFILE,.profile=wifi.profiles[i].id};
            char identity[65]; id(&key,identity);
            if (!strcmp(identity,wanted) && (d!=PN_BLE || labelable_bond(&key))) { change.key=key; found=true; break; }
        }
    }
    if (!found) return CP_STALE;
    if (!change.clear && (!cp_command_string(c,"label",change.label,sizeof(change.label)) || !passport_names_text_valid(change.label,strlen(change.label)))) return CP_INVALID;
    uint32_t ticket; cp_error_t e=name_error(passport_names_submit(&change,&ticket));
    explicit_bzero(&change,sizeof(change));
    if (e!=CP_OK) return e;
    names[d].key=c->command_key; names[d].ticket=ticket;
    *out=(cp_result_t){.state=CP_RESULT_ACCEPTED,.command_key=c->command_key,.uncertain_index=-1};
    strcpy(out->reason,"waiting_for_label_storage"); return CP_OK;
}
static cp_error_t dispatch(const cp_command_t *c,uint64_t now,cp_result_t *out)
{
    refresh();
    if (ble.locked) return CP_LOCKED;
    if (c->operation==CP_OP_DISPLAY_NAME_SET || c->operation==CP_OP_DISPLAY_NAME_CLEAR) return name_command(c,out);
    if (c->operation==CP_OP_WIFI_SCAN_START || c->operation==CP_OP_WIFI_SCAN_CANCEL) {
        if (scan.key) return CP_BUSY;
        if (!wifi.initialized) return CP_UNAVAILABLE;
        if (c->resource_revision!=wifi.scan_revision) return CP_STALE;
        passport_wifi_command_t request={.action=c->operation==CP_OP_WIFI_SCAN_START?PW_SCAN:PW_SCAN_CANCEL,
            .revision=(unsigned)wifi.scan_revision,.remote_scan_id=c->command_key};
        if (!passport_wifi_request(&request)) return CP_BUSY;
        scan.key=c->command_key; scan.deadline=now+8000; scan.cancel=request.action==PW_SCAN_CANCEL;
        *out=(cp_result_t){.state=CP_RESULT_ACCEPTED,.command_key=c->command_key,.uncertain_index=-1}; return CP_OK;
    }
    return passport_ble_remote_begin(c,now,out);
}
static bool poll(uint64_t now,cp_result_t *out)
{
    if (passport_ble_remote_poll(now,out)) return true;
    for (unsigned d=0;d<2;++d) if (names[d].key) {
        passport_name_result_t e=passport_names_result(d,names[d].ticket); if (e==PN_PENDING) continue;
        refresh(); cp_error_t error=name_error(e);
        *out=(cp_result_t){.state=error==CP_OK?CP_RESULT_APPLIED:CP_RESULT_FAILED,.error=error,
            .command_key=names[d].key,.resource_revision=revision(),.uncertain_index=-1};
        snprintf(out->reason,sizeof(out->reason),"%s",error==CP_OK?"label_write_confirmed":error==CP_STORAGE_UNCERTAIN?"label_storage_uncertain":"label_change_not_confirmed");
        names[d].key=0; return true;
    }
    if (scan.key) {
        passport_wifi_snapshot(&wifi);
        if (wifi.scan_request_id!=scan.key && wifi.initialized && now<scan.deadline) return false;
        cp_error_t error=CP_RESULT_UNKNOWN;
        if (wifi.scan_request_id==scan.key) {
            switch(wifi.scan_request_result) {
            case PW_SCAN_ACCEPTED:error=CP_OK;break;case PW_SCAN_STALE:error=CP_STALE;break;
            case PW_SCAN_LOCKED:error=CP_LOCKED;break;case PW_SCAN_BUSY:error=CP_BUSY;break;default:error=CP_UNAVAILABLE;break;
            }
        }
        *out=(cp_result_t){.command_key=scan.key,.state=error==CP_OK?CP_RESULT_APPLIED:CP_RESULT_FAILED,.error=error,.uncertain_index=-1};
        snprintf(out->reason,sizeof(out->reason),"%s",error!=CP_OK?"scan_request_not_confirmed":scan.cancel?"scan_cancelled":"scan_started");
        scan.key=0; return true;
    }
    return false;
}
void passport_management_install(void)
{
    static const passport_control_provider_t provider={.available=available,.dispatch=dispatch,.read=read,.poll=poll};
    memset(names,0,sizeof(names)); memset(&scan,0,sizeof(scan)); passport_control_set_provider(&provider);
}
