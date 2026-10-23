// SPDX-License-Identifier: Apache-2.0
#define main remote_suite_main
#include "../remote_input/test_remote_input.c"
#undef main

static int remote_sum(const fixture_t *f,size_t start)
{
    int x=0;
    for(size_t i=start;i<f->reports_count;++i)
        if(f->report_ids[i]==2)x+=(int8_t)f->reports[i][1];
    return x;
}

static void remote_wide_busy_and_atomic_retry(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    const char *begin="{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}";
    const char *move="{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":300,\"dy\":0}";
    (void)execute(&f,CP_OP_TOUCH_BEGIN,begin);
    const size_t start=f.reports_count;
    (void)execute(&f,CP_OP_TOUCH_MOVE,move);
    while(f.engine.count)step(&f);
    assert(remote_sum(&f,start)==300);
    assert(!hid_engine_observe_transport(&f.engine,f.now,true));
    const interaction_touch_state_t touch=f.model.touch;
    const float carry[2]={f.model.touch_carry[0],f.model.touch_carry[1]};
    cp_command_t busy=command(&f,CP_OP_TOUCH_MOVE,"{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":20}");
    cp_result_t result;
    assert(app_remote_input_execute(f.remote,&busy,f.now,&result)==CP_UNAVAILABLE);
    assert(result.state!=CP_RESULT_APPLIED && !memcmp(&touch,&f.model.touch,sizeof touch));
    assert(!memcmp(carry,f.model.touch_carry,sizeof carry));
    assert(!hid_engine_observe_transport(&f.engine,f.now,false));
    for(unsigned i=0;i<32;++i)
        assert(hid_engine_enqueue_mouse_move(&f.engine,f.engine.state.epoch,INPUT_REMOTE,1,0)==HID_ENGINE_ENQUEUE_OK);
    const interaction_touch_state_t before=f.model.touch;
    cp_command_t full=command(&f,CP_OP_TOUCH_MOVE,"{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":20}");
    assert(app_remote_input_execute(f.remote,&full,f.now,&result)==CP_BUSY);
    assert(!memcmp(&before,&f.model.touch,sizeof before) && !memcmp(carry,f.model.touch_carry,sizeof carry));
    while(f.engine.count)step(&f);
    const size_t retry_start=f.reports_count;
    (void)execute(&f,CP_OP_TOUCH_MOVE,"{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":20}");
    while(f.engine.count)step(&f);
    assert(remote_sum(&f,retry_start)==20);
    finish(&f);
}

static void remote_lease_expiry_erases_partial_residual(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_TOUCH_BEGIN,"{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}");
    (void)execute(&f,CP_OP_TOUCH_MOVE,"{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":300}");
    step(&f);assert(f.engine.count && f.engine.buffered_motion_count);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    f.now=view.lease_expires_ms;app_remote_input_service(f.remote,f.now);
    assert(!f.engine.count && !f.engine.buffered_motion_count && f.engine.state.owner==INPUT_LOCAL);
    const size_t start=f.reports_count;step(&f);step(&f);
    assert(remote_sum(&f,start)==0 && !f.engine.state.release_pending);
    finish(&f);
}

static void remote_cancel_erases_all_residual_and_end_preserves_order(void)
{
    const char *first="{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}";
    const char *second="{\"gesture_id\":\"00000000000000000000000000000002\",\"x\":100,\"y\":100}";
    for(unsigned mode=0;mode<5;++mode) {
        fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
        (void)execute(&f,CP_OP_TOUCH_BEGIN,first);
        const char *active=first;
        if(mode==4) {
            (void)execute(&f,CP_OP_TOUCH_END,first);step(&f);step(&f);
            (void)execute(&f,CP_OP_TOUCH_BEGIN,second);step(&f);
            assert(f.engine.mouse_buttons==1);active=second;
        }
        const char *move=mode==4 ?
            "{\"gesture_id\":\"00000000000000000000000000000002\",\"dx\":300}" :
            "{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":300}";
        (void)execute(&f,CP_OP_TOUCH_MOVE,move);
        if(mode==1 || mode==3 || mode==4)step(&f);
        if(mode>=2)assert(!hid_engine_observe_transport(&f.engine,f.now,true));
        assert(f.engine.count && f.engine.buffered_motion_count);
        const cp_result_t cancel=execute(&f,CP_OP_TOUCH_CANCEL,active);
        assert(cancel.state==CP_RESULT_CLEANUP_PENDING);
        assert(!f.engine.count && !f.engine.buffered_motion_count && f.engine.state.release_pending);
        assert(!f.model.touch.active && f.engine.state.owner==INPUT_LOCAL);
        if(mode>=2) {
            assert(hid_engine_step(&f.engine,send_report,&f)==HID_ENGINE_STEP_BLOCKED);
            assert(!hid_engine_observe_transport(&f.engine,f.now,false));
        }
        const size_t after_cancel=f.reports_count;step(&f);step(&f);
        assert(remote_sum(&f,after_cancel)==0 && !f.engine.state.release_pending);
        for(size_t i=after_cancel;i<f.reports_count;++i)
            for(size_t n=0;n<(f.report_ids[i]==1 ? 8U:4U);++n)assert(!f.reports[i][n]);
        assert(last_result(&f,cancel.command_key).state==CP_RESULT_APPLIED);
        finish(&f);
    }
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_TOUCH_BEGIN,first);
    const size_t start=f.reports_count;
    (void)execute(&f,CP_OP_TOUCH_MOVE,"{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":300}");
    step(&f);assert(f.engine.count);
    (void)execute(&f,CP_OP_TOUCH_END,"{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":400,\"y\":100}");
    assert(f.engine.count && f.engine.state.owner==INPUT_REMOTE);
    while(f.engine.count)step(&f);
    assert(remote_sum(&f,start)==300);
    finish(&f);
}

int main(void)
{
    assert(remote_suite_main()==0);
    remote_wide_busy_and_atomic_retry();remote_lease_expiry_erases_partial_residual();
    remote_cancel_erases_all_residual_and_end_preserves_order();
    puts("PASS: remote wide touch, busy/atomic retry, lease expiry, explicit CANCEL residual isolation and END drain");
    return 0;
}
