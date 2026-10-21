// SPDX-License-Identifier: Apache-2.0
/* Include in the sole coordinator after remote_observe/remote_complete and the
 * native semantic helpers. Main calls init/service and routes owned operations,
 * safety notifications and cached reads here. No second media owner or loop. */
#include "app_remote_media_runtime_port.h"
#include "board_audio_mosaico.h"
#include "app_recording_store.h"

static app_remote_media_t *s_remote_media;
static interaction_t *s_remote_media_preview;
static app_audio_result_t s_remote_media_immediate;
static bool s_remote_media_immediate_ready;

static bool remote_media_session(void *context, const cp_identity_t *identity)
{
    (void)context;
    return app_control_runtime_session_alive(identity->session_id,identity->principal_id);
}
static cp_error_t remote_media_authority(void *context, const cp_capture_authority_request_t *request,
    cp_capture_authority_t *output)
{ (void)context; return app_control_runtime_capture_authority(request,output); }
static bool remote_media_grant(void *context, const uint8_t id[CP_ID_BYTES], cp_capture_grant_t *output)
{ (void)context; return app_control_trust_grant(id,output); }
static bool remote_media_random(void *context, uint8_t *bytes, size_t count)
{ (void)context; esp_fill_random(bytes,count); return true; }
static bool remote_media_observe(void *context, app_remote_media_state_t *output)
{
    (void)context;
    app_media_snapshot_t current;
    if (!app_media_runtime_snapshot(&current) || !s_control_state_published) return false;
    *output=(app_remote_media_state_t){.device=s_published_control_state,.audio_known=current.audio_known,
        .speech_known=current.speech_known,.audio=current.audio,.speech=current.speech,
        .privacy_latched=s_engine.state.locked,.maintenance=remote_update_maintenance(),.speech_context=s_engine.state.mode==INPUT_KEYBOARD &&
            s_keyboard_submode==APP_KEYBOARD_SPOKEN && s_overlay==APP_OVERLAY_NONE};
    ae_codec_port_t codec=board_audio_mosaico_port();
    if (codec.capabilities) (void)codec.capabilities(codec.context,&output->codec);
    app_recording_store_snapshot_t storage;
    if (app_recording_store_snapshot(&storage)) output->store=storage.capabilities;
    for (unsigned i=AE_ROUTE_LOCAL;i<AE_ROUTE_COUNT;++i)
        (void)app_media_runtime_route((ae_route_t)i,&output->routes[i]);
    return true;
}
static bool remote_media_candidate_context(void *context, const cp_command_t *command)
{
    (void)context;
    app_remote_input_view_t owner={0}; app_remote_input_snapshot(s_remote_input,&owner);
    app_remote_input_state_t current;
    if (!remote_observe(NULL,&current)) return false;
    return !remote_update_maintenance() && owner.active && owner.lease_expires_ms>s_now_ms && command->lease_expires_ms<=owner.lease_expires_ms &&
        !memcmp(owner.lease_id,command->lease_id,CP_ID_BYTES) &&
        !memcmp(owner.owner_session,command->identity.session_id,CP_ID_BYTES) &&
        !memcmp(owner.owner_principal,command->identity.principal_id,CP_ID_BYTES) &&
        command->input_epoch==s_engine.state.epoch && command->ui_revision==s_ui_revision &&
        command->transport_generation==current.transport_generation && !strcmp(command->target_id,current.target_id) &&
        s_engine.state.owner==INPUT_REMOTE && !s_engine.state.locked && s_engine.ready && !s_engine.state.release_pending &&
        !current.local_busy && !current.transition_busy && !current.native_send_active && current.screen_awake &&
        s_engine.state.mode==INPUT_KEYBOARD && s_keyboard_submode==APP_KEYBOARD_SPOKEN && s_overlay==APP_OVERLAY_NONE &&
        s_interaction.view.preview_length<INTERACTION_PREVIEW_CAPACITY && s_ui_revision!=UINT64_MAX;
}
static bool remote_media_owner_available(void *context, cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{
    (void)context; reason[0]=0;
    app_media_snapshot_t current;
    if (!app_media_runtime_snapshot(&current) || !current.initialized) {
        (void)snprintf(reason,CP_REASON_CAPACITY,"media_owner_unavailable"); return false;
    }
    if (operation==CP_OP_AUDIO_CAPABILITIES || operation==CP_OP_AUDIO_STATE || operation==CP_OP_AUDIO_KIND_SELECT ||
        operation==CP_OP_SPEECH_STATE || operation==CP_OP_RECORDINGS_GET || operation==CP_OP_RECORDINGS_LIST ||
        operation==CP_OP_RECORDING_READ || operation==CP_OP_RECORDING_DELETE) return true;
    if (operation==CP_OP_SPEECH_LISTEN_START || operation==CP_OP_SPEECH_LISTEN_STOP ||
        operation==CP_OP_SPEECH_CANDIDATE_CONFIRM || operation==CP_OP_SPEECH_CANDIDATE_REJECT) {
        if (current.speech_known && current.speech.state!=SL_UNAVAILABLE && current.speech.state!=SL_FAULT) return true;
        (void)snprintf(reason,CP_REASON_CAPACITY,"speech_model_unavailable"); return false;
    }
    ae_codec_port_t codec=board_audio_mosaico_port(); ae_codec_capabilities_t caps={0};
    if (!codec.capabilities || codec.capabilities(codec.context,&caps)!=AE_OK || !caps.available) {
        (void)snprintf(reason,CP_REASON_CAPACITY,"codec_unavailable"); return false;
    }
    if (operation==CP_OP_RECORDING_START) {
        app_recording_store_snapshot_t storage;
        if (!app_recording_store_snapshot(&storage) || !storage.capabilities.available) {
            (void)snprintf(reason,CP_REASON_CAPACITY,"recording_store_unavailable"); return false;
        }
    }
    return true;
}
static ae_result_t remote_media_submit(void *context, const app_remote_media_request_t *request)
{
    (void)context;
    if (s_remote_media_immediate_ready) return AE_BUSY;
    if (request->action!=APP_REMOTE_MEDIA_KIND && request->action!=APP_REMOTE_MEDIA_SPEECH_CONFIRM &&
        request->action!=APP_REMOTE_MEDIA_SPEECH_REJECT) return app_media_runtime_submit_remote(request);
    ae_result_t result=AE_OK;
    if (request->action==APP_REMOTE_MEDIA_KIND) {
        if (s_ui_revision==UINT64_MAX || s_engine.state.locked) return AE_DENIED;
        app_audio_kind_t kind=request->role==AE_MICROPHONE ? APP_AUDIO_VOICE : APP_AUDIO_SPEAKER;
        if (kind!=s_audio_kind) { s_audio_kind=kind; ++s_ui_revision; sync_interaction(); }
    } else if (request->action==APP_REMOTE_MEDIA_SPEECH_REJECT) {
        result=app_media_runtime_reject(request->candidate_token,request->input_epoch,request->ui_revision);
    } else {
        /* Admission just checked the exact shared lease/UI context. This is
         * one serialized coordinator turn, not a later unguarded worker edit. */
        if (!s_remote_media_preview || s_ui_revision==UINT64_MAX || remote_update_maintenance()) return AE_BUSY;
        app_media_snapshot_t current;
        if (!app_media_runtime_snapshot(&current) || !current.speech_known || current.speech.state!=SL_CANDIDATE ||
            current.speech.token!=request->candidate_token || current.speech.ui_revision!=request->candidate_revision) return AE_STALE;
        const char expected=current.speech.candidate;
        interaction_edit_t edit={.kind=INTERACTION_EDIT_APPEND,.text=&expected,.length=1};
        /* Validate the actual shared edit against a private proposal before
         * consuming a candidate. The coordinator is the sole model owner;
         * the successful proposal can then commit without another fallible
         * append or an intermediate preview/HID effect. */
        *s_remote_media_preview=s_interaction;
        if (!interaction_edit_owned(s_remote_media_preview,INPUT_REMOTE,&edit)) {
            memset(s_remote_media_preview,0,sizeof(*s_remote_media_preview)); return AE_BUSY;
        }
        char candidate=0;
        result=app_media_runtime_confirm(request->candidate_token,request->input_epoch,request->ui_revision,&candidate);
        if (result==AE_OK && candidate!=expected) result=AE_INTEGRITY;
        if (result==AE_OK) { s_interaction=*s_remote_media_preview; ++s_ui_revision; sync_interaction(); }
        memset(s_remote_media_preview,0,sizeof(*s_remote_media_preview));
    }
    if (result!=AE_OK) return result;
    s_remote_media_immediate=(app_audio_result_t){.request_token=request->token,.result=AE_OK};
    s_remote_media_immediate_ready=true; return AE_OK;
}
static bool remote_media_poll(void *context, uint64_t token, app_audio_result_t *output)
{
    (void)context;
    if (s_remote_media_immediate_ready && s_remote_media_immediate.request_token==token) {
        *output=s_remote_media_immediate; return true;
    }
    return app_media_runtime_poll_remote(token,output);
}
static void remote_media_release(void *context, uint64_t token)
{
    (void)context;
    if (s_remote_media_immediate_ready && s_remote_media_immediate.request_token==token) {
        s_remote_media_immediate_ready=false; memset(&s_remote_media_immediate,0,sizeof(s_remote_media_immediate));
    } else app_media_runtime_release_remote_result(token);
}
static ae_result_t remote_media_copy_records(void *context, uint64_t token, ae_record_metadata_t *items, size_t capacity)
{ (void)context; return app_media_runtime_copy_remote_records(token,items,capacity); }
static ae_result_t remote_media_copy_data(void *context, uint64_t token, size_t offset, void *output, size_t bytes)
{ (void)context; return app_media_runtime_copy_remote_data(token,offset,output,bytes); }
static void remote_media_cancel(void *context, uint64_t token, const cp_identity_t *identity, ae_stop_reason_t reason)
{ (void)context; app_media_runtime_cancel_remote(token,identity,reason); }
static cp_error_t remote_media_recordings(void *context, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{ (void)context; return app_media_runtime_read_recordings(identity,resource,json,capacity,length); }
static void remote_media_init(void)
{
    app_media_remote_authority_t authority={.session_alive=remote_media_session,.capture_authority=remote_media_authority};
    if (app_media_runtime_set_remote_authority(&authority)!=AE_OK) return;
    void *memory=heap_caps_calloc(1,app_remote_media_bytes(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_remote_media_preview=heap_caps_calloc(1,sizeof(*s_remote_media_preview),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    app_remote_media_port_t port={.observe=remote_media_observe,.available=remote_media_owner_available,
        .random=remote_media_random,.session_alive=remote_media_session,.grant=remote_media_grant,
        .capture_authority=remote_media_authority,.submit=remote_media_submit,.poll=remote_media_poll,
        .release_result=remote_media_release,.copy_records=remote_media_copy_records,.copy_record_data=remote_media_copy_data,
        .cancel=remote_media_cancel,.complete=remote_complete,
        .read_recordings=remote_media_recordings,.candidate_context=remote_media_candidate_context};
    if (!memory || !s_remote_media_preview || app_remote_media_init(memory,app_remote_media_bytes(),&port,&s_remote_media)!=CP_OK) {
        heap_caps_free(memory); heap_caps_free(s_remote_media_preview); s_remote_media_preview=NULL; s_remote_media=NULL;
    }
}
