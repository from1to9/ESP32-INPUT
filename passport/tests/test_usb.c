#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "usb_stubs.h"
static ssize_t fixture_read(int, void *, size_t);
static ssize_t fixture_write(int, const void *, size_t);
static int fixture_fcntl(int, int, ...);
#define read fixture_read
#define write fixture_write
#define fcntl fixture_fcntl
#include "../main/passport_usb.c"
#undef read
#undef write
#undef fcntl
#include "../main/passport_usb_protocol.c"

static char rx[1024], tx[8192];
static size_t rx_size, rx_at, tx_size;
static unsigned ble_calls, wifi_calls, join_calls, theme_calls, name_calls, frames;
static bool connected = true, queue_ok = true;
static uint64_t clock_ms = 1000;
static passport_ble_state_t observed_ble;
static passport_wifi_state_t observed_wifi;
static passport_command_t ble_request;
static passport_wifi_command_t wifi_request;
static passport_wifi_submission_t submitted;
static passport_name_change_t name_change;
static passport_sound_state_t sound_state;
static esp_err_t theme_error;
static passport_name_result_t name_outcome = PN_PENDING;

// Execute the pinned VFS itself. Only the FIFO, driver ring and lock boundaries
// are substituted; no driver is installed in the production configuration.
typedef unsigned UBaseType_t;
static struct { void *rx_ring_buf; } driver_fixture, *p_usb_serial_jtag_obj;
static struct { int peek_char, read_lock, rx_mode; bool non_blocking; int (*rx_func)(int); } s_ctx;
enum { NONE=-1, USJ_LOCAL_FD=0, ESP_LINE_ENDINGS_LF, ESP_LINE_ENDINGS_CR, ESP_LINE_ENDINGS_CRLF };
#define MIN(a,b) ((a)<(b)?(a):(b))
static unsigned fifo_reads;
static int usb_serial_jtag_ll_read_rxfifo(uint8_t *out, size_t size)
{ assert(size==1); ++fifo_reads; if(rx_at==rx_size)return 0;*out=(uint8_t)rx[rx_at++];return 1; }
static void _lock_acquire_recursive(int *lock) { ++*lock; }
static void _lock_release_recursive(int *lock) { assert(*lock>0); --*lock; }
static void vRingbufferGetInfo(void *ring, void *a, void *b, void *c, void *d, UBaseType_t *bytes)
{ (void)ring;(void)a;(void)b;(void)c;(void)d;*bytes=0; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "sdk_usb_rx.inc"
#pragma GCC diagnostic pop

bool usb_serial_jtag_is_connected(void) { return connected; }
uint32_t esp_random(void) { return 0x12345678; }
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t app={.version="fixture"}; return &app; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static int fixture_fcntl(int fd, int cmd, ...)
{
    assert(fd==0&&(cmd==F_GETFL||cmd==F_SETFL));int arg=0;
    if(cmd==F_SETFL){va_list args;va_start(args,cmd);arg=va_arg(args,int);va_end(args);}
    return usb_serial_jtag_fcntl(fd,cmd,arg);
}
static ssize_t fixture_read(int fd, void *out, size_t size)
{ assert(fd == 0 && size == 1); return usb_serial_jtag_read(fd,out,size); }
static ssize_t fixture_write(int fd, const void *bytes, size_t size)
{
    const char *p = bytes;
    assert(fd == 1 && size < PUSB_FRAME_CAPACITY && size > 7 && p[0] == 30 && p[size-1] == '\n');
    uint32_t crc; assert(p[size-6] == '*' && hex_number(p+size-5, 4, &crc));
    assert(crc == passport_usb_crc(p+1, size-7));
    assert(tx_size + size < sizeof(tx)); memcpy(tx+tx_size, bytes, size); tx_size += size; tx[tx_size] = 0; ++frames;
    return (ssize_t)size;
}
bool passport_ble_request(const passport_command_t *c) { ++ble_calls; ble_request = *c; return queue_ok; }
bool passport_wifi_request(const passport_wifi_command_t *c) { ++wifi_calls; wifi_request = *c; return queue_ok; }
bool passport_wifi_submit_usb(const passport_wifi_submission_t *s) { ++join_calls; submitted = *s; return queue_ok; }
void passport_wifi_ap_credentials(char ssid[32], char password[17]) { strcpy(ssid, "Passport-ABCD"); strcpy(password, "0123456789ABCDEF"); }
const char *passport_wifi_phase_name(passport_wifi_phase_t p) { (void)p; return "associated"; }
esp_err_t passport_ui_set_theme(passport_theme_t t) { assert(t <= PASSPORT_LIGHT); ++theme_calls; return theme_error; }
passport_sound_state_t passport_sound_state(void) { return sound_state; }
esp_err_t passport_sound_set(bool enabled) { sound_state.enabled = enabled; sound_state.output = PASSPORT_SOUND_STARTING; return 0; }
esp_err_t passport_ui_set_sound(bool enabled) { return passport_sound_set(enabled); }
bool passport_sound_cue_usb(passport_cue_t cue, uint32_t id) { assert(cue >= 1 && cue <= 3); sound_state.cue = (passport_receipt_t){id,PR_PENDING,0}; return true; }
passport_name_catalog_t passport_names_catalog(passport_name_domain_t d) { (void)d; return (passport_name_catalog_t){.known=true,.revision=UINT32_MAX}; }
passport_name_result_t passport_names_result(passport_name_domain_t d, uint32_t ticket) { assert(d <= PN_WIFI && ticket == 9); return name_outcome; }
passport_name_result_t passport_names_submit(const passport_name_change_t *c, uint32_t *ticket) { name_change=*c; ++name_calls; *ticket=9; return PN_OK; }
passport_name_result_t passport_names_lookup(const passport_name_key_t *key, char *label) { (void)key; strcpy(label,"Desk"); return PN_OK; }
bool passport_names_text_valid(const char *label, size_t length) { return length > 0 && length <= 48 && !memchr(label,0,length); }
passport_name_key_t passport_wifi_ap_name_key(const passport_wifi_row_t *row)
{ passport_name_key_t key={.kind=PN_SCAN_AP}; memcpy(key.ap.ssid,row->ssid.bytes,row->ssid.length); key.ap.length=row->ssid.length; return key; }

static void pump(void) { passport_usb_service(&observed_ble,&observed_wifi,PASSPORT_DARK,true,false); }
static void send(uint32_t nonce, uint32_t id, const char *command)
{
    tx_size=frames=rx_at=0; tx[0]=0;
    int used=snprintf(rx,sizeof(rx),"\x1eP1 %08x %08x req %s",nonce,id,command);
    rx_size=passport_usb_seal(rx,sizeof(rx),(size_t)used); assert(rx_size); pump();
}
static void reset(void)
{
    memset(&job,0,sizeof(job)); passport_usb_parser_reset(&parser); sequence=0;
    rx_at=rx_size=tx_size=frames=ble_calls=wifi_calls=join_calls=theme_calls=name_calls=0;
    tx[0]=0; connected=queue_ok=true; clock_ms=1000; theme_error=0; name_outcome=PN_PENDING;
    observed_ble=(passport_ble_state_t){.synced=true,.storage_known=true,.generation=4,.bond_revision=7,.input_revision=9};
    observed_wifi=(passport_wifi_state_t){.initialized=true,.storage_known=true};
    s_ctx.peek_char=NONE;s_ctx.non_blocking=false;s_ctx.rx_func=usb_serial_jtag_rx_char_no_driver;
    s_ctx.rx_mode=ESP_LINE_ENDINGS_LF;p_usb_serial_jtag_obj=NULL;fifo_reads=0;
    sound_state=(passport_sound_state_t){.preference_known=true}; passport_usb_init();
}
static void protocol_boundaries(void)
{
    assert(passport_usb_crc("123456789",9)==0x29b1);
    uint32_t u; int32_t i; size_t n; unsigned char bytes[4];
    assert(passport_usb_u32("4294967295",&u)&&u==UINT32_MAX);
    assert(!passport_usb_u32("4294967296",&u)&&!passport_usb_u32("+1",&u));
    assert(passport_usb_i32("-2147483648",&i)&&i==INT32_MIN);
    assert(!passport_usb_i32("2147483648",&i));
    assert(passport_usb_unhex("-",bytes,4,&n)&&n==0);
    assert(!passport_usb_unhex("abc",bytes,4,&n)&&!passport_usb_unhex("0011223344",bytes,4,&n));
    reset(); send(0,88,"hello"); assert(strstr(tx,"data hello last=0")&&frames==2&&!sequence);
    send(boot+1,1,"lock"); assert(strstr(tx,"done stale")&&!ble_calls);
    strcpy(rx,"\x1eP1 garbage\rfoo\n"); rx_size=strlen(rx); rx_at=0; tx_size=0; pump(); assert(!parser.active&&!tx_size);
    rx_at=0; rx_size=sizeof(rx); memset(rx,'x',sizeof(rx)); rx[0]=30; pump(); assert(!parser.active);
    passport_usb_request_t parsed; passport_usb_feed(&parser,30,clock_ms,&parsed);
    passport_usb_feed(&parser,'s',clock_ms,&parsed); assert(passport_usb_parser_expire(&parser,clock_ms+1000));
    for(size_t j=0;j<sizeof(parser);++j) assert(!((unsigned char *)&parser)[j]);
    // Random untrusted bytes cannot exceed parser storage or yield dangling state.
    uint32_t random=7;
    for(unsigned j=0;j<50000;++j) { random=random*1664525u+1013904223u; passport_usb_feed(&parser,random>>24,j,&parsed); }
    passport_usb_parser_reset(&parser);
}
static void pinned_vfs_boundary(void)
{
    reset();rx[0]=30;rx_size=1;rx_at=0;s_ctx.non_blocking=true;unsigned char byte;
    assert(fixture_read(0,&byte,1)==-1&&errno==EWOULDBLOCK&&!fifo_reads&&rx_at==0);
    passport_usb_init();assert(available);
    assert(fixture_read(0,&byte,1)==1&&byte==30&&fifo_reads==1);
    assert(fixture_read(0,&byte,1)==-1&&errno==EWOULDBLOCK&&fifo_reads==2); // No wait when FIFO empty.
    s_ctx.rx_mode=ESP_LINE_ENDINGS_CRLF;rx_at=0;rx[0]='\r';rx_size=1;
    assert(fixture_read(0,&byte,1)==-1&&errno==EWOULDBLOCK);
    rx[1]='\n';rx_size=2;assert(fixture_read(0,&byte,1)==1&&byte=='\n');
    p_usb_serial_jtag_obj=&driver_fixture;passport_usb_init();assert(!available);
    p_usb_serial_jtag_obj=NULL;
}
static void receipts_and_replay(void)
{
    reset(); send(boot,1,"key 4 9 0 40"); assert(ble_calls==1&&ble_request.usage==40&&strstr(tx,"ack accepted")&&!strstr(tx,"done"));
    send(boot,1,"key 4 9 0 41"); assert(ble_calls==1&&strstr(tx,"ack accepted"));
    send(boot,2,"wifi.use_ap"); assert(!wifi_calls&&strstr(tx,"done busy"));
    send(boot,3,"status"); assert(frames==10&&strstr(tx,"pending=1")&&job.owner==USB_BLE);
    observed_ble.command=(passport_receipt_t){1,PR_APPLIED,0}; pump(); assert(!job.owner&&strstr(tx,"00000001 done applied"));
    send(boot,1,"lock"); assert(ble_calls==1&&strstr(tx,"done applied"));
    send(boot,2,"lock"); assert(ble_calls==1&&strstr(tx,"done stale"));
    send(boot,4,"wifi.use_ap"); assert(wifi_calls==1&&wifi_request.action==PW_USE_AP);
    observed_wifi.command=(passport_receipt_t){4,PR_PENDING,0}; pump(); assert(job.owner);
    send(boot,5,"lock"); assert(ble_calls==2&&strstr(tx,"00000004 done unknown")&&strstr(tx,"00000005 ack accepted"));
    clock_ms+=70001; pump(); assert(!job.owner&&job.receipt.result==PR_UNKNOWN);
    queue_ok=false; send(boot,6,"unlock 4"); assert(strstr(tx,"done busy")&&!job.owner);
}
static void secrets_settings_and_names(void)
{
    reset(); send(boot,1,"wifi.join 8 2 736563726574313233");
    assert(join_calls==1&&submitted.scan_revision==8&&submitted.row==2&&!strcmp(submitted.password,"secret123"));
    assert(!strstr(tx,"secret123")&&!strstr(tx,"736563726574313233"));
    for(size_t j=0;j<sizeof(parser);++j) assert(!((unsigned char *)&parser)[j]);
    observed_wifi.command=(passport_receipt_t){1,PR_APPLIED,0};pump();
    send(boot,2,"wifi.join 8 2 00"); assert(join_calls==1&&strstr(tx,"done invalid"));
    theme_error=17;send(boot,3,"theme 1");assert(theme_calls==1&&strstr(tx,"done storage_uncertain detail=17"));
    send(boot,4,"sound 1"); assert(job.owner==USB_SOUND&&!strstr(tx,"done"));
    sound_state.output=PASSPORT_SOUND_READY;pump();assert(job.receipt.result==PR_APPLIED);
    send(boot,5,"sound.cue 2");assert(job.owner==USB_CUE);
    sound_state.cue.result=PR_CANCELLED;pump();assert(job.receipt.result==PR_CANCELLED);
    observed_ble.peer_count=1;observed_ble.peers[0].bond_id[0]=55;
    send(boot,6,"name.ble 12 7 0 4465736b");
    assert(name_calls==1&&name_change.key.bond[0]==55&&job.owner==USB_NAME&&!strstr(tx,"done"));
    name_outcome=PN_UNCERTAIN;pump();assert(job.receipt.result==PR_STORAGE_UNCERTAIN);
    send(boot,7,"name.ble 12 6 0 4465736b"); assert(name_calls==1&&strstr(tx,"done stale"));
}
static void largest_reads(void)
{
    reset();observed_wifi.scan_known=true;observed_wifi.scan_revision=UINT32_MAX;observed_wifi.scan_count=16;
    observed_wifi.rows[15]=(passport_wifi_row_t){.ssid={.length=32},.auth=255,.security=PW_UNSUPPORTED,.rssi=-128,.channel=255,.profile_id=UINT32_MAX};
    memset(observed_wifi.rows[15].ssid.bytes,255,32);
    send(boot,1,"wifi.row 4294967295 15");assert(frames==2&&strstr(tx,"data row"));
    observed_ble.generation=observed_ble.bond_revision=observed_ble.input_revision=UINT32_MAX;
    observed_wifi.revision=observed_wifi.active_profile=observed_wifi.setup_id=observed_wifi.pending_id=UINT32_MAX;
    send(boot,2,"status");assert(frames==10&&strstr(tx,"data ble_revision"));
    connected=false;passport_usb_feed(&parser,30,clock_ms,&(passport_usb_request_t){0});pump();assert(!parser.active);
}
int main(void)
{
    pinned_vfs_boundary();protocol_boundaries();receipts_and_replay();secrets_settings_and_names();largest_reads();
    puts("PASS: USB framing/CRC/bounds, owner admission/final receipts, replay and boot guards, safety supersession, credential redaction, settings and labels");
}
