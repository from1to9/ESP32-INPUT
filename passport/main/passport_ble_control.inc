// SPDX-License-Identifier: Apache-2.0
// Included by the sole NimBLE owner. Remote changes reuse its native transition
// and neutral-release path, but retain a command-specific observed result.
static void remote_management_cancel(cp_error_t error)
{
    if (!remote_management.key || remote_management.done) return;
    if (transition) transition=false;
    (void)stop_advertising();
    remote_management.done=true; remote_management.error=error;
}
cp_error_t passport_ble_remote_begin(const cp_command_t *c, uint64_t now, cp_result_t *out)
{
    if (remote_management.key || transition) return CP_BUSY;
    if (engine.state.locked) return CP_LOCKED;
    if (!state.synced || !state.storage_known) return CP_UNAVAILABLE;
    if (c->resource_revision!=state.bond_revision) return CP_STALE;
    char generation[24];
    if (cp_command_string(c,"generation",generation,sizeof(generation))) {
        char actual[24]; snprintf(actual,sizeof(actual),"%u",(unsigned)state.generation);
        if (strcmp(generation,actual)) return CP_STALE;
    } else return CP_INVALID;
    char unused[65];
    if (cp_command_string(c,"confirmation_id",unused,sizeof(unused))) return CP_INVALID;
    bool targets_host=c->operation==CP_OP_BLUETOOTH_TARGET_REQUEST || c->operation==CP_OP_BLUETOOTH_DISCONNECT;
    if (!targets_host && cp_command_string(c,"peer_id",unused,sizeof(unused))) return CP_INVALID;
    int64_t seconds=60;
    if (cp_command_integer(c,"window_seconds",&seconds) && c->operation!=CP_OP_BLUETOOTH_PAIRING_OPEN && c->operation!=CP_OP_BLUETOOTH_TARGET_REQUEST) return CP_INVALID;
    passport_command_t request={.generation=state.generation,.bond_revision=state.bond_revision,
        .window_ms=(uint32_t)seconds*1000};
    char target[PASSPORT_PEER_TEXT]={0};
    if (c->operation==CP_OP_BLUETOOTH_TARGET_REQUEST || c->operation==CP_OP_BLUETOOTH_DISCONNECT) {
        char id[65];
        if (!cp_command_string(c,"peer_id",id,sizeof(id))) return CP_INVALID;
        unsigned matches=0;
        for (unsigned i=0;i<state.peer_count;++i) {
            char saved[33]; bool nonzero=false;
            for (unsigned j=0;j<16;++j) { snprintf(saved+j*2,3,"%02x",state.peers[i].bond_id[j]); nonzero|=state.peers[i].bond_id[j]!=0; }
            if (!nonzero || strcmp(id,saved)) continue;
            ++matches; request.peer_type=state.peers[i].type; memcpy(request.peer,state.peers[i].address,6);
            memcpy(target,state.peers[i].label,sizeof(target));
        }
        if (matches!=1) return CP_STALE;
        if (c->operation==CP_OP_BLUETOOTH_DISCONNECT && (!state.connected || strcmp(target,state.current))) return CP_STALE;
        request.kind=c->operation==CP_OP_BLUETOOTH_DISCONNECT ? PASSPORT_DISCONNECT : PASSPORT_ADVERTISE;
    } else if (c->operation==CP_OP_BLUETOOTH_PAIRING_OPEN) {
        if (state.peer_count>=PASSPORT_PEERS) return CP_BUSY;
        request.kind=PASSPORT_PAIR;
    } else if (c->operation!=CP_OP_BLUETOOTH_PAIRING_CLOSE && c->operation!=CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE) return CP_UNAVAILABLE;
    uint64_t original_window=window_end;
    if (c->operation==CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE) {
        if (!new_connection || connection==BLE_HS_CONN_HANDLE_NONE || !state.pairing) return CP_UNAVAILABLE;
        reconnect_end=0;
        if (disconnect_link()) return CP_UNAVAILABLE;
    } else if (c->operation==CP_OP_BLUETOOTH_PAIRING_CLOSE) {
        reconnect_end=0;
        if (!stop_advertising()) return CP_UNAVAILABLE;
        if (new_connection && connection!=BLE_HS_CONN_HANDLE_NONE && disconnect_link()) return CP_UNAVAILABLE;
    } else {
        command(&request,now);
        if (!transition) return CP_UNAVAILABLE;
    }
    remote_management.key=c->command_key; remote_management.operation=c->operation;
    remote_management.deadline=c->operation==CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE ? original_window : now+request.window_ms+3000;
    remote_management.done=false; remote_management.started=false; remote_management.uncertain=false; remote_management.wrong_host=false; remote_management.error=CP_OK;
    memcpy(remote_management.target,target,sizeof(target));
    *out=(cp_result_t){.command_key=c->command_key,.state=CP_RESULT_ACCEPTED,.uncertain_index=-1};
    strcpy(out->reason,"waiting_for_bluetooth");
    return CP_OK;
}
bool passport_ble_remote_poll(uint64_t now, cp_result_t *out)
{
    if (!remote_management.key) return false;
    if (!remote_management.done && engine.state.locked) remote_management_cancel(CP_LOCKED);
    cp_operation_id_t op=remote_management.operation;
    if (!remote_management.done && !transition) {
        if (op==CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE && !state.connected) {
            if (now<remote_management.deadline) advertise(true,(uint32_t)(remote_management.deadline-now));
            remote_management.error=now>=remote_management.deadline ? CP_EXPIRED : state.advertising ? CP_OK : CP_UNAVAILABLE;
            remote_management.done=true;
        } else if (op==CP_OP_BLUETOOTH_PAIRING_CLOSE && !new_connection) remote_management.done=true;
        else if (op==CP_OP_BLUETOOTH_DISCONNECT && !state.connected) remote_management.done=true;
        else if (op==CP_OP_BLUETOOTH_PAIRING_OPEN) {
            remote_management.done=true;
            remote_management.error=state.pairing && state.advertising ? CP_OK : CP_UNAVAILABLE;
        } else if (op==CP_OP_BLUETOOTH_TARGET_REQUEST) {
            if (state.ready && !strcmp(state.current,remote_management.target)) remote_management.done=true;
            else if (!state.advertising && !state.connected) {
                remote_management.error=remote_management.started ? CP_EXPIRED : CP_UNAVAILABLE;
                remote_management.done=true;
            } else remote_management.started=true;
        }
    }
    if (!remote_management.done && now>=remote_management.deadline) remote_management_cancel(CP_EXPIRED);
    if (!remote_management.done) return false;
    cp_error_t error=remote_management.error;
    // A disconnect after an unconfirmed neutral report is never clean success.
    if (error==CP_OK && remote_management.uncertain && (op==CP_OP_BLUETOOTH_TARGET_REQUEST || op==CP_OP_BLUETOOTH_DISCONNECT || op==CP_OP_BLUETOOTH_PAIRING_OPEN)) error=CP_RESULT_UNKNOWN;
    *out=(cp_result_t){.command_key=remote_management.key,.error=error,
        .state=error==CP_OK ? CP_RESULT_APPLIED : CP_RESULT_FAILED,.uncertain_index=-1};
    snprintf(out->reason,sizeof(out->reason),"%s",error==CP_OK ? (op==CP_OP_BLUETOOTH_TARGET_REQUEST ? "selected_host_ready" : "bluetooth_change_observed") :
        remote_management.wrong_host ? "unexpected_host_rejected" : error==CP_EXPIRED ? "host_or_window_timeout" : error==CP_RESULT_UNKNOWN ? "release_outcome_unknown" : "bluetooth_change_not_completed");
    memset(&remote_management,0,sizeof(remote_management));
    return true;
}
