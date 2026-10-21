// SPDX-License-Identifier: Apache-2.0
#include "app_remote_input.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    hid_engine_t engine;
    interaction_t model;
    app_remote_input_t *remote;
    void *memory;
    app_remote_input_state_t state;
    uint64_t now, next_key;
    cp_result_t completions[512]; size_t completions_count;
    uint8_t reports[512][8], report_ids[512]; size_t reports_count;
    bool block_completion, observe_failure, delegate_enabled;
    size_t fail_report, delegated;
} fixture_t;

static bool observe(void *p, app_remote_input_state_t *out)
{ fixture_t *f=p; *out=f->state; return !f->observe_failure; }
static void changed(void *p, unsigned flags)
{
    fixture_t *f=p;
    if (flags & APP_REMOTE_CHANGED_UI) ++f->state.ui_revision;
    if (flags & APP_REMOTE_CHANGED_DRAFT) ++f->state.draft_revision;
    if (flags & APP_REMOTE_CHANGED_SELECTION) ++f->state.selection_revision;
    ++f->state.state_sequence;
}
static bool complete(void *p, const cp_result_t *result)
{
    fixture_t *f=p;
    if (f->block_completion) return false;
    assert(f->completions_count < 512); f->completions[f->completions_count++]=*result; return true;
}
static bool available(void *p, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{ fixture_t *f=p; (void)op; reason[0]=0; return f->delegate_enabled; }
static cp_error_t delegate(void *p, const cp_command_t *cmd, cp_result_t *result)
{
    fixture_t *f=p; ++f->delegated;
    char value[32]; bool enabled;
    if (cmd->operation == CP_OP_MODE_SELECT) {
        assert(cp_command_string(cmd,"mode",value,sizeof value));
        const input_mode_t mode=!strcmp(value,"mouse") ? INPUT_MOUSE :
            !strcmp(value,"keyboard") ? INPUT_KEYBOARD : INPUT_AUDIO;
        assert(hid_engine_set_mode(&f->engine,mode)); changed(f,APP_REMOTE_CHANGED_UI);
    } else if (cmd->operation == CP_OP_INPUT_MOTION_SET) {
        assert(cp_command_boolean(cmd,"enabled",&enabled));
        assert(hid_engine_set_gyro_owned(&f->engine,INPUT_REMOTE,enabled));
        assert(interaction_set_motion_owned(&f->model,INPUT_REMOTE,enabled));
    } else if (cmd->operation == CP_OP_KEYBOARD_SUBMODE_SELECT) {
        assert(cp_command_string(cmd,"submode",value,sizeof value));
        assert(!strcmp(value,"touch"));
        hid_engine_cancel(&f->engine);
        interaction_set_keyboard_motion(&f->model,false);
        changed(f,APP_REMOTE_CHANGED_UI);
    }
    *result=(cp_result_t){.state=CP_RESULT_APPLIED,.error=CP_OK,.uncertain_index=-1};
    return CP_OK;
}
static bool send_report(void *p, uint8_t report_id, const uint8_t *data, size_t bytes)
{
    fixture_t *f=p;
    if (f->fail_report && f->reports_count+1 == f->fail_report) { f->fail_report=0; return false; }
    assert(f->reports_count < 512 && bytes <= 8);
    f->report_ids[f->reports_count]=report_id;
    memcpy(f->reports[f->reports_count++],data,bytes); return true;
}
static void step(fixture_t *f)
{
    f->now+=10;
    (void)hid_engine_observe_transport(&f->engine,f->now,f->engine.congested);
    app_remote_input_service(f->remote,f->now);
    (void)hid_engine_step(&f->engine,send_report,f);
    app_remote_input_service(f->remote,f->now);
}
static void setup(fixture_t *f, input_mode_t mode)
{
    *f=(fixture_t){.now=100,.delegate_enabled=true,
        .state={.transport_generation=7,.ui_revision=1,.draft_revision=1,.selection_revision=1,
            .state_sequence=1,.management_generation=3,.target_id="test-target",.remote_allowed=true,
            .screen_awake=true,.selection_visible=true,.shortcut_modifier=1}};
    hid_engine_init(&f->engine); hid_engine_set_link(&f->engine,true,true);
    assert(hid_engine_set_mode(&f->engine,mode));
    interaction_init(&f->model);
    interaction_sync(&f->model,&f->engine.state,true,f->now);
    f->memory=calloc(1,app_remote_input_bytes()); assert(f->memory);
    const app_remote_input_port_t port={.engine=&f->engine,.interaction=&f->model,.user=f,
        .observe=observe,.changed=changed,.complete=complete,.delegate_available=available,.delegate=delegate};
    assert(app_remote_input_init(f->memory,app_remote_input_bytes(),&port,&f->remote)==CP_OK);
    step(f);step(f); assert(!f->engine.state.release_pending);
}
static void finish(fixture_t *f) { free(f->memory); }
static cp_command_t command(fixture_t *f, cp_operation_id_t op, const char *args)
{
    app_remote_input_view_t view; app_remote_input_snapshot(f->remote,&view);
    cp_command_t cmd={.version=1,.operation=op,.command_key=++f->next_key,
        .identity={.principal_id={1},.session_id={2},.scopes=CP_SCOPE_ALL},
        .deadline_ms=f->now+2500,.management_generation=3,.input_epoch=f->engine.state.epoch,
        .transport_generation=f->state.transport_generation,.ui_revision=f->state.ui_revision,
        .lease_expires_ms=f->now+3000};
    memcpy(cmd.lease_id,view.lease_id,sizeof cmd.lease_id);
    strcpy(cmd.target_id,f->state.target_id); strcpy(cmd.args_json,args);
    return cmd;
}
static cp_result_t execute(fixture_t *f, cp_operation_id_t op, const char *args)
{
    cp_command_t cmd=command(f,op,args); cp_result_t result;
    assert(app_remote_input_execute(f->remote,&cmd,f->now,&result)==CP_OK); return result;
}
static uint64_t acquire(fixture_t *f, const char *producer)
{
    char args[128];
    if (producer) (void)snprintf(args,sizeof args,"{\"state_sequence\":\"%" PRIu64 "\",\"producer\":\"%s\"}",
        f->state.state_sequence,producer);
    else (void)snprintf(args,sizeof args,"{\"state_sequence\":\"%" PRIu64 "\"}",f->state.state_sequence);
    cp_command_t cmd=command(f,CP_OP_CONTROL_ACQUIRE,args); memset(cmd.lease_id,7,sizeof cmd.lease_id);
    cp_result_t result; assert(app_remote_input_execute(f->remote,&cmd,f->now,&result)==CP_OK);
    assert(result.state==CP_RESULT_ACCEPTED && f->engine.state.owner==INPUT_REMOTE && f->engine.state.release_pending);
    step(f);step(f);
    app_remote_input_view_t view;app_remote_input_snapshot(f->remote,&view);
    assert(view.active && !view.pending && !f->engine.state.release_pending);
    return result.command_key;
}
static cp_result_t last_result(fixture_t *f, uint64_t key)
{
    for (size_t i=f->completions_count;i>0;--i) if(f->completions[i-1].command_key==key)return f->completions[i-1];
    assert(false);return (cp_result_t){0};
}

static void lease_barrier_renewal_and_safety(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE); const uint64_t acquired=acquire(&f,NULL);
    assert(last_result(&f,acquired).state==CP_RESULT_APPLIED);
    char args[80];(void)snprintf(args,sizeof args,"{\"state_sequence\":\"%" PRIu64 "\"}",f.state.state_sequence);
    cp_result_t renewed=execute(&f,CP_OP_CONTROL_RENEW,args);step(&f);
    assert(last_result(&f,renewed.command_key).state==CP_RESULT_APPLIED);
    const cp_result_t released=execute(&f,CP_OP_CONTROL_RELEASE,"{}");
    assert(released.state==CP_RESULT_CLEANUP_PENDING && f.engine.state.owner==INPUT_LOCAL);
    step(&f);step(&f);
    assert(last_result(&f,released.command_key).state==CP_RESULT_APPLIED);
    finish(&f);
    setup(&f,INPUT_MOUSE);acquire(&f,"script");
    f.now+=3000;app_remote_input_service(f.remote,f.now);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(!view.active && f.engine.state.owner==INPUT_LOCAL && f.engine.state.release_pending);
    finish(&f);
}

static void mouse_keyboard_admission_sent_and_deadman(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    cp_command_t cmd=command(&f,CP_OP_MOUSE_CLICK,"{\"button\":\"right\"}");cp_result_t result;
    assert(app_remote_input_execute(f.remote,&cmd,f.now,&result)==CP_OK && result.state==CP_RESULT_ACCEPTED);
    const size_t reports=f.reports_count;
    assert(app_remote_input_execute(f.remote,&cmd,f.now,&result)==CP_OK && f.engine.count==1);
    step(&f);assert(f.reports_count==reports+1 && f.engine.count==1);
    step(&f);assert(last_result(&f,cmd.command_key).state==CP_RESULT_SENT && f.reports_count==reports+2);
    result=execute(&f,CP_OP_MOUSE_DRAG_BEGIN,"{\"button\":\"left\",\"deadman_ms\":100}");step(&f);
    assert(f.engine.mouse_buttons==1 && last_result(&f,result.command_key).state==CP_RESULT_SENT);
    result=execute(&f,CP_OP_MOUSE_MOVE_RELATIVE,"{\"dx\":10,\"dy\":-5}");step(&f);
    assert(last_result(&f,result.command_key).state==CP_RESULT_SENT && f.reports[f.reports_count-1][0]==1);
    f.engine.congested=true;(void)hid_engine_observe_transport(&f.engine,f.now,true);
    cmd=command(&f,CP_OP_MOUSE_DRAG_END,"{\"button\":\"left\"}");memset(cmd.lease_id,0,sizeof cmd.lease_id);
    assert(app_remote_input_execute(f.remote,&cmd,f.now,&result)==CP_OK && result.state==CP_RESULT_ACCEPTED);
    assert(f.engine.mouse_buttons==1); f.engine.congested=false;
    (void)hid_engine_observe_transport(&f.engine,f.now,false);step(&f);
    assert(last_result(&f,cmd.command_key).state==CP_RESULT_SENT && !f.engine.mouse_buttons);
    result=execute(&f,CP_OP_MOUSE_DRAG_BEGIN,"{\"button\":\"left\",\"deadman_ms\":100}");step(&f);
    f.now+=100;app_remote_input_service(f.remote,f.now);
    assert(f.engine.state.owner==INPUT_LOCAL && f.engine.state.release_pending);finish(&f);
    setup(&f,INPUT_KEYBOARD);acquire(&f,"script");
    result=execute(&f,CP_OP_KEYBOARD_SHORTCUT,"{\"shortcut\":\"copy\"}");step(&f);step(&f);
    assert(last_result(&f,result.command_key).state==CP_RESULT_SENT);
    assert(f.reports[f.reports_count-2][0]==1 && f.reports[f.reports_count-2][2]==6);
    finish(&f);
}

static void preview_shared_selection_atomic_edits_and_batched_send(void)
{
    fixture_t f;setup(&f,INPUT_KEYBOARD);acquire(&f,"selection");
    char args[192];(void)snprintf(args,sizeof args,"{\"cell\":6,\"selection_revision\":\"%" PRIu64 "\"}",f.state.selection_revision);
    (void)execute(&f,CP_OP_SELECTION_ACTIVATE,args);assert(f.model.view.group==6 && !f.model.view.preview_length);
    (void)snprintf(args,sizeof args,"{\"cell\":3,\"selection_revision\":\"%" PRIu64 "\"}",f.state.selection_revision);
    (void)execute(&f,CP_OP_SELECTION_ACTIVATE,args);assert(!strcmp(f.model.view.preview,"1"));
    assert(!interaction_append(&f.model,'X')); /* Native wrapper still rejects foreign ownership. */
    (void)execute(&f,CP_OP_PREVIEW_CLEAR,"{}");
    char text[65];memset(text,'A',64);text[64]=0;
    (void)snprintf(args,sizeof args,"{\"text\":\"%s\",\"draft_revision\":\"%" PRIu64 "\"}",text,f.state.draft_revision);
    (void)execute(&f,CP_OP_PREVIEW_APPEND,args);assert(f.model.view.preview_length==64);
    (void)snprintf(args,sizeof args,"{\"text\":\"X\",\"draft_revision\":\"%" PRIu64 "\"}",f.state.draft_revision);
    cp_command_t rejected=command(&f,CP_OP_PREVIEW_APPEND,args);cp_result_t result;
    assert(app_remote_input_execute(f.remote,&rejected,f.now,&result)==CP_INVALID && f.model.view.preview_length==64);
    (void)snprintf(args,sizeof args,"{\"draft_revision\":\"%" PRIu64 "\"}",f.state.draft_revision);
    result=execute(&f,CP_OP_PREVIEW_SEND,args);const uint64_t key=result.command_key;
    for (unsigned i=0;i<128;++i)step(&f);
    result=last_result(&f,key);
    assert(result.state==CP_RESULT_SENT && result.progress_known && result.started_prefix==64 &&
        result.completed_pairs==64 && result.uncertain_index==-1 && !f.model.view.preview_length);
    finish(&f);
}

static void failed_key_release_never_replays_started_prefix(void)
{
    fixture_t f;setup(&f,INPUT_KEYBOARD);acquire(&f,"script");
    const cp_result_t queued=execute(&f,CP_OP_KEYBOARD_TEXT_ENQUEUE,"{\"text\":\"AB\"}");
    step(&f);f.fail_report=f.reports_count+1;step(&f);
    assert(f.engine.state.owner==INPUT_LOCAL && f.engine.state.release_pending);
    step(&f);step(&f);
    const cp_result_t failed=last_result(&f,queued.command_key);
    assert(failed.state==CP_RESULT_CANCELLED && failed.progress_known && failed.started_prefix==1 &&
        failed.completed_pairs==0 && failed.uncertain_index==0);
    unsigned nonzero=0;
    for(size_t i=0;i<f.reports_count;++i)if(f.report_ids[i]==1 && f.reports[i][2])++nonzero;
    assert(nonzero==1);finish(&f);
}

static void virtual_touch_identity_taps_drag_and_cleanup(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    const char *first="{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}";
    const char *second="{\"gesture_id\":\"00000000000000000000000000000002\",\"x\":100,\"y\":100}";
    (void)execute(&f,CP_OP_TOUCH_BEGIN,first);f.now+=20;
    cp_result_t result=execute(&f,CP_OP_TOUCH_END,first);step(&f);step(&f);
    assert(last_result(&f,result.command_key).state==CP_RESULT_SENT);
    result=execute(&f,CP_OP_TOUCH_BEGIN,second);step(&f);
    assert(f.engine.mouse_buttons==1 && interaction_touch_holding(&f.model));
    cp_command_t stale=command(&f,CP_OP_TOUCH_CANCEL,first);memset(stale.lease_id,0,sizeof stale.lease_id);
    assert(app_remote_input_execute(f.remote,&stale,f.now,&result)==CP_STALE && f.engine.mouse_buttons==1);
    cp_command_t end=command(&f,CP_OP_TOUCH_END,second);
    assert(app_remote_input_execute(f.remote,&end,f.now,&result)==CP_OK);step(&f);
    assert(!f.engine.mouse_buttons && !interaction_touch_holding(&f.model));
    cp_command_t reused=command(&f,CP_OP_TOUCH_BEGIN,first);
    assert(app_remote_input_execute(f.remote,&reused,f.now,&result)==CP_STALE);
    finish(&f);
}

static void mutable_gates_delegation_and_completion_backpressure(void)
{
    for(unsigned fault=0;fault<8;++fault) {
        fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
        cp_command_t cmd=command(&f,CP_OP_MOUSE_CLICK,"{\"button\":\"left\"}");cp_result_t result;
        if(fault==0)--cmd.input_epoch;
        if(fault==1)--cmd.transport_generation;
        if(fault==2)strcpy(cmd.target_id,"other");
        if(fault==3)++cmd.lease_id[0];
        if(fault==4)++cmd.identity.session_id[0];
        if(fault==5)cmd.deadline_ms=f.now;
        if(fault==6)f.state.local_busy=true;
        if(fault==7)hid_engine_set_locked(&f.engine,true);
        assert(app_remote_input_execute(f.remote,&cmd,f.now,&result)!=CP_OK);
        assert(!f.engine.count);finish(&f);
    }
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    cp_command_t ui=command(&f,CP_OP_UI_NAVIGATE,"{\"overlay\":\"settings\"}");cp_result_t result;
    --ui.ui_revision;
    assert(app_remote_input_execute(f.remote,&ui,f.now,&result)==CP_STALE && !f.delegated);
    (void)execute(&f,CP_OP_UI_NAVIGATE,"{\"overlay\":\"settings\"}");assert(f.delegated==1);
    (void)execute(&f,CP_OP_MODE_SELECT,"{\"mode\":\"keyboard\"}");
    step(&f);step(&f);app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(view.active && f.engine.state.mode==INPUT_KEYBOARD);
    f.block_completion=true;result=execute(&f,CP_OP_KEYBOARD_KEY_TAP,"{\"usage\":40}");
    const size_t before=f.reports_count;step(&f);step(&f);
    for(unsigned i=0;i<10;++i)app_remote_input_service(f.remote,f.now);
    assert(f.reports_count==before+2);
    f.block_completion=false;app_remote_input_service(f.remote,f.now);
    assert(last_result(&f,result.command_key).state==CP_RESULT_SENT);finish(&f);
}

static void safety_bypasses_deadline_provider_and_revision_without_new_target_effects(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_MOUSE_DRAG_BEGIN,"{\"button\":\"left\"}");step(&f);
    cp_command_t stop=command(&f,CP_OP_INPUT_RELEASE_ALL,"{}");
    stop.deadline_ms=f.now;memset(stop.lease_id,0,sizeof stop.lease_id);
    f.observe_failure=true;f.state.ui_revision=UINT64_MAX;
    cp_result_t result;
    assert(app_remote_input_execute(f.remote,&stop,f.now,&result)==CP_OK);
    assert(result.state==CP_RESULT_CLEANUP_PENDING && f.engine.state.owner==INPUT_LOCAL && f.engine.state.release_pending);
    f.observe_failure=false;step(&f);step(&f);
    assert(!f.engine.mouse_buttons);finish(&f);
    setup(&f,INPUT_MOUSE);acquire(&f,"script");
    stop=command(&f,CP_OP_INPUT_RELEASE_ALL,"{}");stop.deadline_ms=0;
    ++f.state.transport_generation;strcpy(f.state.target_id,"replacement");
    hid_engine_cancel(&f.engine);(void)hid_engine_set_owner(&f.engine,INPUT_LOCAL);
    step(&f);step(&f);
    assert(hid_engine_enqueue_mouse_move(&f.engine,f.engine.state.epoch,INPUT_LOCAL,9,8)==HID_ENGINE_ENQUEUE_OK);
    const uint64_t epoch=f.engine.state.epoch;
    assert(app_remote_input_execute(f.remote,&stop,f.now,&result)==CP_OK);
    assert(result.state==CP_RESULT_FAILED && result.error==CP_STALE && f.engine.state.epoch==epoch && f.engine.count==1);
    finish(&f);
}

static void source_aware_motion_and_virtual_timeout_use_the_same_model(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);
    for(uint64_t i=0;i<64;++i) {
        const interaction_imu_sample_t s={.timestamp_us=100000+i*10000,.az_g=1};
        (void)interaction_motion_sample(&f.model,&s);
    }
    assert(f.model.view.calibrated);
    interaction_config_t motion_config=f.model.config;motion_config.filter_seconds=0;
    assert(interaction_configure(&f.model,&motion_config));
    f.now=740;acquire(&f,"device_imu");
    (void)execute(&f,CP_OP_INPUT_MOTION_SET,"{\"enabled\":true}");
    assert(f.engine.state.gyro_enabled && f.model.view.motion_enabled && f.model.motion_source==INPUT_REMOTE);
    interaction_imu_sample_t s={.timestamp_us=f.now*1000,.az_g=1};
    (void)interaction_motion_sample_owned(&f.model,INPUT_REMOTE,&s);
    while(s.timestamp_us/1000 <= f.model.freeze_until_ms) {
        s.timestamp_us+=10000;(void)interaction_motion_sample_owned(&f.model,INPUT_REMOTE,&s);
    }
    s.timestamp_us+=10000;
    s.gz_dps=25;
    f.now=s.timestamp_us/1000;
    assert(app_remote_input_motion_sample(f.remote,&s,f.now));
    assert(f.engine.count==1 && f.engine.queue[f.engine.head].owner==INPUT_REMOTE &&
        f.engine.queue[f.engine.head].data[1]==1 && f.engine.queue[f.engine.head].data[2]==0);
    step(&f);
    assert(!hid_engine_set_gyro(&f.engine,true)); /* Native source cannot enable foreign motion. */
    cp_command_t script=command(&f,CP_OP_MOUSE_MOVE_RELATIVE,"{\"dx\":1,\"dy\":0}");cp_result_t result;
    assert(app_remote_input_execute(f.remote,&script,f.now,&result)==CP_BUSY);
    finish(&f);
    setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_TOUCH_BEGIN,"{\"gesture_id\":\"00000000000000000000000000000001\",\"deadman_ms\":100}");
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);assert(view.virtual_touch_active);
    f.now+=100;app_remote_input_service(f.remote,f.now);
    app_remote_input_snapshot(f.remote,&view);
    assert(!view.virtual_touch_active && !view.active && f.engine.state.release_pending);finish(&f);
}

static void actual_sample_routing_selection_virtual_device_touch_and_motion_clutch(void)
{
    fixture_t f;setup(&f,INPUT_KEYBOARD);
    for(uint64_t i=0;i<64;++i) {
        const interaction_imu_sample_t s={.timestamp_us=100000+i*10000,.az_g=1};
        (void)interaction_motion_sample(&f.model,&s);
    }
    f.now=740;acquire(&f,"selection");
    const size_t reports=f.reports_count;const uint64_t revision=f.state.selection_revision;
    for(unsigned i=0;i<40;++i) {
        f.now+=10;
        const float angle=(float)(f.now-730)*.0017453292519943295f;
        const interaction_imu_sample_t s={.timestamp_us=f.now*1000,.gx_dps=100,
            .ay_g=sinf(angle),.az_g=cosf(angle)};
        assert(app_remote_input_motion_sample(f.remote,&s,f.now));
    }
    assert(f.model.view.focus==5 && f.state.selection_revision>revision && !f.model.view.preview_length &&
        !f.engine.count && f.reports_count==reports);
    finish(&f);
    setup(&f,INPUT_MOUSE);acquire(&f,"device_touch");
    const char *contact="{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}";
    (void)execute(&f,CP_OP_TOUCH_BEGIN,contact);
    cp_result_t moved=execute(&f,CP_OP_TOUCH_MOVE,
        "{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":20}");step(&f);
    assert(last_result(&f,moved.command_key).state==CP_RESULT_SENT &&
        f.reports[f.reports_count-1][1]==20 && !f.model.view.touch_active && f.engine.state.owner==INPUT_REMOTE);
    (void)execute(&f,CP_OP_TOUCH_END,
        "{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":120,\"y\":100}");
    finish(&f);
    setup(&f,INPUT_MOUSE);
    for(uint64_t i=0;i<64;++i) {
        const interaction_imu_sample_t s={.timestamp_us=100000+i*10000,.az_g=1};
        (void)interaction_motion_sample(&f.model,&s);
    }
    f.now=740;acquire(&f,"device_imu");
    (void)execute(&f,CP_OP_INPUT_MOTION_SET,"{\"enabled\":true}");
    (void)execute(&f,CP_OP_TOUCH_BEGIN,contact);
    for(unsigned i=0;i<20;++i) {
        f.now+=10;
        const interaction_imu_sample_t s={.timestamp_us=f.now*1000,.gz_dps=25,.az_g=1};
        assert(app_remote_input_motion_sample(f.remote,&s,f.now) && !f.engine.count);
    }
    const cp_result_t tap=execute(&f,CP_OP_TOUCH_END,contact);step(&f);step(&f);
    assert(last_result(&f,tap.command_key).state==CP_RESULT_SENT && f.model.view.motion_enabled);
    finish(&f);
}

static void late_remote_revocation_preserves_fresh_local_touch(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_CONTROL_RELEASE,"{}");step(&f);step(&f);
    interaction_sync(&f.model,&f.engine.state,true,f.now); /* Native coordinator refresh after recovery. */
    f.model.view.touch_active=true;
    const interaction_touch_event_t touch={.phase=INTERACTION_TOUCH_BEGIN,.gesture=10,
        .epoch=f.engine.state.epoch,.ui_revision=f.state.ui_revision,.timestamp_ms=f.now,.x=100,.y=100};
    (void)interaction_touch_input(&f.model,&touch,f.now,f.state.ui_revision,true);
    assert(f.model.touch.active && f.model.touch.source==INPUT_LOCAL);
    const cp_identity_t old={.principal_id={1},.session_id={2},.scopes=CP_SCOPE_ALL};
    const uint64_t epoch=f.engine.state.epoch;
    app_remote_input_revoke(f.remote,CP_SAFETY_SESSION_LOST,&old,f.now);
    assert(f.model.touch.active && f.engine.state.epoch==epoch);
    finish(&f);
}

static void renewal_preserves_virtual_contact_and_safety_has_reserved_capacity(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    (void)execute(&f,CP_OP_TOUCH_BEGIN,"{\"gesture_id\":\"00000000000000000000000000000001\"}");
    const uint64_t ui=f.state.ui_revision;
    char args[96];(void)snprintf(args,sizeof args,"{\"state_sequence\":\"%" PRIu64 "\"}",f.state.state_sequence);
    (void)execute(&f,CP_OP_CONTROL_RENEW,args);step(&f);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(view.active && view.virtual_touch_active && f.state.ui_revision==ui);
    cp_result_t result;
    cp_command_t cancel=command(&f,CP_OP_TOUCH_CANCEL,"{\"gesture_id\":\"00000000000000000000000000000001\"}");
    memset(cancel.lease_id,0,sizeof cancel.lease_id);cancel.deadline_ms=0;
    assert(app_remote_input_execute(f.remote,&cancel,f.now,&result)==CP_OK && !f.model.touch.active);
    finish(&f);
    setup(&f,INPUT_MOUSE);acquire(&f,"script");f.block_completion=true;
    for(unsigned i=0;i<APP_REMOTE_INPUT_JOBS;++i) {
        (void)execute(&f,CP_OP_MOUSE_CLICK,"{\"button\":\"left\"}");step(&f);step(&f);
    }
    cp_command_t full=command(&f,CP_OP_MOUSE_CLICK,"{\"button\":\"left\"}");
    assert(app_remote_input_execute(f.remote,&full,f.now,&result)==CP_BUSY && !f.engine.count);
    const cp_result_t stop=execute(&f,CP_OP_INPUT_RELEASE_ALL,"{}");
    assert(stop.state==CP_RESULT_CLEANUP_PENDING && f.engine.state.release_pending);
    const size_t before=f.reports_count;step(&f);step(&f);
    f.block_completion=false;app_remote_input_service(f.remote,f.now);
    assert(last_result(&f,stop.command_key).state==CP_RESULT_APPLIED && f.reports_count==before+2);
    finish(&f);
}

static void authorized_mode_transition_settles_prior_input_without_revoking_lease(void)
{
    fixture_t f;setup(&f,INPUT_KEYBOARD);acquire(&f,"script");
    cp_result_t text=execute(&f,CP_OP_KEYBOARD_TEXT_ENQUEUE,"{\"text\":\"AB\"}");step(&f);
    (void)execute(&f,CP_OP_MODE_SELECT,"{\"mode\":\"mouse\"}");
    step(&f);step(&f);
    cp_result_t ended=last_result(&f,text.command_key);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(ended.state==CP_RESULT_CANCELLED && ended.started_prefix==1 && ended.completed_pairs==0 &&
        ended.uncertain_index==0 && view.active && f.engine.state.owner==INPUT_REMOTE);
    cp_result_t move=execute(&f,CP_OP_MOUSE_MOVE_RELATIVE,"{\"dx\":3,\"dy\":-2}");step(&f);
    assert(last_result(&f,move.command_key).state==CP_RESULT_SENT);
    finish(&f);
}

static void submode_transition_settles_prior_input_without_revoking_lease(void)
{
    fixture_t f;setup(&f,INPUT_KEYBOARD);acquire(&f,"script");
    const cp_result_t text=execute(&f,CP_OP_KEYBOARD_TEXT_ENQUEUE,"{\"text\":\"AB\"}");step(&f);
    const uint64_t old_epoch=f.engine.state.epoch;
    const cp_result_t submode=execute(&f,CP_OP_KEYBOARD_SUBMODE_SELECT,"{\"submode\":\"touch\"}");
    assert(submode.state==CP_RESULT_APPLIED && f.engine.state.epoch>old_epoch);
    step(&f);step(&f);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    const cp_result_t ended=last_result(&f,text.command_key);
    assert(ended.state==CP_RESULT_CANCELLED && ended.started_prefix==1 && ended.completed_pairs==0 &&
        ended.uncertain_index==0 && view.active && f.engine.state.owner==INPUT_REMOTE);
    const cp_result_t key=execute(&f,CP_OP_KEYBOARD_KEY_TAP,"{\"usage\":40}");step(&f);step(&f);
    assert(last_result(&f,key.command_key).state==CP_RESULT_SENT);
    finish(&f);
}

static void ownership_precedes_availability_replay_and_lease_effects(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");
    const cp_operation_id_t foreign[]={CP_OP_BLUETOOTH_PAIRING_OPEN,CP_OP_AUDIO_MUTE_SET,
        CP_OP_AUDIO_CAPTURE_STOP,CP_OP_DEVICE_LOCK,0,UINT16_MAX};
    cp_result_t result;char reason[CP_REASON_CAPACITY];
    const uint64_t epoch=f.engine.state.epoch;
    const size_t completions=f.completions_count;
    for(size_t i=0;i<sizeof foreign/sizeof foreign[0];++i) {
        assert(!app_remote_input_owns(foreign[i]));
        assert(!app_remote_input_available(f.remote,foreign[i],reason));
        cp_command_t cmd=command(&f,foreign[i],"{}");cmd.command_key=UINT64_MAX;
        assert(app_remote_input_execute(f.remote,&cmd,f.now,&result)==CP_NOT_IMPLEMENTED);
        assert(result.error==CP_NOT_IMPLEMENTED && result.command_key==UINT64_MAX);
    }
    assert(f.engine.state.epoch==epoch && !f.engine.state.release_pending &&
        f.completions_count==completions && !f.delegated);
    const cp_result_t moved=execute(&f,CP_OP_MOUSE_MOVE_RELATIVE,"{\"dx\":1,\"dy\":0}");step(&f);
    assert(last_result(&f,moved.command_key).state==CP_RESULT_SENT);
    f.delegate_enabled=false;
    assert(app_remote_input_owns(CP_OP_UI_NAVIGATE));
    assert(!app_remote_input_available(f.remote,CP_OP_UI_NAVIGATE,reason));
    cp_command_t ui=command(&f,CP_OP_UI_NAVIGATE,"{\"overlay\":\"settings\"}");
    assert(app_remote_input_execute(f.remote,&ui,f.now,&result)==CP_UNAVAILABLE && !f.delegated);
    f.now+=3500;
    cp_command_t foreign_stop=command(&f,CP_OP_AUDIO_CAPTURE_STOP,"{}");
    assert(app_remote_input_execute(f.remote,&foreign_stop,f.now,&result)==CP_NOT_IMPLEMENTED);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(view.active && f.engine.state.owner==INPUT_REMOTE && !f.engine.state.release_pending);
    app_remote_input_service(f.remote,f.now);app_remote_input_snapshot(f.remote,&view);
    assert(!view.active && f.engine.state.owner==INPUT_LOCAL && f.engine.state.release_pending);
    finish(&f);
}

static void rejected_virtual_end_is_consumed_without_contact_or_click_replay(void)
{
    fixture_t f;setup(&f,INPUT_MOUSE);acquire(&f,"script");f.block_completion=true;
    const char *begin="{\"gesture_id\":\"00000000000000000000000000000001\",\"x\":100,\"y\":100}";
    (void)execute(&f,CP_OP_TOUCH_BEGIN,begin);
    for(unsigned i=0;i<APP_REMOTE_INPUT_JOBS;++i) {
        (void)execute(&f,CP_OP_TOUCH_MOVE,i%2 ?
            "{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":-1}" :
            "{\"gesture_id\":\"00000000000000000000000000000001\",\"dx\":1}");step(&f);
    }
    cp_command_t end=command(&f,CP_OP_TOUCH_END,begin);cp_result_t result;
    const size_t count=f.reports_count;
    assert(app_remote_input_execute(f.remote,&end,f.now,&result)==CP_BUSY);
    app_remote_input_view_t view;app_remote_input_snapshot(f.remote,&view);
    assert(!view.virtual_touch_active && !f.model.touch.active && !f.engine.count);
    f.block_completion=false;app_remote_input_service(f.remote,f.now);
    assert(app_remote_input_execute(f.remote,&end,f.now,&result)==CP_BUSY && f.reports_count==count);
    (void)execute(&f,CP_OP_TOUCH_BEGIN,
        "{\"gesture_id\":\"00000000000000000000000000000002\",\"x\":100,\"y\":100}");
    assert(!interaction_touch_holding(&f.model) && !f.engine.mouse_buttons);
    finish(&f);
}

int main(void)
{
    lease_barrier_renewal_and_safety();
    mouse_keyboard_admission_sent_and_deadman();
    preview_shared_selection_atomic_edits_and_batched_send();
    failed_key_release_never_replays_started_prefix();
    virtual_touch_identity_taps_drag_and_cleanup();
    mutable_gates_delegation_and_completion_backpressure();
    safety_bypasses_deadline_provider_and_revision_without_new_target_effects();
    source_aware_motion_and_virtual_timeout_use_the_same_model();
    late_remote_revocation_preserves_fresh_local_touch();
    renewal_preserves_virtual_contact_and_safety_has_reserved_capacity();
    authorized_mode_transition_settles_prior_input_without_revoking_lease();
    submode_transition_settles_prior_input_without_revoking_lease();
    ownership_precedes_availability_replay_and_lease_effects();
    rejected_virtual_end_is_consumed_without_contact_or_click_replay();
    actual_sample_routing_selection_virtual_device_touch_and_motion_clutch();
    puts("PASS: canonical remote leases, shared input/model handlers, async pair progress, virtual touch and safety gates");
    return 0;
}
