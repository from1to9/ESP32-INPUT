// SPDX-License-Identifier: Apache-2.0
#define main ble_suite_main
/* The original test main relies on C's implicit main return. Its renamed,
 * unused entry point is no longer special; production callbacks are unchanged. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "../ble_hid/test_ble_hid.c"
#pragma GCC diagnostic pop
#undef main

static bool engine_ble_send(void *context,uint8_t id,const uint8_t *data,size_t length)
{ return ble_hid_send_generation(*(uint64_t *)context,id,data,length); }

int main(void)
{
    reset();connect_peer();subscribe();
    hid_engine_t engine;hid_engine_init(&engine);hid_engine_set_link(&engine,true,true);
    uint64_t generation=ble_hid_snapshot().generation;
    for(unsigned i=0;i<2;++i) {
        assert(hid_engine_step(&engine,engine_ble_send,&generation)==HID_ENGINE_STEP_SENT);
        complete_notification(ESP_GATT_OK);
    }
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,300,-301,1,1)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_step(&engine,engine_ble_send,&generation)==HID_ENGINE_STEP_SENT);
    int x=(int8_t)state.mouse[1],y=(int8_t)state.mouse[2];
    assert(x==127 && y==-127 && ble_hid_snapshot().congested);
    assert(!hid_engine_observe_transport(&engine,2,true));
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,50,-50,2,2)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_step(&engine,engine_ble_send,&generation)==HID_ENGINE_STEP_BLOCKED);
    complete_notification(ESP_GATT_OK);assert(!ble_hid_snapshot().congested);
    assert(!hid_engine_observe_transport(&engine,3,false));
    while(engine.count) {
        assert(hid_engine_step(&engine,engine_ble_send,&generation)==HID_ENGINE_STEP_SENT);
        x+=(int8_t)state.mouse[1];y+=(int8_t)state.mouse[2];
        complete_notification(ESP_GATT_OK);
    }
    assert(x==350 && y==-351);
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,300,0,3,3)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_step(&engine,engine_ble_send,&generation)==HID_ENGINE_STEP_SENT);
    complete_notification(1); /* Real completion error must revoke readiness. */
    const ble_hid_snapshot_t failed=ble_hid_snapshot();assert(!failed.ready || failed.generation!=generation);
    hid_engine_set_link(&engine,failed.connected,failed.ready);
    assert(!engine.count && engine.state.release_pending);
    puts("PASS: real BLE reservation/CONF, signed remainder, busy retention and failure isolation");
    return 0;
}
