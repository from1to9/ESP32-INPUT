#pragma once
#include "stubs.h"
#include <string.h>
typedef struct { uint8_t type, val[6]; } ble_addr_t;
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EREJECT 1
#define BLE_HS_ESTORE_CAP 2
#define BLE_HS_ESTORE_FAIL 3
#define BLE_HS_ENOENT 4
#define BLE_HS_EALREADY 5
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_CCCD 3
#define BLE_STORE_OBJ_TYPE_CSFC 4
#define BLE_STORE_OBJ_TYPE_PEER_ADDR 5
#define BLE_STORE_OBJ_TYPE_LOCAL_IRK 6
struct ble_store_value_sec {
    ble_addr_t peer_addr;
    uint8_t ltk_present, key_size, sc;
    uint16_t ediv;
    uint64_t rand_num;
    uint8_t ltk[16];
};
struct ble_store_key_sec { ble_addr_t peer_addr; uint8_t idx; };
union ble_store_key { struct ble_store_key_sec sec; };
union ble_store_value { struct ble_store_value_sec sec; uint8_t bytes[80]; };
struct ble_store_status_event { int type; };
int ble_addr_cmp(const ble_addr_t *, const ble_addr_t *);
int ble_store_read_our_sec(const struct ble_store_key_sec *, struct ble_store_value_sec *);
int ble_store_util_bonded_peers(ble_addr_t *, int *, int);
int ble_store_iterate(int, int (*)(int, union ble_store_value *, void *), void *);
int ble_store_config_read(int, const union ble_store_key *, union ble_store_value *);
int ble_store_config_write(int, const union ble_store_value *);
int ble_store_config_delete(int, const union ble_store_key *);
void ble_store_config_init(void);
int ble_gap_unpair(const ble_addr_t *);

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
struct os_mbuf { uint8_t data[256]; unsigned len; };
#define OS_MBUF_PKTLEN(m) ((m)->len)
int os_mbuf_append(struct os_mbuf *, const void *, unsigned);
int os_mbuf_copydata(struct os_mbuf *, unsigned, unsigned, void *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, unsigned);
int ble_gatts_notify_custom(uint16_t, uint16_t, struct os_mbuf *);
typedef struct { int value; } ble_uuid_t;
typedef ble_uuid_t ble_uuid16_t;
#define BLE_UUID16_DECLARE(x) (&(ble_uuid_t){(x)})
#define BLE_UUID16_INIT(x) {(x)}
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
typedef int (*access_fn)(uint16_t,uint16_t,struct ble_gatt_access_ctxt *, void *);
struct ble_gatt_dsc_def { const ble_uuid_t *uuid; int att_flags; access_fn access_cb; void *arg; };
struct ble_gatt_chr_def { const ble_uuid_t *uuid; access_fn access_cb; void *arg; int flags; uint16_t *val_handle; struct ble_gatt_dsc_def *descriptors; };
struct ble_gatt_svc_def { int type; const ble_uuid_t *uuid; struct ble_gatt_chr_def *characteristics; };
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_GATT_ACCESS_OP_WRITE_CHR 3
#define BLE_ATT_ERR_READ_NOT_PERMITTED 1
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 2
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 3
#define BLE_ATT_ERR_INSUFFICIENT_RES 4
#define BLE_ATT_F_READ 1
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_READ_ENC 2
#define BLE_GATT_CHR_F_NOTIFY 4
#define BLE_GATT_CHR_F_WRITE 8
#define BLE_GATT_CHR_F_WRITE_NO_RSP 16
#define BLE_GATT_CHR_F_WRITE_ENC 32
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HCI_ADV_FILT_CONN 1
#define BLE_OWN_ADDR_PUBLIC 0
struct ble_hs_adv_fields { int flags; uint8_t *name; int name_len, name_is_complete; ble_uuid16_t *uuids16; int num_uuids16, uuids16_is_complete, appearance, appearance_is_present; };
struct ble_gap_adv_params { int conn_mode, disc_mode, filter_policy; };
struct ble_gap_conn_desc { ble_addr_t peer_id_addr; struct { bool encrypted, bonded; } sec_state; };
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect;
    struct { int status; } enc_change;
    struct { uint16_t conn_handle, attr_handle; bool cur_notify; } subscribe;
};
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_ENC_CHANGE 3
#define BLE_GAP_EVENT_SUBSCRIBE 4
#define BLE_GAP_EVENT_REPEAT_PAIRING 5
#define BLE_GAP_EVENT_PASSKEY_ACTION 6
#define BLE_GAP_EVENT_ADV_COMPLETE 7
#define BLE_GAP_REPEAT_PAIRING_IGNORE 1
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
int ble_gap_adv_stop(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_wl_set(const ble_addr_t *, int);
int ble_gap_adv_start(int,const ble_addr_t *,int,const struct ble_gap_adv_params *,int (*)(struct ble_gap_event *,void *),void *);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc *);
int ble_gap_security_initiate(uint16_t);
int ble_gap_terminate(uint16_t,int);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char *);
void ble_svc_gap_device_appearance_set(int);
struct ble_npl_event { int unused; };
struct ble_npl_callout { int unused; };
void ble_npl_callout_init(struct ble_npl_callout *,void *,void (*)(struct ble_npl_event *),void *);
void ble_npl_callout_reset(struct ble_npl_callout *, unsigned);
void ble_npl_callout_stop(struct ble_npl_callout *);
unsigned ble_npl_time_ms_to_ticks32(unsigned);
void *nimble_port_get_dflt_eventq(void);
extern struct ble_config {
    void (*sync_cb)(void); void (*reset_cb)(int);
    int sm_io_cap, sm_bonding, sm_sc, sm_mitm, sm_our_key_dist, sm_their_key_dist;
    int (*store_read_cb)(int,const union ble_store_key *,union ble_store_value *);
    int (*store_write_cb)(int,const union ble_store_value *);
    int (*store_delete_cb)(int,const union ble_store_key *);
    int (*store_status_cb)(struct ble_store_status_event *,void *);
} ble_hs_cfg;
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 10
int nvs_open(const char *,int,nvs_handle_t *);
int nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
int nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
int nvs_commit(nvs_handle_t);
