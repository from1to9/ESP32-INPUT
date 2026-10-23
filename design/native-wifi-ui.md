# Native setup and input controls: UI extension contract

Status: architecture accepted by the coordinator owner, 2026-10-21; concrete appended types are declared in [app_ui.h](../firmware/main/app_ui.h) and the native UI candidate is implemented in [app_ui.c](../firmware/main/app_ui.c). Baseline inspected at `955877104733af6d756ec358e2c328db3b85a35b`; the separate 50-percent brightness stage is published at `550f1a31b45d279e303a8b527dd806c2f2eb8c42`. Coordinator/service integration and complete stable validation are separate delivery gates. The latest user instruction makes on-device scan/select/password/join the normal setup path; the computer stays on its existing LAN. A protected setup hotspot remains an explicitly labeled recovery action, not the default instruction.

This scoped extension follows the [interaction contract](../docs/interaction.md), [focused layout](ui-plan.md) and [connection-management design](connection-management.md). It implements native station setup without claiming that the proposed multi-profile, authenticated LAN API, MCP or OTA services already exist.

## 1. Ownership and delivery scope

- The network manager owns scan generations, immutable result identities, actual security support, join/cancel jobs, storage and station/IP observations. It never sends HID or changes LVGL objects.
- The coordinator owns navigation, selected candidate identity, job admission, epoch/revision/gesture checks and the private credential handoff. UI callbacks enqueue bounded work only.
- The UI owns the transient, local-only credential draft and its keyboard focus. It never changes network configuration optimistically or copies credential keystrokes to the HID draft.
- The existing three fixed primary pages and one dynamic panel remain. The test owner retains `tests/ui/`; the parent owns coordinator wiring and shared requirements/configuration documents.
- First delivery preserves the existing single saved-network capability. Display saved/current/uncertain storage separately. Eight profiles, priority ordering and Save only remain future capabilities unless the network owner implements them. Join once / Remember and join follow the current service request policy only after actual implementation.

## 2. Screens and navigation

The normal path is Menu -> Settings -> Connectivity -> Wi-Fi -> Find networks -> Network details -> Enter password -> Join. No step asks the computer to leave its own Wi-Fi.

| Page | Content and actions |
| --- | --- |
| Wi-Fi overview, existing `APP_SETTINGS_WIFI` | Current SSID, association/IP progress, LAN IP, saved SSID and storage result, actual join error, separate Web/API availability; Find networks, saved-network retry/cancel where supported, confirmed Forget and Back |
| Nearby networks, new `APP_OVERLAY_WIFI_NETWORKS` | Four rows per page; safe SSID label, security, measured RSSI, supported/current/saved state; Previous/Next, Refresh or Cancel scan, Back |
| Network details, new `APP_OVERLAY_WIFI_NETWORK` | Complete safe network label, security/support reason, current/saved state, impact of switching management network; Enter password/Join only when admitted; Back |
| Local credentials, new `APP_OVERLAY_LOCAL_EDITOR` | Selected network label, masked password, local validation, independent character grid, case/symbol pages, Delete/Space and explicit Join; Cancel/Close clears the draft |
| Accepted join | Return to Wi-Fi overview with the actual job identity and phase. Connecting, Getting address, Connected, Saving, Failed and Cancelling reflect manager observations. Back dismisses the page without silently cancelling an accepted job. |

Use the existing 480 x 480 shell, title/critical cue/Close row and a 456-pixel content width. Nearby rows are 456 x 52 with at least 8-pixel gaps and four rows per page. Paging/footer controls are at least 44 pixels. The credential editor uses the existing 3 x 3 geometry with 112 x 76 character/group cells and a 96-pixel utility rail; its character mapping is independent of the HID keyboard. Selected SSID, masked entry and count replace the HID preview. The footer provides Delete, Space and Join. A case/symbol page covers the manager-supported printable ASCII credential set; no character is silently dropped or truncated.

Opening a list or refreshing cannot replace the row under a held finger. A changed scan generation/page uses the dynamic-panel replacement boundary and invalidates the coordinator revision. Delayed actions retain their original scan/record identities and press-time epoch/revision. Expired or replaced candidates are rejected as stale and require a fresh selection.

The overview must explicitly distinguish `LAN connected` from `Web/API ready`. If no station-bound service exists, show `Web/API unavailable` or `Not implemented`; display the LAN IP as network information, without presenting a non-existent service URL. A URL becomes actionable only when the actual service reports READY and supplies its listening address. A Wi-Fi link does not imply Internet access, controller enrollment or remote-input authority.

## 3. Proposed public UI state

The public header groups the scan, selected network, Bluetooth name and local editor into bounded view types. The following table describes their semantics; the header defines the exact member names. Only four scan records are copied per view.

| State | Required semantics |
| --- | --- |
| `wifi_scan_state` | Unknown/unavailable/idle/queued/scanning/cancelling/ready/error; actual reason and cancellation capability |
| `wifi_scan_generation`, `wifi_scan_page` | Stable result generation and coordinator-owned zero-based page; generation changes when results are replaced, not on every view poll |
| `wifi_scan_count`, `wifi_scan_total_known`, `wifi_scan_truncated` | Returned bounded record count; total only if known; never turn unavailable results into an empty list |
| `wifi_scan_rows[4]` | Each row has manager-issued `record_index`, safe display label, security label, RSSI validity/value, saved/current markers, join availability and reason. UI receives only the current page, not a second 32-record cache. |
| Selected candidate | `wifi_selected_generation`, `wifi_selected_record_index`, safe full SSID label, security, password requirement and actual supported credential byte/encoding limits; no saved password |
| Join operation | Job/generation identity, accepted/running/terminal phase, safe error code/message, cancellation availability, runtime association and IP observations, independent durable save result |
| Saved network | Known/present/valid/uncertain metadata and safe stored SSID; separate from current runtime SSID. Reuse existing service storage truth rather than `configured` alone. |
| LAN service | Unavailable/disabled/starting/ready/error plus safe reason; URL only from an actually running service. The UI cannot derive readiness solely from the station IP. |

The initial service may retain the proposed 32-result ceiling; it must advertise/define its real capacity and security support. Native selection can reference raw SSID bytes through a manager-owned identity, so the UI never submits an escaped display label as the network name. A safe label may require up to 129 bytes for escaped 32-byte SSIDs; unsupported glyphs/bytes must not be replaced with a different SSID. Hidden-network/manual entry remains explicitly unavailable unless its service and editor support are included.

## 4. Proposed intents and coordinator boundary

Append new action/overlay values; do not renumber existing values. All ordinary actions remain subject to whole-wake consumption, local takeover, modal ownership and press-time revision checks.

| Proposed action | Meaning |
| --- | --- |
| `APP_WIFI_SCAN` | `x=1` requests an explicit bounded scan, `x=0` requests cancel of the identified scan job. Acceptance does not mean results are ready. |
| `APP_WIFI_SCAN_PAGE` | Requests a zero-based page of the currently identified scan generation. |
| `APP_WIFI_NETWORK_SELECT` | Selects the exact immutable manager record index from the captured scan generation; opens detail only after acknowledgement. |
| `APP_WIFI_CREDENTIAL_OPEN` | `x=0` selects Join once and `x=1` Remember and join; opens a fresh private editor for the selected supported candidate; never retrieves a saved password. |
| Existing `APP_WIFI_FORGET_REQUEST`/`APP_CONFIRM` | Preserve explicit confirmation and uncertain-storage recovery. |
| Existing `APP_WIFI_SETUP` | Recovery hotspot only, if retained. Default Wi-Fi navigation no longer starts it. |

Proposed ordinary intent additions are explicit `uint32_t wifi_scan_generation` and `uint16_t wifi_record_index` fields. The latter is the immutable manager index within that generation, not the visible row number. Do not overload pointer coordinates. The current service draft has no join-cancel or saved-credential retry API, so those controls/actions remain unavailable until an owning API exists.

## 5. Private credential handoff

Keep the existing generic `app_intent_t.text` free of credentials. Do not enlarge every HID queue item to carry a password. Add one explicitly registered nonblocking callback, proposed `app_ui_set_local_edit_submit(app_local_edit_submit_fn)`, installed by the coordinator before the credential panel is made available.

The callback accepts a bounded discriminated `app_local_edit_request_t` submission. Its WIFI_PASSPHRASE variant contains:

- press-time epoch, UI revision and gesture;
- selected scan generation/immutable record index and coordinator-issued editor session ID;
- password length plus a 64-byte passphrase buffer for the initially supported 8..63-byte ASCII WPA2 form;
- explicit Join once / Remember and join policy for the current single-network service.

The coordinator copies an accepted submission into one bounded private slot/queue, validates the still-current editor/candidate/gesture and hands it to the network worker. It never logs, serializes or inserts the secret into ordinary intent/state diagnostics. The callback returns `APP_LOCAL_EDIT_ACCEPTED`, `BUSY`, `STALE`, `INVALID` or `UNAVAILABLE`; acceptance means a private copy was made, not that the manager applied the change. A missing handler or busy slot keeps Join unavailable/rejected with a truthful local reason. UI clears the full temporary request after every callback return, and its entire draft on accepted or stale submission. Other rejected admission may retain it only inside the still-valid local editor for deliberate retry.

Ordinary state contains no password, password echo or revealed text. Default entry is masked; the first implementation need not offer Reveal. Password characters never enter LVGL label strings, the original-text label cache, accessibility text, a retained screenshot or the HID draft. Local count/validation metadata is confined to this panel. Clear the complete draft and any pending private submission on cancel, close/back out of the editor, session replacement, timeout, lock, screen-off, shutdown or successful handoff. Saved credentials are never loaded into the editor. The network manager separately clears its temporary secret copies when a job completes/fails/cancels and preserves only authorized durable storage.

The agreed editor deadline is five minutes, defined by `APP_UI_LOCAL_EDIT_TIMEOUT_MS` and enforced by the coordinator. Its session/deadline is coordinator-owned; a stale render or queued submit cannot revive an expired draft. Secure wipe uses a non-optimized byte clearing routine. Clearing a short mask label is not evidence that the underlying password has been erased.

## 6. Integration ownership

1. Network owner: exact scan snapshot/page API, generation/record IDs, supported auth modes, passphrase encoding/range, one-saved-network join transaction and cancellation/rollback boundaries.
2. Coordinator owner: the appended header contract, private one-slot submission shape, editor session lifetime and navigation/AI handling. AI single must not confirm HID characters while the private editor is open; AI double/Close cancels that editor and wipes its draft; physical hold remains lock/stop. Admit Common keys from the Keyboard chooser, and Back to networks from the network detail through `APP_MENU`. Every new editor receives a fresh session identity; a consumed or expired identity cannot reopen a draft.
3. LAN owner: actual service state/address source. Until that service exists, the overview deliberately reports unavailable; the computer's existing Wi-Fi remains unchanged.
4. UI/test owners: page geometry, row-generation replacement, secret cleanup observation and native FULL-render/resource budgets for this extension. Do not assume the previous 142-object peak covers new screens before measuring them.

The parent owns the brightness-default change to 50 percent; UI documentation/prototype will reflect 50 while intentionally labeled test fixtures may still use other values.

## 7. Required evidence

Preserve all compact UI interaction/failure scenarios and zero idle/hidden-state allocations/flushes. Add real pointer tests and both-theme renders for scan queued/running/cancel/error/empty/truncated/stale results, page boundaries, duplicate/long/non-displayable SSIDs, unsupported security, stale row selection, masked editor character coverage and limits, queue rejection, accepted secret transfer, held-key disable/page replacement, wake/lock/timeout cleanup, join phases/errors/cancel, runtime-versus-saved mismatch, uncertain storage, no LAN IP, and service-unavailable versus real service-ready address.

Observe live allocator bytes to prove synthetic test passwords and tails are cleared. Confirm credential actions emit no `APP_TEXT`, `APP_GRID`, `APP_SEND`, shortcut or HID output; the ordinary HID draft survives unchanged. Meaningful manager/coordinator tests must establish generation rejection and private-slot cleanup. Target build, actual LAN join/IP acquisition, host reachability and BLE/USB coexistence are separate device gates, performed by their authorized owners.

## 8. Shared local editor and Bluetooth device name

The additional user request to edit the Bluetooth device name uses the same `APP_OVERLAY_LOCAL_EDITOR` layout/character-selection component with a distinct `BLUETOOTH_NAME` purpose. Its purpose and session ID are coordinator-owned; a purpose change always clears the old buffer before loading the next editor.

Keep separate bounded Wi-Fi passphrase and Bluetooth-name buffers. The passphrase is masked and never prefills from saved credentials. A public device-name draft may prefill from the actual configured local device name. Neither editor reads, modifies or submits the HID preview. Bluetooth host names/identities are separate and never renamed by this control.

Proposed entry action: `APP_BLUETOOTH_NAME_EDIT`, admitted only from Bluetooth settings, opens the typed editor. It uses the shared `app_ui_set_local_edit_submit(app_local_edit_submit_fn)` registration described above. The submission is a discriminated union with explicit WIFI_PASSPHRASE and BLUETOOTH_NAME variants, common press-time epoch/revision/gesture/editor-session context, and separate payload types. Wi-Fi carries generation/immutable record index, remember policy and the 64-byte private passphrase. Name carries only the bounded local device-name bytes. The coordinator routes each purpose through its owning manager and clears the consumed payload; neither variant uses generic `APP_TEXT` or the ordinary HID queue. No other purpose can submit a Wi-Fi credential.

The Bluetooth owner has proposed 1..18 UTF-8 bytes, rejecting malformed UTF-8, NUL/control characters and leading/trailing ASCII spaces. Its adapter provides `ble_hid_name_valid`, asynchronous `ble_hid_name_request` and `ble_hid_name_snapshot`, with one pending request and no forced disconnect. The worker verifies GAP/advertising application, reports verified restoration on failure, and marks missing-callback outcomes uncertain. UI consumes that bound/status rather than choosing a limit from screen width. The initial device grid types printable ASCII; an existing valid UTF-8 name is retained byte-for-byte and deleted by complete code point, never truncated to fit. Unsupported display glyphs use a safe escaped preview without changing the submitted bytes. Show configured, pending/effective and saved/error states truthfully. A successful save alone cannot claim the current advertisement or an already connected host has refreshed its cached name. Preserve the current host, bond store and all unrelated settings. Pairing instructions use the actual effective local device name instead of a hardcoded `ESP32-INPUT`; input-target context continues to show the host identity.

The private editor's on-screen Apply/Join action is the only submit path. AI single does not operate the hidden HID character grid. AI double, Close, Back, lock, screen-off, timeout and editor replacement cancel and clear the draft without applying it. The entire first wake/takeover gesture remains consumed.

## 9. Motion availability and actionable guidance

Current source gates Motion on all of: a calibrated supported IMU, selected input provider availability, connected/ready HID, no release or transport error, no transport switch, and no calibration in progress. In `main.c`, `motion_available` is `capabilities.imu && interaction.calibrated`; `calibration_available` is the IMU capability. In `app_ui.c`, the Motion toggle additionally requires the ordinary HID-ready predicate. The user has selected USB and still cannot enable Motion. The current live cause is not established: an earlier Bluetooth readiness log cannot diagnose this USB-selected state. The coordinator must publish the actual selected-provider, calibration and sensor facts; UI must not guess a Bluetooth failure or force Motion on.

The existing disabled toggle merely says `Motion / off`; the compact connection alert does not explain calibration/sensor prerequisites. Proposed presentation adds a bounded two-line reason in the pad's noninteractive caption and a `Motion help` control in the unused Mouse selector slot at x140/y8/w132/h48. Help opens Status through existing `APP_MENU(APP_OVERLAY_TARGET)`; it never emits pointer motion or changes a setting. Status shows input and sensor/calibration conditions separately, with Connections and Input settings actions through the existing settings navigation.

| Actual blocker | Visible guidance | Next action |
| --- | --- | --- |
| Selected Bluetooth has no ready input host | `Connect Bluetooth or select USB input.` | Connectivity; pairing instructions name this device's effective Bluetooth name |
| Selected USB absent/not ready | `Connect a USB data host or select Bluetooth.` | Connectivity; do not infer readiness from charging/cable presence |
| Connected but HID not ready | `Host connected; input is not ready.` | Connection detail; no claim that pairing alone enables input |
| Release or transport switch in progress | `Waiting for held input to release.` / `Switching input connection.` | Status; retain truthful recovery/cancellation state |
| Actual transport error | `Input connection error. Open Status.` | Status shows all simultaneous error/release/disconnection conditions |
| Calibration running | `Keep the device still to calibrate.` | Input settings; no redundant calibration request while active |
| IMU present, calibration not ready | `Calibrate motion in Input settings.` | Existing `APP_CALIBRATE` through Input settings |
| IMU unavailable | `Motion sensor unavailable.` | Show actual capability/error evidence; never suggest Bluetooth pairing repairs a sensor |
| All prerequisites met | `Touch pauses motion.` | Motion toggle is enabled; AI single retains its contextual toggle |

The appended `app_motion_reason_t` is coordinator-derived so UI, API and tests agree with the gate, retaining independent sensor/calibration and link fields for simultaneous blockers. Its zero value is UNKNOWN, not READY. Enabling requires READY plus the existing actual prerequisites; an already active Motion control remains available to turn off. Existing public fields do not establish fresh IMU read health: do not label a transient read failure unless the board/coordinator reports it. This guidance never forces Motion on without a ready host or calibration.

## 10. Alignment with the network owner's current header draft

The header draft observed during this proposal provides `app_wifi_scan_start/cancel`, `app_wifi_get_scan_snapshot`, one-row `app_wifi_get_scan_record(generation, index, ...)`, and private `app_wifi_join(request)`. A **generation plus immutable manager record index** is acceptable as the stable result identity; it is not the mutable visible row number. The proposed ordinary intent fields are `wifi_scan_generation` and `wifi_record_index`, captured from the displayed row's immutable binding.

The coordinator copies at most four records into the current UI page. It resolves the selected record again before the private join submission and copies its original SSID bytes into the service request; UI labels are never parsed back into SSIDs. The current draft supports `remember=false` and `remember=true` against one durable record, so Join once / Remember and join can be offered only when that implementation is admitted. It does not expose a join-cancel or saved-credential retry API yet: those actions stay absent/unavailable until an owning API exists. Scan cancel remains real and independent.

The draft distinguishes `APP_WIFI_OBTAINING_IP`, `lan_ready` and `backend_available`; the latter remains false for this foundation. UI must show address acquisition separately and report `Web/API not implemented`, with no working backend URL claim. The accepted local submission shape and five-minute lifetime are declared in the public header. Bluetooth name validation/status uses the concrete 1..18-byte UTF-8 adapter contract above.

## 11. Common keys without shrinking the character grid

The Keyboard header exposes a persistent `Keys` button at x 224/y 8, 48 x 48 pixels in every submode. The submode chooser uses x 140/y 8, 76 x 48 pixels with the same 16 px caption as Keys and Menu. Its existing `Common keys` entry remains available. Both submit `APP_MENU` / `APP_OVERLAY_COMMON_KEYS`; navigation waits for coordinator acknowledgement. Primary character entry retains its nine 112 x 76 cells, utility rail, preview and explicit Send.

The panel identifies the selected host and acknowledged Ctrl/Command setting. Fifteen 144 x 52 buttons form a complete, symmetric five-row, three-column grid, starting at x 12/168/324 and y 112 with a 60 px row step. The arrangement is Copy / Paste / Undo; Backspace / Delete fwd / Enter; Esc / Tab / Select all; Home / Up / End; Left / Down / Right. Back to keyboard remains at y 424. All targets remain at least 44 x 44 pixels. No full-height scrolling or shrinking of the character grid is needed.

Copy/Paste/Undo/Select all emit `APP_SHORTCUT` identities from every Keyboard submode. `APP_SHORTCUT_SELECT_ALL` is appended after the six original enum values, preserving their identities and the legacy six-button Shortcuts submode. The coordinator uses the acknowledged host setting to send Ctrl+C/V/Z/A for Windows or Command+C/V/Z/A for Apple, with a reserved release and no sticky modifier. Select all uses HID usage `0x04` with the same modifier and release policy. Paste never reads the host clipboard or automatically sends the draft. The other eleven buttons emit `APP_KEY` with the following HID usage and zero modifiers; they never imply repeat or append Enter.

| Label | HID usage | Meaning |
| --- | --- | --- |
| Esc | `0x29` | Host Escape |
| Tab | `0x2b` | Host Tab |
| Enter | `0x28` | Explicit host Enter, independent of Send |
| Backspace | `0x2a` | Delete backward at the host |
| Delete fwd | `0x4c` | Delete forward at the host |
| Home / End | `0x4a` / `0x4d` | Host Home / End |
| Left / Up / Down / Right | `0x50` / `0x52` / `0x51` / `0x4f` | Host arrow keys |

`APP_DELETE` keeps its existing meaning of removing the last local preview character. The preview action remains labeled `Delete draft`. The existing Shortcuts submode remains compatible with the same coordinator-owned Ctrl/Command setting. The common-key panel is admitted only in Keyboard mode and shares ordinary host-readiness, modal, stale-gesture, wake, lock and release gates. Local draft and private editors are unchanged by a live key press. No common key is sent while a private editor is open.

Required checks cover the one-tap entry in all four Keyboard submodes, readable header captions, all fifteen actions, both host modifier settings, preview preservation, explicit Enter, disabled/disconnected/sending/release-pending state, held-button revisions, stale actions after closing, and absence of a key press during wake, lock or private editing. Actual 480 x 480 renders cover both themes, ready/disconnected state and capture/lock cues.

## 12. Tap to click and double-tap drag

Add an Input settings toggle labeled `Tap to click`, default on under the shared persistent setting contract. `APP_TAP_TO_CLICK` carries explicit `x=0/1`; the view separately reports availability and the acknowledged value. UI never updates the setting optimistically or recognizes touchpad gestures itself. The existing `APP_TOUCH_BEGIN/MOVE/END/CANCEL` stream remains the Input Engine's source of truth.

With the setting on, the Input Engine recognizes a single tap, a double tap and a double-tap-and-hold drag. It owns movement slop, timing bounds, click admission, held drag/release and cancellation semantics; its recognizer contract supplies the actual thresholds and failure handling. Real stationary `LV_EVENT_PRESSING` on the active touchpad emits a zero-delta `APP_TOUCH_MOVE` with a minimum 50 ms interval, measured with the wrap-safe LVGL tick difference. The coordinator stamps monotonic `event_ms` at callback entry, before queue delay. No timer invents contact after release, lock, screen-off or mode/panel change. Drag ends on finger release or cancellation. A mode/overlay/transport change, lock, screen-off, takeover, setting change or missing touch boundary must not leave a held button or replay a delayed click.

Motion help reuses the existing primary choice control by mode, and motion/gesture guidance shares the original pad caption. This preserves the 102-object core budget and the existing 160-object one-panel ceiling without removing the new functions or shrinking input targets.

With the setting off, touchpad contact produces movement only. Explicit Left, Right, Drag/Release and scroll controls retain their current behavior. UI help describes `Tap to click off / touchpad moves only` and points to Input settings. Gesture drag and the existing explicit drag control must use one coordinator-owned drag state with deliberate ownership; UI must not maintain a second hidden hold state.

Required evidence belongs primarily to the Input Engine and coordinator: single/double tap, second-contact hold and movement, interrupted sequences, off-mode movement-only behavior, explicit-button preservation, late/invalid boundaries, queue pressure, failed release and persistence migration. UI evidence covers truthful toggle value, pending/error storage, help text, no duplicate gesture interpretation and unchanged spacious touchpad geometry.
