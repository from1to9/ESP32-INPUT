// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
static int test_getaddrinfo(const char *,const char *,const struct addrinfo *,struct addrinfo **);
static void test_freeaddrinfo(struct addrinfo *);
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
#include "../../firmware/main/app_firmware_update.c"
#include "weak_heap.h"

static uint64_t clock_ms=100;static unsigned critical,begins,writes,ends,aborts,boot_writes,restarts,health_marks;
static bool signature_valid=true,known_clock=true,fail_read,flip_readback,cancel_on_read,bad_encoding,probation;
static esp_err_t ota_end_error,floor_error,boot_error;static bool apply_failed_floor,apply_failed_boot;
static esp_err_t state_query_error,mark_error,floor_read_error,floor_open_error;
static unsigned state_queries,rollback_calls;
static bool new_image,apply_failed_mark,ignore_successful_mark,fail_state_after_mark,fail_state_on_image_open,cancel_on_staged_publish;
static uint8_t image[300],slot[1024],floor_blob[8];static bool floor_present;
static char catalog[4096];static int status=200,length_delta;
static jmp_buf loop_end,restart_end;
static esp_partition_t partitions[]={
    {.type=0,.subtype=0x10,.address=0x10000,.size=0x600000},
    {.type=0,.subtype=0x11,.address=0x610000,.size=0x600000},
    {.type=1,.subtype=2,.address=0xf20000,.size=0x10000},
    {.type=1,.subtype=0x40,.address=0xf30000,.size=0xd0000},
    {.type=1,.subtype=0,.address=0xc10000,.size=0x2000},
    {.type=1,.subtype=0x82,.address=0xc20000,.size=0x300000},
    {.type=1,.subtype=2,.address=0x9000,.size=0x6000},
    {.type=1,.subtype=1,.address=0xf000,.size=0x1000}};
static const esp_partition_t *boot=&partitions[0],*next=&partitions[1];
static esp_app_desc_t running={.project_name="esp32_input",.version="1.0.0"};
struct esp_http_client {const uint8_t *bytes;size_t length,offset;};static struct esp_http_client http;
void test_enter(void){++critical;}
void test_leave(void){
    assert(critical);--critical;
    if(!critical&&cancel_on_staged_publish&&updater&&updater->view.state==APP_UPDATE_STAGED&&updater->active_work==WORK_DOWNLOAD){
        cancel_on_staged_publish=false;assert(app_update_cancel(updater->view.job_id)==ESP_OK);assert(app_update_flash_busy());
    }
}
int64_t esp_timer_get_time(void){return(int64_t)clock_ms*1000;}
void esp_fill_random(void *p,size_t n){memset(p,0x35,n);}
void *heap_caps_calloc(size_t n,size_t s,unsigned caps){return test_heap_calloc(4,n,s,caps);}
void *heap_caps_aligned_calloc(size_t a,size_t n,size_t s,unsigned caps){return test_heap_calloc(a,n,s,caps);}
void *heap_caps_malloc(size_t n,unsigned caps){return test_heap_calloc(4,1,n,caps);}
void heap_caps_free(void *p){test_heap_free(p);}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *out){assert(fn==worker&&!strcmp(name,"firmware_update")&&stack==16384&&priority==1&&!arg&&!out);return pdPASS;}
void vTaskDelay(TickType_t ticks){assert(!critical);if(ticks==20)longjmp(loop_end,1);clock_ms+=ticks;}
bool app_clock_wall_time(uint64_t *seconds){if(!known_clock)return false;*seconds=1900000000;return true;}
const esp_app_desc_t *esp_app_get_description(void){return &running;}
const esp_partition_t *esp_partition_find_first(unsigned type,unsigned subtype,const char *label){
    const char *names[]={"ota_0","ota_1","control","recordings","otadata","model","nvs","phy_init"};
    for(unsigned i=0;i<8;++i)if(type==partitions[i].type&&subtype==partitions[i].subtype&&!strcmp(label,names[i]))return &partitions[i];return NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t n){assert(p==&partitions[1]&&offset+n<=sizeof(slot));memcpy(data,slot+offset,n);if(flip_readback&&n)((uint8_t *)data)[0]^=1;return ESP_OK;}
const esp_partition_t *esp_ota_get_running_partition(void){return &partitions[0];}
const esp_partition_t *esp_ota_get_boot_partition(void){return boot;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){assert(!p);return next;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t n,esp_ota_handle_t *handle){assert(p==&partitions[1]&&p!=boot&&n==OTA_WITH_SEQUENTIAL_WRITES);*handle=1;++begins;memset(slot,0,sizeof(slot));return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t handle,const void *data,size_t n){assert(handle==1&&n<=sizeof(image));memcpy(slot,data,n);++writes;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t handle){assert(handle==1);++ends;return ota_end_error;}
esp_err_t esp_ota_abort(esp_ota_handle_t handle){assert(handle==1);++aborts;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p){assert(p==&partitions[1]);++boot_writes;if(!boot_error||apply_failed_boot)boot=p;return boot_error;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *out){assert(p==&partitions[1]);*out=running;strcpy(out->version,"2.0.0");return ESP_OK;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *out){assert(!critical&&p==&partitions[0]);++state_queries;if(state_query_error)return state_query_error;*out=new_image?ESP_OTA_IMG_NEW:probation?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){assert(!critical);++health_marks;if((mark_error==ESP_OK&&!ignore_successful_mark)||apply_failed_mark)probation=false;if(fail_state_after_mark)state_query_error=ESP_FAIL;return mark_error;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){assert(!critical);++rollback_calls;return ESP_FAIL;}
void esp_restart(void){++restarts;longjmp(restart_end,1);}
esp_err_t nvs_open_from_partition(const char *p,const char *ns,int mode,nvs_handle_t *out){assert(!strcmp(p,"control")&&!strcmp(ns,"ctrl_ota"));if(mode==NVS_READONLY&&floor_open_error)return floor_open_error;*out=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t n,const char *key,void *out,size_t *size){assert(n==1&&!strcmp(key,"floor"));if(floor_read_error)return floor_read_error;if(!floor_present)return ESP_ERR_NVS_NOT_FOUND;assert(*size>=8);memcpy(out,floor_blob,8);*size=8;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t n,const char *key,const void *data,size_t size){assert(n==1&&!strcmp(key,"floor")&&size==8);if(!floor_error||apply_failed_floor){memcpy(floor_blob,data,8);floor_present=true;}return floor_error;}
esp_err_t nvs_commit(nvs_handle_t n){assert(n==1);return ESP_OK;}
void nvs_close(nvs_handle_t n){assert(n==1);}
static int test_getaddrinfo(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out){
    static struct sockaddr_in address;static struct addrinfo result;assert(!strcmp(name,"publisher.example")&&!service&&hints->ai_family==AF_INET);
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;assert(inet_pton(AF_INET,"8.8.8.8",&address.sin_addr)==1);result=(struct addrinfo){.ai_family=AF_INET,.ai_addr=(struct sockaddr *)&address,.ai_addrlen=sizeof(address)};*out=&result;return 0;
}
static void test_freeaddrinfo(struct addrinfo *answers){assert(answers);}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){
    assert(!strcmp(c->host,"8.8.8.8")&&!strcmp(c->common_name,"publisher.example")&&c->disable_auto_redirect&&c->timeout_ms==2000&&c->transport_type==HTTP_TRANSPORT_OVER_SSL);
    http=(struct esp_http_client){0};if(!strcmp(c->path,"/catalog")){http.bytes=(const uint8_t *)catalog;http.length=strlen(catalog);}else{assert(!strcmp(c->path,"/image.bin"));http.bytes=image;http.length=sizeof(image);if(fail_state_on_image_open)state_query_error=ESP_FAIL;}
    if(bad_encoding){esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.header_key="Content-Encoding",.header_value="gzip"};assert(c->event_handler(&event)==ESP_OK);}return &http;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value){assert(c==&http);if(!strcmp(key,"Host"))assert(!strcmp(value,"publisher.example"));else assert(!strcmp(key,"Accept-Encoding")&&!strcmp(value,"identity"));return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int n){assert(c==&http&&!n);return ESP_OK;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c){assert(c==&http);return(int64_t)c->length;}
int esp_http_client_get_status_code(esp_http_client_handle_t c){assert(c==&http);return status;}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c){return(int64_t)c->length+length_delta;}
int esp_http_client_read(esp_http_client_handle_t c,char *out,int capacity){assert(c==&http&&capacity>0);if(fail_read)return -1;size_t n=c->length-c->offset;if(n>(size_t)capacity)n=(size_t)capacity;memcpy(out,c->bytes+c->offset,n);c->offset+=n;if(cancel_on_read)updater->cancel=true;return(int)n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){return c->offset==c->length;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){assert(c==&http);return ESP_OK;}
void psa_set_key_type(psa_key_attributes_t *a,unsigned n){a->type=n;}
void psa_set_key_bits(psa_key_attributes_t *a,unsigned n){a->bits=n;}
void psa_set_key_usage_flags(psa_key_attributes_t *a,unsigned n){a->usage=n;}
void psa_set_key_algorithm(psa_key_attributes_t *a,unsigned n){a->algorithm=n;}
void psa_reset_key_attributes(psa_key_attributes_t *a){memset(a,0,sizeof(*a));}
psa_status_t psa_crypto_init(void){return PSA_SUCCESS;}
psa_status_t psa_import_key(const psa_key_attributes_t *a,const uint8_t *p,size_t n,mbedtls_svc_key_id_t *out){assert(a->type==PSA_ECC_FAMILY_SECP_R1&&a->bits==256&&a->usage==PSA_KEY_USAGE_VERIFY_MESSAGE&&a->algorithm==PSA_ALG_ECDSA(PSA_ALG_SHA_256)&&n==65&&p[0]==4);*out=1;return PSA_SUCCESS;}
psa_status_t psa_verify_message(mbedtls_svc_key_id_t key,unsigned algorithm,const uint8_t *p,size_t n,const uint8_t *sig,size_t size){assert(key==1&&algorithm==PSA_ALG_ECDSA(PSA_ALG_SHA_256)&&p&&n>0&&size==64&&sig);return signature_valid?PSA_SUCCESS:-1;}
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key){assert(key==1);return PSA_SUCCESS;}
psa_status_t psa_hash_setup(psa_hash_operation_t *op,unsigned algorithm){assert(algorithm==PSA_ALG_SHA_256);memset(op,0,sizeof(*op));return PSA_SUCCESS;}
psa_status_t psa_hash_update(psa_hash_operation_t *op,const uint8_t *p,size_t n){assert(op->count+n<=sizeof(op->bytes));memcpy(op->bytes+op->count,p,n);op->count+=n;return PSA_SUCCESS;}
psa_status_t psa_hash_finish(psa_hash_operation_t *op,uint8_t *out,size_t capacity,size_t *n){assert(capacity>=32);cp_sha256(op->bytes,op->count,out);*n=32;return PSA_SUCCESS;}
psa_status_t psa_hash_abort(psa_hash_operation_t *op){memset(op,0,sizeof(*op));return PSA_SUCCESS;}

static void catalog_make(void){
    uint8_t digest[32];cp_sha256(image,sizeof(image),digest);char hex_digest[65];for(unsigned i=0;i<32;++i)sprintf(hex_digest+i*2,"%02x",digest[i]);
    char payload[2048],quoted[3072],signature[129];memset(signature,'1',128);signature[128]=0;
    int n=snprintf(payload,sizeof(payload),"{\"v\":1,\"release_id\":\"release-2\",\"release_index\":2,\"product\":\"input\",\"board\":\"mosaico\",\"chip\":\"esp32s31\",\"project\":\"esp32_input\",\"layout\":\"esp32s31-16m-ota6-v3\",\"settings_schema\":4,\"version\":\"2.0.0\",\"size\":300,\"sha256\":\"%s\",\"url\":\"https://publisher.example/image.bin\",\"expires_unix_seconds\":\"1900003600\"}",hex_digest);assert(n>0&&(size_t)n<sizeof(payload));assert(cj_escape(payload,quoted,sizeof(quoted)));
    n=snprintf(catalog,sizeof(catalog),"{\"v\":1,\"alg\":\"ES256\",\"key_id\":\"release-key\",\"payload\":%s,\"signature\":\"%s\"}",quoted,signature);assert(n>0&&(size_t)n<sizeof(catalog));
}
static void step_worker(void){if(setjmp(loop_end)==0)worker(NULL);}
static app_update_authority_t authority_make(void){app_update_authority_t a={.authorized=true,.unlocked=true,.maintenance_ready=true,.restart_authorized=true,.policy_revision=7,.authority_expires_ms=clock_ms+10000};strcpy(a.expected_digest,updater->release.digest);return a;}
static void prepare_release(void){updater->staged=NULL;updater->view.boot_selected=false;updater->view.state=APP_UPDATE_IDLE;boot=&partitions[0];assert(app_update_check()==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_AVAILABLE);}
static void regression_boot(const app_update_config_t *config,bool pending,esp_err_t query_error)
{
    assert(!critical);if(updater){wipe(updater,sizeof(*updater));heap_caps_free(updater);updater=NULL;}
    clock_ms=100;probation=pending;state_query_error=query_error;mark_error=floor_read_error=floor_open_error=ESP_OK;new_image=false;
    apply_failed_mark=ignore_successful_mark=fail_state_after_mark=fail_state_on_image_open=cancel_on_staged_publish=false;
    signature_valid=known_clock=true;fail_read=flip_readback=cancel_on_read=bad_encoding=false;
    ota_end_error=floor_error=boot_error=ESP_OK;apply_failed_floor=apply_failed_boot=floor_present=false;
    begins=writes=ends=aborts=boot_writes=restarts=health_marks=state_queries=rollback_calls=0;
    boot=&partitions[0];next=&partitions[1];status=200;length_delta=0;
    esp_err_t expected=pending||query_error!=ESP_OK?ESP_ERR_NOT_SUPPORTED:ESP_OK;
    assert(app_update_init(config)==expected&&updater);
}
static void stage_release(void)
{
    prepare_release();app_update_authority_t authority=authority_make();
    assert(app_update_download("release-2",&authority)==ESP_OK);step_worker();
    assert(updater->view.state==APP_UPDATE_STAGED&&updater->staged==&partitions[1]);
}
static void running_state_must_be_observed_valid_before_any_update_write(const app_update_config_t *config)
{
    const esp_err_t errors[]={ESP_FAIL,ESP_ERR_NOT_FOUND};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i){
        regression_boot(config,false,errors[i]);app_update_snapshot_t view;app_update_snapshot(&view);
        assert(!view.available&&!view.running_state_known&&!view.running_confirmed&&view.running_state_error==errors[i]);
        assert(app_update_check()==ESP_ERR_INVALID_STATE);step_worker();assert(!begins&&!writes&&!boot_writes&&!health_marks&&!rollback_calls);
    }
    regression_boot(config,false,ESP_OK);new_image=true;
    assert(app_update_configure(config)==ESP_ERR_NOT_SUPPORTED&&updater->view.running_state_known&&!updater->view.running_confirmed);
    assert(app_update_check()==ESP_ERR_INVALID_STATE&&!begins&&!writes);new_image=false;
    regression_boot(config,true,ESP_OK);
    assert(updater->view.state==APP_UPDATE_PENDING_HEALTH&&!updater->view.available&&updater->view.probation_active&&!updater->view.health_confirmation_attempted);
    assert(app_update_check()==ESP_ERR_INVALID_STATE&&download_image()==ESP_ERR_INVALID_STATE&&!begins&&!writes);
    for(unsigned boundary=0;boundary<2;++boundary){
        regression_boot(config,false,ESP_OK);prepare_release();app_update_authority_t authority=authority_make();
        assert(app_update_download("release-2",&authority)==ESP_OK);
        if(boundary==0)state_query_error=ESP_FAIL;else fail_state_on_image_open=true;
        uint8_t preserved[sizeof(slot)];memcpy(preserved,slot,sizeof(slot));step_worker();
        assert(!begins&&!writes&&!boot_writes&&!floor_present&&!memcmp(preserved,slot,sizeof(slot)));
        assert(!updater->view.available&&!updater->view.running_state_known&&!updater->view.running_confirmed);
    }
    regression_boot(config,false,ESP_OK);stage_release();state_query_error=ESP_FAIL;
    app_update_authority_t authority=authority_make();assert(app_update_activate(updater->view.job_id,&authority)==ESP_OK);step_worker();
    assert(!boot_writes&&!floor_present&&!restarts&&!updater->view.running_confirmed);
}
static void staged_and_queued_cancellation_revokes_actual_stage_authority(const app_update_config_t *config)
{
    regression_boot(config,false,ESP_OK);assert(app_update_cancel(updater->view.job_id)==ESP_ERR_INVALID_STATE&&!updater->view.cleanup_pending);
    stage_release();uint64_t job=updater->view.job_id;uint8_t preserved[sizeof(slot)];memcpy(preserved,slot,sizeof(slot));
    assert(app_update_cancel(job)==ESP_OK&&updater->view.state==APP_UPDATE_CANCELLED&&!updater->staged&&!updater->view.cleanup_pending&&!app_update_flash_busy());
    app_update_authority_t authority=authority_make();assert(app_update_activate(job,&authority)==ESP_ERR_INVALID_STATE);
    step_worker();assert(!boot_writes&&!restarts&&!memcmp(preserved,slot,sizeof(slot))&&!app_update_flash_busy());
    assert(app_update_cancel(job)==ESP_ERR_INVALID_STATE&&!updater->view.cleanup_pending);
    regression_boot(config,false,ESP_OK);stage_release();job=updater->view.job_id;authority=authority_make();
    assert(app_update_activate(job,&authority)==ESP_OK&&app_update_cancel(job)==ESP_OK);
    step_worker();assert(updater->view.state==APP_UPDATE_CANCELLED&&!updater->staged&&!boot_writes&&!restarts&&!app_update_flash_busy());
    regression_boot(config,false,ESP_OK);prepare_release();authority=authority_make();
    assert(app_update_download("release-2",&authority)==ESP_OK&&app_update_cancel(updater->view.job_id)==ESP_OK);
    step_worker();assert(!begins&&!writes&&updater->view.state==APP_UPDATE_CANCELLED&&!app_update_flash_busy());
    regression_boot(config,false,ESP_OK);prepare_release();authority=authority_make();
    assert(app_update_download("release-2",&authority)==ESP_OK);cancel_on_staged_publish=true;step_worker();
    assert(!cancel_on_staged_publish&&begins==1&&writes==1&&ends==1&&updater->view.state==APP_UPDATE_CANCELLED&&
        !updater->view.cleanup_pending&&!updater->staged&&!app_update_flash_busy()&&!boot_writes&&!restarts);
}
static void confirmation_readback_and_terminal_probation_deadline(const app_update_config_t *config)
{
    regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms+UPDATE_MINIMUM_HEALTH_MS;mark_error=ESP_FAIL;
    assert(app_update_confirm_health(true)==ESP_OK);step_worker();
    assert(health_marks==1&&updater->view.state==APP_UPDATE_PENDING_HEALTH&&updater->view.running_state_known&&!updater->view.running_confirmed&&
        updater->view.health_confirmation_attempted&&updater->view.health_confirmation_error==ESP_FAIL);
    assert(app_update_confirm_health(true)==ESP_ERR_INVALID_STATE);clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;step_worker();
    assert(health_marks==1&&rollback_calls==1&&updater->view.state==APP_UPDATE_BOOT_SELECTION_UNCERTAIN&&!updater->view.running_confirmed&&!updater->view.probation_active);
    step_worker();step_worker();assert(health_marks==1&&rollback_calls==1&&app_update_confirm_health(true)==ESP_ERR_INVALID_STATE);
    regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms+UPDATE_MINIMUM_HEALTH_MS;mark_error=ESP_FAIL;apply_failed_mark=true;
    assert(app_update_confirm_health(true)==ESP_OK);step_worker();
    assert(health_marks==1&&updater->view.state==APP_UPDATE_CONFIRMED&&updater->view.running_confirmed&&updater->view.health_confirmation_error==ESP_FAIL);
    clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;step_worker();assert(!rollback_calls);
    regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms+UPDATE_MINIMUM_HEALTH_MS;ignore_successful_mark=true;
    assert(app_update_confirm_health(true)==ESP_OK);step_worker();
    assert(updater->view.state==APP_UPDATE_PENDING_HEALTH&&!updater->view.running_confirmed);
    clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;step_worker();assert(rollback_calls==1&&health_marks==1);
    for(unsigned later_known=0;later_known<2;++later_known){
        regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms+UPDATE_MINIMUM_HEALTH_MS;fail_state_after_mark=true;
        assert(app_update_confirm_health(true)==ESP_OK);step_worker();
        assert(!updater->view.running_state_known&&!updater->view.running_confirmed&&updater->view.state==APP_UPDATE_PENDING_HEALTH);
        if(later_known)state_query_error=ESP_OK;
        clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;step_worker();
        assert(!rollback_calls&&health_marks==1);
        assert(updater->view.state==(later_known?APP_UPDATE_CONFIRMED:APP_UPDATE_BOOT_SELECTION_UNCERTAIN));
        step_worker();assert(!rollback_calls&&health_marks==1);
    }
    regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;
    assert(app_update_confirm_health(true)==ESP_ERR_INVALID_STATE);step_worker();assert(rollback_calls==1&&!health_marks);
    regression_boot(config,true,ESP_OK);clock_ms=updater->boot_ms-1;step_worker();
    assert(updater->view.state==APP_UPDATE_BOOT_SELECTION_UNCERTAIN&&!health_marks&&!rollback_calls);
}
static void confirmed_boot_cannot_override_failed_downgrade_floor_validation(const app_update_config_t *config)
{
    for(unsigned failure=0;failure<3;++failure){
        regression_boot(config,false,ESP_OK);wipe(updater,sizeof(*updater));heap_caps_free(updater);updater=NULL;
        probation=true;floor_present=true;memset(floor_blob,0,sizeof(floor_blob));
        if(failure==1)floor_read_error=ESP_FAIL;
        else if(failure==2)floor_open_error=ESP_FAIL;
        uint8_t preserved_floor[sizeof(floor_blob)];memcpy(preserved_floor,floor_blob,sizeof(floor_blob));
        assert(app_update_init(config)==ESP_ERR_NOT_SUPPORTED&&!updater->view.available&&updater->view.state==APP_UPDATE_PENDING_HEALTH);
        clock_ms=updater->boot_ms+UPDATE_MINIMUM_HEALTH_MS;
        assert(app_update_confirm_health(true)==ESP_OK);step_worker();
        assert(updater->view.state==APP_UPDATE_CONFIRMED&&updater->view.running_confirmed&&!updater->view.available);
        assert(updater->view.error==(failure==0?ESP_ERR_INVALID_CRC:ESP_FAIL));
        assert(app_update_check()==ESP_ERR_INVALID_STATE&&!begins&&!writes&&!boot_writes&&floor_present&&
            !memcmp(preserved_floor,floor_blob,sizeof(floor_blob)));
        floor_read_error=floor_open_error=ESP_OK;
        const uint8_t valid_floor[8]={'O','T','F','1',0,0,0,9};memcpy(floor_blob,valid_floor,sizeof(floor_blob));
        clock_ms=updater->boot_ms+UPDATE_PROBATION_MS;step_worker();
        assert(!updater->view.available&&app_update_check()==ESP_ERR_INVALID_STATE&&!rollback_calls);
        assert(app_update_configure(config)==ESP_OK&&updater->view.available&&updater->floor==9);
        assert(app_update_check()==ESP_OK);step_worker();
        assert(updater->view.state==APP_UPDATE_ERROR&&!begins&&!writes&&!boot_writes&&updater->floor==9);
    }
}
int main(void){
    for(unsigned i=0;i<sizeof(image);++i)image[i]=(uint8_t)i;catalog_make();uint8_t public_key[65]={4};
    app_update_config_t c={.abi_version=1,.catalog_url="https://publisher.example/catalog",.tls_ca_pem="TEST CA",.signing_key_id="release-key",.signing_public_key=public_key,.product="input",.board="mosaico",.layout=APP_UPDATE_LAYOUT};
    test_heap_fail_next=2;
    assert(app_update_init(&c)==ESP_ERR_NO_MEM&&!updater&&!test_heap_live);
    test_heap_fail_external=true;
    assert(app_update_init(&c)==ESP_OK&&updater->view.available);assert(!app_update_flash_busy());
    assert(test_heap_external_failures);test_heap_fail_external=false;
    test_heap_fail_bytes=sizeof(cj_document_t);
    assert(app_update_check()==ESP_OK);step_worker();
    assert(updater->view.state==APP_UPDATE_ERROR&&updater->view.error==ESP_ERR_NO_MEM&&test_heap_live==1&&!begins&&!boot_writes);
    test_heap_fail_bytes=0;
    assert(layout_ready());partitions[0].address=0x420000;assert(!layout_ready());partitions[0].address=0x10000;
    partitions[1].size=0x400000;assert(!layout_ready());partitions[1].size=APP_UPDATE_SLOT_BYTES;assert(layout_ready());
    source_t source;assert(!source_parse("http://publisher.example/image.bin",&source));assert(!source_parse("https://user@publisher.example/image.bin",&source));assert(!source_parse("https://publisher.example/image.bin?token=x",&source));
    assert(!allowed_ip(inet_addr("127.0.0.1"),true)&&!allowed_ip(inet_addr("169.254.1.1"),true)&&!allowed_ip(inet_addr("10.0.0.1"),false)&&allowed_ip(inet_addr("10.0.0.1"),true));
    signature_valid=false;assert(app_update_check()==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_ERROR&&!begins&&!boot_writes);signature_valid=true;
    known_clock=false;assert(app_update_check()==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_ERROR&&!begins);known_clock=true;
    status=302;assert(app_update_check()==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_ERROR);status=200;
    bad_encoding=true;assert(app_update_check()==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_ERROR);bad_encoding=false;
    prepare_release();app_update_authority_t a=authority_make();a.authorized=false;assert(app_update_download("release-2",&a)==ESP_ERR_INVALID_STATE&&!begins);
    a=authority_make();assert(app_update_download("release-2",&a)==ESP_OK);assert(app_update_check()==ESP_ERR_INVALID_STATE);assert(app_update_flash_busy());length_delta=1;step_worker();assert(updater->view.state==APP_UPDATE_ERROR&&!begins);length_delta=0;
    prepare_release();a=authority_make();assert(app_update_download("release-2",&a)==ESP_OK);cancel_on_read=true;step_worker();assert(updater->view.state==APP_UPDATE_CANCELLED&&begins==1&&!writes&&aborts==1&&!boot_writes);cancel_on_read=false;
    prepare_release();a=authority_make();assert(app_update_download("release-2",&a)==ESP_OK);ota_end_error=ESP_FAIL;step_worker();assert(updater->view.state==APP_UPDATE_ERROR&&!updater->staged&&!boot_writes);ota_end_error=0;
    prepare_release();a=authority_make();assert(app_update_download("release-2",&a)==ESP_OK);flip_readback=true;step_worker();assert(updater->view.state==APP_UPDATE_ERROR&&!updater->staged&&!boot_writes);flip_readback=false;
    prepare_release();a=authority_make();assert(app_update_download("release-2",&a)==ESP_OK);step_worker();assert(updater->view.state==APP_UPDATE_STAGED&&updater->staged==&partitions[1]&&!boot_writes&&!updater->view.boot_selected&&boot==&partitions[0]);
    uint64_t job=updater->view.job_id;assert(app_update_check()==ESP_ERR_INVALID_STATE);assert(app_update_activate(job+1,&a)==ESP_ERR_INVALID_ARG);
    a=authority_make();assert(app_update_activate(job,&a)==ESP_OK);updater->work=WORK_NONE;floor_error=ESP_FAIL;apply_failed_floor=true;assert(activate()==ESP_FAIL);assert(updater->view.state==APP_UPDATE_BOOT_SELECTION_UNCERTAIN&&!boot_writes&&floor_present);floor_error=0;
    updater->view.state=APP_UPDATE_STAGED;a=authority_make();assert(app_update_activate(job,&a)==ESP_OK);updater->work=WORK_NONE;boot_error=ESP_FAIL;apply_failed_boot=true;assert(activate()==ESP_FAIL);assert(updater->view.state==APP_UPDATE_BOOT_SELECTION_UNCERTAIN&&boot==&partitions[1]&&!restarts);boot_error=0;
    boot=&partitions[0];updater->view.state=APP_UPDATE_STAGED;a=authority_make();assert(app_update_activate(job,&a)==ESP_OK);updater->work=WORK_NONE;
    if(setjmp(restart_end)==0)(void)activate();assert(restarts==1&&updater->view.boot_selected&&updater->view.state==APP_UPDATE_RESTART_PENDING);
    assert(app_update_cancel(job)==ESP_ERR_INVALID_STATE);assert(app_update_configure(&c)==ESP_ERR_INVALID_STATE);
    regression_boot(&c,true,ESP_OK);assert(app_update_confirm_health(true)==ESP_ERR_INVALID_STATE&&!health_marks);
    clock_ms=updater->boot_ms+45001;assert(app_update_confirm_health(false)==ESP_ERR_INVALID_STATE);assert(app_update_confirm_health(true)==ESP_OK);step_worker();assert(health_marks==1&&updater->view.state==APP_UPDATE_CONFIRMED);
    running_state_must_be_observed_valid_before_any_update_write(&c);
    staged_and_queued_cancellation_revokes_actual_stage_authority(&c);
    confirmation_readback_and_terminal_probation_deadline(&c);
    confirmed_boot_cannot_override_failed_downgrade_floor_validation(&c);
    assert(critical==0);wipe(updater,sizeof(*updater));heap_caps_free(updater);updater=NULL;assert(!test_heap_live);
    puts("PASS: production OTA trust/signature boundary, source/TLS/DNS binding, inactive-slot stream/hash/readback, cancellation, uncertain boot and health probation");return 0;
}
