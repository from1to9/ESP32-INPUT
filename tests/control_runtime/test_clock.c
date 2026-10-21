// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_clock.c"

static uint64_t clock_ms=100;
static unsigned critical,created,initialized,started,stopped,step;
static void (*callback)(struct timeval *);
static jmp_buf finish;
void test_enter(void){++critical;}
void test_leave(void){assert(critical);--critical;}
int64_t esp_timer_get_time(void){return(int64_t)clock_ms*1000;}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *out){assert(fn==worker&&!strcmp(name,"network_clock")&&stack==4096&&priority==1&&!arg&&!out);++created;return pdPASS;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c){assert(!c->start&&!c->wait_for_sync&&!c->server_from_dhcp&&c->num_of_servers==1&&c->sync_cb&&c->servers[0]);callback=c->sync_cb;++initialized;return ESP_OK;}
esp_err_t esp_netif_sntp_start(void){++started;return ESP_OK;}
void esp_netif_sntp_deinit(void){++stopped;}
void vTaskDelay(TickType_t ticks){
    assert(ticks==100&&critical==0);app_clock_snapshot_t view;uint64_t wall=0;app_clock_snapshot(&view);
    struct timeval tv={.tv_sec=1900000000,.tv_usec=0};
    switch(++step){
    case 1:
        assert(view.synchronizing&&!view.known&&view.unix_seconds==0&&view.age_ms==UINT32_MAX);
        assert(!app_clock_wall_time(&wall));tv.tv_sec=1;callback(&tv);assert(!app_clock_wall_time(&wall));
        tv.tv_sec=1900000000;callback(&tv);assert(app_clock_wall_time(&wall)&&wall==1900000000);break;
    case 2:
        assert(stopped==1&&view.known&&!view.synchronizing&&!view.cryptographically_authenticated);
        tv.tv_sec=2000000000;callback(&tv);assert(app_clock_wall_time(&wall)&&wall==1900000000);clock_ms+=60001;break;
    case 3:
        assert(!view.known&&view.synchronizing&&initialized==2);
        app_clock_station_ready(false);callback(&tv);assert(!app_clock_wall_time(&wall));break;
    case 4:
        assert(!view.station_ready&&!view.synchronizing&&!view.known&&stopped==2);
        app_clock_station_ready(true);clock_ms+=60001;break;
    case 5:
        assert(view.synchronizing&&initialized==3);callback(&tv);assert(app_clock_wall_time(&wall));clock_ms=99;assert(!app_clock_wall_time(&wall));break;
    case 6:{
        assert(!view.known);
        app_clock_config_t invalid={.servers={"clock.example/attack"}};assert(app_clock_init(&invalid)==ESP_ERR_INVALID_ARG);
        app_clock_config_t replacement={.servers={"replacement.example"},.maximum_age_ms=60000,.refresh_interval_ms=60000};assert(app_clock_init(&replacement)==ESP_OK);clock_ms+=100;break;}
    case 7:
        assert(view.synchronizing&&!view.known&&created==1);callback(&tv);assert(app_clock_wall_time(&wall));++clock_ms;break;
    default:
        assert(view.known&&!view.cryptographically_authenticated&&created==1&&started==4);longjmp(finish,1);
    }
}
int main(void){
    /* Existing system time is deliberately irrelevant: no API reads it as
     * evidence. Only the registered SDK callback can establish an anchor. */
    uint64_t wall=0;assert(!app_clock_wall_time(&wall));app_clock_config_t c={.servers={"clock.example"},.maximum_age_ms=60000,.refresh_interval_ms=60000};assert(app_clock_init(&c)==ESP_OK);app_clock_station_ready(true);
    if(setjmp(finish)==0)worker(NULL);
    assert(critical==0);puts("PASS: clock callback evidence, unknown boot/date, stale and regressed time, disconnect, late callback and reconfiguration");return 0;
}
