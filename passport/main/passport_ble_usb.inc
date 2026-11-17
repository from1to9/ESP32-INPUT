// SPDX-License-Identifier: Apache-2.0
// Local USB receipts live on the same owner as native commands and HID output.
static void command(const passport_command_t *request, uint64_t now)
{
    if (request->command_id && usb_command.active) usb_command_finish(PR_UNKNOWN, 0);
    passport_result_t outcome = command_impl(request, now);
    if (!request->command_id) {
        if (outcome == PR_PENDING || outcome == PR_APPLIED || outcome == PR_RELEASE_REQUIRED)
            usb_command_finish(PR_UNKNOWN, 0);
        return;
    }
    state.command = (passport_receipt_t){request->command_id, outcome, 0};
    if (outcome != PR_PENDING) return;
    usb_command.request = *request;
    usb_command.active = true; usb_command.uncertain = false;
    usb_command.epoch = engine.state.epoch;
    usb_command.deadline = now + (request->kind == PASSPORT_ADVERTISE ?
        (request->window_ms ? request->window_ms : WINDOW_MS) + 3000u : 5000u);
}

static void usb_command_poll(uint64_t now, hid_engine_step_result_t step)
{
    if (!usb_command.active) return;
    passport_command_kind_t kind = usb_command.request.kind;
    bool input = kind == PASSPORT_MOVE || kind == PASSPORT_CLICK || kind == PASSPORT_KEY || kind == PASSPORT_SCROLL;
    bool release = kind == PASSPORT_LOCK || kind == PASSPORT_CANCEL || kind == PASSPORT_TAKEOVER;
    if ((input || release) && (engine.state.epoch != usb_command.epoch || step == HID_ENGINE_STEP_SEND_FAILED ||
                  step == HID_ENGINE_STEP_RETRY_EXHAUSTED)) {
        usb_command_finish(PR_UNKNOWN, 0); return;
    }
    if (input && !engine.count && !engine.state.release_pending) {
        usb_command_finish(PR_APPLIED, 0); return;
    }
    if (kind == PASSPORT_LOCK || kind == PASSPORT_CANCEL || kind == PASSPORT_TAKEOVER) {
        if (!engine.state.release_pending && !engine.count) usb_command_finish(PR_APPLIED, 0);
    } else if (kind == PASSPORT_STOP_PAIR) {
        if (!state.pairing && !new_connection) usb_command_finish(PR_APPLIED, 0);
    } else if (!transition && kind <= PASSPORT_FORGET) {
        if (usb_command.uncertain) { usb_command_finish(PR_UNKNOWN, 0); return; }
        if (kind == PASSPORT_PAIR) usb_command_finish(state.pairing && state.advertising ? PR_APPLIED : PR_UNAVAILABLE, 0);
        else if (kind == PASSPORT_DISCONNECT && !state.connected) usb_command_finish(PR_APPLIED, 0);
        else if (kind == PASSPORT_FORGET) {
            ble_addr_t peer = {.type = usb_command.request.peer_type};
            memcpy(peer.val, usb_command.request.peer, 6);
            usb_command_finish(!state.storage_known ? PR_STORAGE_UNCERTAIN : !is_saved(&peer) ? PR_APPLIED : PR_UNAVAILABLE, 0);
        } else if (kind == PASSPORT_ADVERTISE) {
            ble_addr_t peer = {.type = usb_command.request.peer_type};
            memcpy(peer.val, usb_command.request.peer, 6);
            if (state.ready && selected_valid && same(&selected, &peer)) usb_command_finish(PR_APPLIED, 0);
            else if (!state.advertising && !state.connected) usb_command_finish(PR_UNAVAILABLE, 0);
        }
    }
    if (usb_command.active && now >= usb_command.deadline) usb_command_finish(PR_UNKNOWN, 0);
}
