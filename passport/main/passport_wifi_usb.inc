// SPDX-License-Identifier: Apache-2.0
// These receipts and all radio/storage decisions belong to the Wi-Fi owner.
static void handle_command(const passport_wifi_command_t *command, uint64_t now)
{
    if (command->command_id) usb_command_finish(PR_UNKNOWN, 0);
    passport_result_t outcome = command_impl(command, now);
    if (!command->command_id) {
        if (outcome == PR_PENDING || outcome == PR_APPLIED) usb_command_finish(PR_UNKNOWN, 0);
        return;
    }
    state.command = (passport_receipt_t){command->command_id, outcome, 0};
    usb_action = command->action; usb_profile = command->id;
    usb_deadline = now + (command->action == PW_SCAN || command->action == PW_SCAN_CANCEL ? SCAN_MS + 1000u : 65000u);
}

static void handle_submission(const queued_command_t *message)
{
    if (!message->local_usb) { (void)submit_credentials(message); return; }
    usb_command_finish(PR_UNKNOWN, 0);
    passport_result_t outcome = submit_credentials(message);
    uint64_t now = now_ms();
    if (outcome == PR_APPLIED) outcome = confirm_submission(now) ? PR_PENDING : PR_UNAVAILABLE;
    state.command = (passport_receipt_t){message->credentials.request_id, outcome, 0};
    usb_action = PW_CONFIRM; usb_profile = 0; usb_deadline = now + 65000u;
}

static void usb_command_poll(uint64_t now)
{
    if (!state.command.id || state.command.result != PR_PENDING) return;
    if (!state.initialized) { usb_command_finish(PR_UNAVAILABLE, 0); return; }
    if (usb_action == PW_SCAN && !state.scanning)
        usb_command_finish(state.scan_known ? PR_APPLIED : PR_UNAVAILABLE, 0);
    else if (usb_action == PW_SCAN_CANCEL && !scan_draining) usb_command_finish(PR_APPLIED, 0);
    else if ((usb_action == PW_USE_AP || usb_action == PW_DISCONNECT) && !switching &&
             state.ap_started && !desired_station) usb_command_finish(state.dhcp_running ? PR_APPLIED : PR_UNAVAILABLE, 0);
    else if (usb_action == PW_SETUP_OPEN && state.setup_open) usb_command_finish(PR_APPLIED, 0);
    else if (usb_action == PW_CONNECT && state.phase == PW_ONLINE && state.active_profile == usb_profile && !switching)
        usb_command_finish(state.storage_known ? PR_APPLIED : PR_STORAGE_UNCERTAIN, 0);
    else if (usb_action == PW_CONFIRM && state.request_state == PW_REQUEST_SAVED && state.phase == PW_ONLINE && !switching)
        usb_command_finish(state.storage_known ? PR_APPLIED : PR_STORAGE_UNCERTAIN, 0);
    if (now >= usb_deadline) usb_command_finish(PR_UNKNOWN, 0);
}
