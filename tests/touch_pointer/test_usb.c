// SPDX-License-Identifier: Apache-2.0
#define main usb_suite_main
#include "../usb_hid/test_usb_hid.c"
#undef main

static bool engine_usb_send(void *context,uint8_t id,const uint8_t *data,size_t length)
{ return usb_hid_send_generation(*(uint64_t *)context,id,data,length); }

static int submit_and_complete(hid_engine_t *engine,uint64_t *generation)
{
    assert(hid_engine_step(engine,engine_usb_send,generation)==HID_ENGINE_STEP_SENT);
    assert(post_dispatch());run_deferred();
    const int x=hw.submitted_wire[0]==2 ? (int8_t)hw.submitted_wire[2]:0;
    complete_report();
    return x;
}

int main(void)
{
    assert(usb_suite_main()==0);
    start_ready();hid_engine_t engine;hid_engine_init(&engine);
    hid_engine_set_link(&engine,true,true);
    uint64_t generation=usb_hid_snapshot().generation;
    (void)submit_and_complete(&engine,&generation);
    (void)submit_and_complete(&engine,&generation);
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,300,0,1,1)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_step(&engine,engine_usb_send,&generation)==HID_ENGINE_STEP_SENT);
    assert(post_dispatch());run_deferred();int total=(int8_t)hw.submitted_wire[2];
    assert(total==127 && usb_hid_snapshot().congested);
    assert(!hid_engine_observe_transport(&engine,2,true));
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,50,0,2,2)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_step(&engine,engine_usb_send,&generation)==HID_ENGINE_STEP_BLOCKED);
    assert(engine.queue[engine.head].remaining_x==223);
    complete_report();assert(!usb_hid_snapshot().congested);
    assert(!hid_engine_observe_transport(&engine,3,false));
    while(engine.count)total+=submit_and_complete(&engine,&generation);
    assert(total==350 && !engine.buffered_motion_count);
    assert(hid_engine_enqueue_mouse_displacement(&engine,engine.state.epoch,INPUT_LOCAL,300,0,3,3)==HID_ENGINE_ENQUEUE_OK);
    const uint64_t old_generation=generation;
    stack_reset();generation=usb_hid_snapshot().generation;assert(generation!=old_generation);
    hid_engine_cancel(&engine);
    assert(!engine.count && engine.state.release_pending);
    puts("PASS: real USB admission, defer, host completion, busy retention and reset isolation");
    return 0;
}
