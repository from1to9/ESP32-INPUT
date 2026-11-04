#!/usr/bin/env python3
"""Replay the pinned BR/EDR name setter and verified GAP read at the HCI boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / ".tools/esp-idf/components/bt/host/bluedroid"


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


def opcode_case(source, handler):
    start = source.index("    case HCI_READ_LOCAL_NAME:", source.index(handler))
    return source[start:source.index("        break;", start) + len("        break;")]


def main():
    cmd = (SDK / "stack/hcic/hcicmds.c").read_text()
    btm = (SDK / "stack/btm/btm_devctl.c").read_text()
    dm = (SDK / "bta/dm/bta_dm_act.c").read_text()
    btu = (SDK / "stack/btu/btu_hcif.c").read_text()
    body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, tBTM_STATUS, tBT_DEVICE_TYPE;
typedef uint16_t UINT16;
typedef bool BOOLEAN;
typedef void tBTM_GET_DEV_NAME_CBACK(UINT8, char *);
enum {BT_DEVICE_TYPE_BREDR=1, BT_DEVICE_TYPE_BLE=2, BT_DEVICE_TYPE_DUMO=3};
enum {BTM_SUCCESS, BTM_ILLEGAL_VALUE, BTM_DEV_RESET, BTM_CMD_STARTED, BTM_NO_RESOURCES, BTM_ERR_PROCESSING};
#define CLASSIC_BT_INCLUDED 1
#define TRUE 1
#define FALSE 0
#define BD_NAME_LEN 248
#define BTM_MAX_LOC_BD_NAME_LEN 248
#define HCIC_PREAMBLE_SIZE 3
#define HCIC_PARAM_SIZE_CHANGE_NAME 248
#define HCIC_PARAM_SIZE_READ_CMD 0
#define HCI_CHANGE_LOCAL_NAME 0x0c13
#define HCI_READ_LOCAL_NAME 0x0c14
#define HCI_SUCCESS 0
#define LOCAL_BR_EDR_CONTROLLER_ID 0
#define BCM_STRLCPY_S(dst,src,n) snprintf(dst,n,"%s",src)
#define BCM_STRNCMP_S strncmp
#define BTM_TRACE_ERROR(...)
#define UINT8_TO_STREAM(p,v) (*(p)++ = (uint8_t)(v))
#define UINT16_TO_STREAM(p,v) do {UINT8_TO_STREAM(p,v); UINT8_TO_STREAM(p,(v)>>8);} while(0)
#define ARRAY_TO_STREAM(p,v,n) do {memcpy(p,v,n);p+=n;} while(0)
struct {struct {char ble_bd_name[249], bredr_bd_name[249];} cfg;} btm_cb;
static bool ready(void) {return true;}
static struct host_controller {bool (*get_is_ready)(void);} host = {ready};
static struct host_controller *controller_get_interface(void) {return &host;}
typedef struct {UINT16 len, offset;} BT_HDR;
typedef struct {void *context; BT_HDR hdr; UINT8 data[251];} hci_cmd_metadata_t;
static hci_cmd_metadata_t packet;
static bool allocation_fails, controller_rejects_write;
static unsigned writes, reads, replies, startup_replies;
static char controller[249], eir[249], observed[249];
static UINT8 result;
static void *read_context;
#define HCI_GET_CMD_BUF(len) (allocation_fails ? NULL : &packet.hdr)
#define HCI_GET_CMD_METAMSG(p) (&packet)
static void btu_hcif_send_cmd(unsigned id, BT_HDR *p) {
    assert(id == 0 && p == &packet.hdr);
    unsigned op = packet.data[0] | (packet.data[1] << 8);
    if (op == HCI_CHANGE_LOCAL_NAME) {
        ++writes; if (!controller_rejects_write) memcpy(controller,packet.data+3,BD_NAME_LEN);
    } else {
        assert(op == HCI_READ_LOCAL_NAME); ++reads; read_context = packet.context;
    }
}
static void btm_read_local_name_complete(UINT8 *p, UINT16 len) {++startup_replies;}
typedef union {
    struct {UINT8 name[249]; tBT_DEVICE_TYPE name_type;} set_name;
    struct {tBT_DEVICE_TYPE name_type; tBTM_GET_DEV_NAME_CBACK *p_cback;} get_name;
} tBTA_DM_MSG;
static void bta_dm_set_eir(char *name) {strcpy(eir,name);}
static void reader(UINT8 status, char *value) {
    ++replies; result=status; snprintf(observed,sizeof observed,"%s",value ? value : "");
}
'''
    for source, signature in ((cmd, "BOOLEAN btsnd_hcic_change_name ("),
                              (cmd, "BOOLEAN btsnd_hcic_read_name_checked("),
                              (cmd, "BOOLEAN btsnd_hcic_read_name ("),
                              (btm, "tBTM_STATUS BTM_SetLocalDeviceName ("),
                              (btm, "tBTM_STATUS BTM_ReadLocalDeviceName ("),
                              (dm, "void bta_dm_set_dev_name ("), (dm, "void bta_dm_get_dev_name (")):
        body += function(source, signature)
    body += "static void complete(UINT8 *p, UINT16 evt_len, void *p_cplt_cback) {switch(HCI_READ_LOCAL_NAME) {\n"
    body += opcode_case(btu, "static void btu_hcif_hdl_command_complete (") + "\n}}\n"
    body += "static void failed_status(void *p_vsc_status_cback) {switch(HCI_READ_LOCAL_NAME) {\n"
    body += opcode_case(btu, "static void btu_hcif_hdl_command_status (") + "\n}}\n"
    body += r'''
static void set_name(const char *name) {
    tBTA_DM_MSG msg={.set_name={.name_type=BT_DEVICE_TYPE_BREDR}};
    strcpy((char *)msg.set_name.name,name); bta_dm_set_dev_name(&msg);
}
static void get_name(void) {
    tBTA_DM_MSG msg={.get_name={BT_DEVICE_TYPE_BREDR,reader}};
    bta_dm_get_dev_name(&msg);
}
static void report_controller(void) {
    UINT8 reply[1+BD_NAME_LEN]={HCI_SUCCESS};
    memcpy(reply+1,controller,BD_NAME_LEN); complete(reply,sizeof(reply),read_context);
}
int main(void) {
    set_name("Old-AUD"); assert(writes==1);
    allocation_fails=true; set_name("Desk-AUD"); allocation_fails=false;
    assert(writes==1 && !strcmp(eir,"Desk-AUD") && !strcmp(btm_cb.cfg.bredr_bd_name,"Desk-AUD"));
    get_name(); assert(reads==1 && replies==0);
    report_controller(); assert(replies==1 && result!=BTM_SUCCESS && !*observed);
    controller_rejects_write=true; set_name("Desk-AUD"); get_name(); report_controller();
    assert(writes==2 && result!=BTM_SUCCESS && !strcmp(controller,"Old-AUD"));
    controller_rejects_write=false; set_name("Desk-AUD"); get_name(); report_controller();
    assert(writes==3 && result==BTM_SUCCESS && !strcmp(observed,"Desk-AUD"));
    get_name(); UINT8 short_reply[1]={0}; complete(short_reply,1,read_context);
    assert(result!=BTM_SUCCESS);
    get_name(); failed_status(read_context); assert(result!=BTM_SUCCESS);
    allocation_fails=true; get_name(); assert(result==BTM_NO_RESOURCES); allocation_fails=false;
    unsigned before=replies; assert(btsnd_hcic_read_name()); report_controller();
    assert(startup_replies==1 && replies==before);
    puts("PASS: HCI write allocation failure, controller rejection, verified name, missing/short read, and isolated startup receipt");
}
'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / "replay.c"
        source.write_text(body)
        binary = source.with_suffix("")
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-fsanitize=address,undefined",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
