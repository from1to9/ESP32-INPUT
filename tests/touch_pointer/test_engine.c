// SPDX-License-Identifier: Apache-2.0
#define main engine_suite_main
#include "../test_hid_engine.c"
#undef main

static hid_engine_enqueue_result_t displacement(hid_engine_t *e, int x, int y, uint64_t at)
{ return hid_engine_enqueue_mouse_displacement(e,e->state.epoch,INPUT_LOCAL,x,y,at,at); }

static void partition_and_signed_remainders(void)
{
    for (unsigned parts = 1; parts <= 32; ++parts) {
        hid_engine_t e; sink_t sink = {0}; connected(&e,&sink,INPUT_MOUSE);
        assert(!hid_engine_observe_transport(&e,100,true));
        for (unsigned i=0;i<parts;++i) {
            const int x=(int)((i+1U)*300U/parts)-(int)(i*300U/parts);
            const int y=-(int)((i+1U)*301U/parts)+(int)(i*301U/parts);
            assert(displacement(&e,x,y,100)==HID_ENGINE_ENQUEUE_OK);
        }
        assert(e.count==1 && e.motion_since_ms==100);
        assert(!hid_engine_observe_transport(&e,110,false));
        int x=0,y=0; unsigned packets=0;
        while(e.count) { assert(step(&e,&sink)==HID_ENGINE_STEP_SENT);
            x+=(int8_t)sink.data[1];y+=(int8_t)sink.data[2];++packets; }
        assert(x==300 && y==-301 && packets==3 && !e.buffered_motion_count);
    }
}

static void age_survives_partial_send_and_advances_after_drain(void)
{
    hid_engine_t e; sink_t s={0};connected(&e,&s,INPUT_MOUSE);
    assert(displacement(&e,300,-301,100)==HID_ENGINE_ENQUEUE_OK);
    assert(step(&e,&s)==HID_ENGINE_STEP_SENT && e.queue[e.head].remaining_x==173);
    assert(!hid_engine_observe_transport(&e,349,false));
    assert(hid_engine_observe_transport(&e,350,false));
    assert(!e.count && e.state.release_pending && !e.buffered_motion_count);
    release_barrier(&e,&s);
    assert(displacement(&e,20,0,400)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_scroll(&e,e.state.epoch,INPUT_LOCAL,1)==HID_ENGINE_ENQUEUE_OK);
    assert(displacement(&e,30,0,500)==HID_ENGINE_ENQUEUE_OK);
    assert(step(&e,&s)==HID_ENGINE_STEP_SENT && e.motion_since_ms==500);
    assert(!hid_engine_observe_transport(&e,650,false));
    assert(hid_engine_observe_transport(&e,750,false));
}

static void atomic_limits_zero_sum_and_reserved_release(void)
{
    hid_engine_t e; sink_t s={0};connected(&e,&s,INPUT_MOUSE);
    assert(displacement(&e,4000,4000,100)==HID_ENGINE_ENQUEUE_OK);
    hid_engine_t before=e;
    assert(displacement(&e,200,-200,100)==HID_ENGINE_ENQUEUE_FULL);
    assert(!memcmp(&before,&e,sizeof e));
    assert(displacement(&e,-4000,-4000,100)==HID_ENGINE_ENQUEUE_OK);
    assert(!e.count && !e.buffered_motion_count);
    assert(!hid_engine_observe_transport(&e,400,false));
    before=e;
    assert(hid_engine_enqueue_mouse_displacement(&e,e.state.epoch,INPUT_LOCAL,1,1,100,400)==HID_ENGINE_ENQUEUE_STALE);
    assert(!memcmp(&before,&e,sizeof e));
    assert(hid_engine_enqueue_mouse_buttons(&e,e.state.epoch,INPUT_LOCAL,1)==HID_ENGINE_ENQUEUE_OK);
    for(unsigned i=0;i<30;++i)
        assert(hid_engine_enqueue_mouse_scroll(&e,e.state.epoch,INPUT_LOCAL,1)==HID_ENGINE_ENQUEUE_OK);
    before=e;
    assert(displacement(&e,20,0,400)==HID_ENGINE_ENQUEUE_FULL);
    assert(!memcmp(&before,&e,sizeof e));
    assert(!hid_engine_observe_transport(&e,400,true));
    assert(hid_engine_enqueue_mouse_buttons(&e,e.state.epoch,INPUT_LOCAL,0)==HID_ENGINE_ENQUEUE_OK);
    assert(e.count==32);
}

static void buttons_and_scroll_are_ordering_barriers(void)
{
    hid_engine_t e; sink_t s={0};connected(&e,&s,INPUT_MOUSE);
    const uint64_t epoch=e.state.epoch;
    assert(displacement(&e,300,0,100)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_click(&e,epoch,INPUT_LOCAL,2)==HID_ENGINE_ENQUEUE_OK);
    assert(displacement(&e,-300,0,100)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_scroll(&e,epoch,INPUT_LOCAL,1)==HID_ENGINE_ENQUEUE_OK);
    assert(displacement(&e,20,0,100)==HID_ENGINE_ENQUEUE_OK);
    const int xs[]={127,127,46,0,0,-127,-127,-46,0,20};
    const int buttons[]={0,0,0,2,0,0,0,0,0,0};
    for(size_t i=0;i<sizeof xs/sizeof xs[0];++i) {
        assert(step(&e,&s)==HID_ENGINE_STEP_SENT);
        assert((int8_t)s.data[1]==xs[i] && s.data[0]==buttons[i]);
        assert((int8_t)s.data[3]==(i==8 ? 1:0));
    }
    assert(!e.count);
    assert(hid_engine_enqueue_mouse_buttons(&e,epoch,INPUT_LOCAL,1)==HID_ENGINE_ENQUEUE_OK);
    assert(displacement(&e,200,0,100)==HID_ENGINE_ENQUEUE_OK);
    assert(hid_engine_enqueue_mouse_buttons(&e,epoch,INPUT_LOCAL,0)==HID_ENGINE_ENQUEUE_OK);
    const int held[]={1,1,1,0};
    for(unsigned i=0;i<4;++i) { assert(step(&e,&s)==HID_ENGINE_STEP_SENT);assert(s.data[0]==held[i]); }
}

static void failure_and_authority_boundaries_erase_residual(void)
{
    for(unsigned boundary=0;boundary<6;++boundary) {
        hid_engine_t e; sink_t s={0};connected(&e,&s,INPUT_MOUSE);
        assert(displacement(&e,300,0,100)==HID_ENGINE_ENQUEUE_OK);
        const uint64_t epoch=e.state.epoch;
        if(boundary==0)hid_engine_set_locked(&e,true);
        if(boundary==1)assert(hid_engine_set_mode(&e,INPUT_KEYBOARD));
        if(boundary==2)assert(hid_engine_set_owner(&e,INPUT_REMOTE));
        if(boundary==3)hid_engine_set_link(&e,false,false);
        if(boundary==4)hid_engine_cancel(&e);
        if(boundary==5){s.fail_next=true;assert(step(&e,&s)==HID_ENGINE_STEP_SEND_FAILED);}
        assert(!e.count && !e.buffered_motion_count && e.state.epoch!=epoch && e.state.release_pending);
        assert(hid_engine_enqueue_mouse_displacement(&e,epoch,INPUT_LOCAL,1,1,100,100)==HID_ENGINE_ENQUEUE_STALE);
    }
}

int main(void)
{
    assert(engine_suite_main()==0);
    partition_and_signed_remainders();
    age_survives_partial_send_and_advances_after_drain();
    atomic_limits_zero_sum_and_reserved_release();
    buttons_and_scroll_are_ordering_barriers();
    failure_and_authority_boundaries_erase_residual();
    printf("PASS: touch displacement partitions, signed remainders, age, bounds, ordering and authority; action_bytes=%zu engine_bytes=%zu\n",sizeof(hid_engine_action_t),sizeof(hid_engine_t));
    return 0;
}
