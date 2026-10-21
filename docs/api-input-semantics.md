# API input semantics and configuration plan

Status: planning only, reviewed 2026-10-21 against the working source tree based on `0705a5f7d4067ced8439c54c157a202df6b4c229`. This document adds no server, command handler, configuration storage or hardware capability. Complete the API/function plan and reconcile the integration decisions below before implementing new API behavior.

The [complete API matrix](api-function-matrix.md) owns registry names, field scopes and operation coverage; the [configuration contract](configuration.md) owns the common transaction and metadata model. The [remote-control design](remote-control.md) supplies the baseline authentication, browser/Mac, input-channel and resource contracts. Its older blanket physical-approval, per-field setter and navigation proposals are superseded where the complete matrix and latest owner decisions specify scoped remote authority. [Wi-Fi/OTA](wifi-management-ota.md) and [audio/recording](api-audio-recording.md) own their manager lifecycles. This companion defines input meaning, local/remote parity, every current application action and failure cases. Product intent remains in [requirements](requirements.md) and the shared [interaction contract](interaction.md); qualification evidence belongs in the [remote test plan](remote-control-test-plan.md). All API operations remain proposed.

## 1. Source audit and implementation boundary

| Owning source | Audited responsibility |
| --- | --- |
| [Application UI contract](../firmware/main/app_ui.h) | All 48 `app_action_t` values, copied intents, gesture/context identity and read-only view |
| [Coordinator](../firmware/main/main.c) | Mode/lock/takeover, modal focus, draft sends, settings, transport switching, pairing confirmations and truthful unavailable audio |
| [Input core](../components/input_core/include/input_core.h) and [implementation](../components/input_core/input_core.c) | Epoch/owner/mode gates, neutral barrier, relative motion and printable US ANSI conversion |
| [HID engine contract](../components/hid_engine/include/hid_engine.h) | Bounded admission, report scheduling, cancellation, text counters, congestion and release recovery |
| [Interaction contract](../components/interaction/include/interaction.h) and [implementation](../components/interaction/interaction.c) | Grid, preview, button classification, calibration, filters, fine touch and send settlement |
| [Device settings contract](../components/device_settings/include/device_settings.h) | Validated persisted preferences, record migration, display idle and consumed wake gestures |
| [Settings store](../firmware/main/settings_store.h) | Asynchronous coalesced persistence and requested/saved revisions |
| [Board contract](../firmware/main/board_port.h) and [adapter](../firmware/main/board_mosaico.c) | Actual hardware availability, applied brightness, physical events and haptic dispatch |
| [Haptics policy](../components/haptics/haptics.c) | Bounded feedback, global suppression, cooldown and motor-off recovery |

Current firmware has local coordination, BLE/USB HID adapters and Wi-Fi provisioning/station management. It has no complete authenticated runtime command service, remote lease manager, replay cache or API settings schema. `INPUT_REMOTE` is an ownership primitive, not authentication or authorization. Current UI intents always enqueue direct HID with `INPUT_LOCAL`; accepting a network payload into that path would forge its source.

Current interaction draft/grid validators also require `INPUT_LOCAL`. Remote preview editing therefore needs an explicit shared-model authorization boundary; calling the current functions after changing the owner to remote would reject edits. Preserve trusted source/lease checks while extending that owning model, rather than temporarily claiming local ownership or maintaining a second browser-only canonical draft.

Three earlier remote proposals exceed today's implementation:

| Earlier proposal | Current source | Initial compatibility plan |
| --- | --- | --- |
| 128-character draft/job | Preview capacity 64; engine text batch 32 | Advertise draft/job maximum 64 and batch maximum 32 until a deliberate shared-model extension is implemented and tested. Reject excess; never truncate |
| Chord of up to six nonmodifier keys | Report has six slots, but enqueue accepts one usage plus an 8-bit modifier | Advertise one nonmodifier usage. Multi-key chords are unsupported until the production engine gains atomic validation and mandatory release |
| Sent prefix consists of completed down/up pairs | Local coordinator consumes preview using `text_accepted`, which counts accepted key-downs; engine separately exposes `text_completed` | Preserve both counters and an uncertain character in the planned API. Do not relabel the current accepted count as completed pairs or host delivery |

The initial API must discover these real limits. Increasing a JSON limit alone, presenting a placeholder as a working feature, or wrapping a void local handler in `success:true` does not implement the missing semantics.

## 2. Shared command boundary and parity

Use one portable, versioned, validated domain schema for the device UI, Wi-Fi API and Mac MCP tools. Adapters convert their own inputs into named commands with immutable copied arguments and trusted source metadata. The coordinator alone mutates state. UI/API/MCP share validators, transitions, capability gates and result definitions; they do not maintain independent defaults or parallel HID implementations.

Do not expose the numeric application enum, `app_intent_t`, raw HID reports, GPIO events or mutable model structures as a wire API. Network callers cannot choose `INPUT_LOCAL`, supply physical gesture IDs, fabricate physical consent, write `locked=false`, replace epochs or acknowledge a release barrier. A remote virtual gesture has a separately issued channel-bound identity and remains remote. Derived principal, channel, scopes, consent authority and lease come from authenticated records; physical source evidence comes from the board path.

Parity means the same domain effect and safety transition, including cancellation and persistence status. It does not require browser controls to navigate the device's settings pages or synthesize touches. Browser-only navigation has no device effect. An explicit `ui.navigate` operation uses the shared UI transition, cancellation behavior and context revision. Context-sensitive UI commands cannot activate hidden controls or approve an unrelated local modal. A scoped manager operation instead validates the named resource/job, revision, authority and conflicts; it needs no fabricated page navigation, and may confirm its exact operation when the registry permits remote confirmation.

The revised product UI has core-only Mouse/Keyboard/Audio workspaces and an on-demand shared menu for mode selection, status and settings. The discoverable screen entry and AI double-click open that same menu. The interaction/UI owners define its route IDs, close behavior and layout; this document does not freeze legacy overlays, persistent tabs or detailed headers into an API requirement. Current source navigation below is an audit of the earlier implementation, not the approved future screen layout.

Current local handling silently discards many invalid/stale intents and sometimes returns only a message. The future shared boundary must return typed rejection reasons for API use while retaining the same no-effect behavior. It must not interpret a missing local result as acceptance.

### Modes and producers

| State | Permitted behavior |
| --- | --- |
| Mouse | Relative movement, scroll, atomic clicks and explicit left drag when all HID gates pass. Calibrated local gyro is a separate producer |
| Keyboard | Key taps/shortcuts, explicit direct text, or shared preview editing/selection followed by explicit send. IMU selection changes focus only |
| Audio | Current UI can display its unavailable page and voice/speaker choice; no audio or HID output becomes available from selecting it. Planned operational audio requests require implemented and qualified capabilities |
| Locked | No nonzero HID, motion, remote unmute or ordinary mutation. Neutral cleanup and authenticated safety actions remain possible; public product reads still require their viewing scope |
| Disconnected, recovering or release pending | No new nonzero HID. Read status, settle jobs and perform permitted device-local configuration through its schema; never infer a healthy host |
| Modal, calibration or transport transition | The owning operation has exclusive relevant state. Reject conflicting commands as busy; a separately authorized manager operation does not require an HID lease or physical menu navigation |

Changing mode cancels sending/drag, clears motion/filter carry and advances epoch before new output. Selecting the already active mode with no overlay is a no-op and must not cancel a send or save settings. Opening a local menu already cancels input. Closing Wi-Fi setup also stops its AP and clears transient setup material. Selecting a transport is shared across modes; it does not select an audio profile or duplicate input to both hosts.

Planned `input.motion.set` separates controller authority from the selected producer. A scoped remote input owner may explicitly select the qualified device-IMU producer while its lease remains valid; no physical handoff or repeated physical confirmation is required. Exactly one selected producer feeds the shared HID gate, and expiry/takeover stops it. Current local-only gyro/touch checks need a shared coordinator/model extension for that behavior. Never fake `INPUT_LOCAL`, enable concurrent remote deltas and IMU output, or call an adapter directly to make a rejected toggle appear successful.

### Planned remote producer and virtual-gesture extension (PC07)

These transitions are unimplemented and must pass the enabling cases in section 10 before any capability advertises them. The shared semantic dispatcher owns all admission and arbiter changes; no API/MCP shortcut calls the engine or a hardware adapter directly.

| Operation / event | Required transition |
| --- | --- |
| Explicit leased IMU selection | Validate I(`input.control`), boot/channel/epoch/target, calibration, mode and producer availability; cancel/settle the old input producer, advance epoch, clear carry and satisfy the neutral barrier. Keep the authenticated remote controller as authority and select only `device_imu` as producer |
| Remote-controlled IMU running | Recheck lease, state freshness, current generation/mode and output gates for each new sample/report. Use real monotonic sample timing and the shared calibrated filter/gain; delayed samples never become a burst. Wrist movement alone does not take ownership |
| Direct/scripted movement during selected IMU | Reject `producer_conflict`; do not combine, interleave or silently replace producer streams. An explicit producer-selection transition must cancel/neutralize the old producer before the new one can emit |
| `input.motion.set(false)`, input stop or lease loss | Stop sample-to-HID admission, clear motion/filter/carry and release output. No automatic local handoff, reconnect restart or restoration of previous motion intent |
| Eligible fresh physical touch/AI down | Revoke the remote lease and its producer through normal local takeover, invalidate epoch, cancel/neutralize and consume the takeover gesture. Wake-consumed physical events remain wake-only as specified below |
| Virtual device `touch.begin/move/end/cancel` | Use a separate remote-source gesture record bound to authenticated channel/session, boot, input epoch, target/lease, current UI/selection revision and declared surface. Resolve actions through the same semantic dispatcher as local controls; never inject a raw `app_intent_t` or physical callback |
| Virtual gesture deadline or missing continuation | Enforce a finite declared job deadline and qualified idle deadman no greater than the configured remote hold ceiling (initial candidate 1,500 ms), both bounded by lease expiry. Begin cannot reserve indefinite input. End/cancel/expiry closes only the matching gesture and schedules required neutral cleanup; delayed events cannot end or revive a replacement |

Virtual gestures cannot claim physical presence, approve protected enrollment, unlock, wake through the physical input policy, refresh physical activity or trigger local takeover. If the device screen/context is unavailable, reject its context-dependent activation; remote clients can still use separately authorized semantic operations. Gesture refresh never extends the original job deadline or lease. The initial discrete-operation horizon remains 2,000 ms; longer gesture streams require a separately qualified bounded contract. Device-IMU streaming uses the explicit producer lease/liveness contract above, not a hidden unbounded queue of discrete moves. All owner/producer fields and stop reasons must be visible in state.

## 3. Authority, local priority and the all-functions boundary

The Wi-Fi surface must cover every implemented function, settings field and observable status. Coverage includes explicit unsupported results and actual authority requirements. Enrolled scoped remote authority is sufficient where the matrix permits it; do not invent a physical step for every administrative action. Physical unlock and initial trust enrollment remain protected.

1. Neutral recovery, lock and stop outrank ordinary work. Their reserved bounded lane cannot emit nonzero input, approve an unrelated proposal or unlock. They require the matrix's authenticated safety scope, but no input owner lease. Input cancellation does not cancel an independently authorized update/background-recording job; each manager enforces its own cancellation and lock policy.
2. A trusted physical AI down or first touch that passes the local wake/capture policy during remote ownership cancels remote jobs, invalidates their epoch, clears drag/motion and starts neutral cleanup. Consume that takeover action; a later deliberate local gesture may act. Wrist motion alone never takes ownership.
3. A continued takeover hold may lock. Unlock requires a new complete trusted physical AI hold. The same held press cannot lock and subsequently unlock, and lost button edges require release followed by a fresh hold.
4. Remote acquisition requires explicit local allowance, unlocked state, a fresh acknowledged snapshot and no active local drag/send/motion/listening or modal confirmation. A competing owner gets `owner_busy`; active local work gets `local_busy`. No queued acquisition or automatic reacquire is allowed.
5. Observers and remote polling never take ownership, renew physical idle, wake the display by pretending to be a touch, or grant consent. Management and configuration use their own enrolled scopes and management generation, without borrowing an HID lease or stealing active local input.

| Requested function | Explicit remote boundary |
| --- | --- |
| Global unlock | A typed request is rejected `local_presence_required`; show the required physical hold. No remote flag, button remapping or administrator scope bypass |
| Initial trust/scope enrollment | Establish the trusted principal/scope ceiling through the protected owner enrollment flow; no remote self-enrollment or self-expansion. Enrolled `admin.clients`/`admin.policy` authority may perform allowed subsequent administration remotely |
| Microphone unmute/capture/recording | Explicit consent for the exact bounded job/session from a fresh local approval or separately enrolled `capture.consent` authority. Ordinary input/admin authority is insufficient. Enrolled standing grants may authorize starts within their source/destination/duration/byte/expiry ceiling, without a repeated physical step; saving permission never restores active capture |
| Pairing, bond deletion, target/network changes | `connections.manage` or `wifi.configure`, exact object/job revision and explicit destructive intent where applicable. Apply any operation-specific new-trust/host/presence challenge the manager actually requires; no blanket on-device physical approval. Host Bluetooth pairing may still require action on the host |
| Firmware and remote policy administration | `admin.policy` may enable/narrow/revoke a bounded update policy. `update.check`, `update.stage` and `update.activate` operate within the approved principal/source/channel/version/size/time/maintenance/restart limits or a fresh explicit approval. A matching enrolled policy needs no repeated physical confirmation; signature/layout/release/lock checks remain mandatory |
| Physical power controls and BOOT/POWER GPIO | No remote physical-event injection. Future graceful maintenance/reboot, if designed, is a separate scoped operation with a real terminal/unknown result |
| Stop, neutral release, lock true and mute true | Authorized controller safety action without a lease. Do not require an unavailable hardware capability merely to cancel an active producer; report actual neutral/mute outcome separately |

`operation.confirm/cancel` names an exact pending operation, digest, revision and original authority. A client may explicitly approve that operation remotely when its enrolled scope/policy permits it; this is authorized approval, not self-enrollment or fabricated physical presence. A challenge explicitly marked physical-only must be satisfied through the protected physical path. Never offer a generic confirm-hidden-dialog command.

For the subset requiring a fresh physical proposal, retain the existing 60-second proposal default and bind approval to its principal, action, resource/generation and expiry. Takeover still cancels old input work and consumes the first eligible local action. A new input continuation needs a fresh lease; a management continuation follows its manager generation and separately authorized approval rather than acquiring an unnecessary HID lease. Session-scoped capture consent and bounded update policies follow their owning contracts; do not convert this physical-proposal default into an extra approval requirement for them.

Display wake is independent of ownership and unlock. The first asleep touch/AI gesture wakes and is fully consumed, including delayed click/double/hold callbacks. Overlapping captured gestures are consumed. The AI wake double-click guard is currently 280 ms; a fresh touch after captured contacts release can pass during that AI-only guard. Failed brightness application keeps local controls blocked. Remote commands do not inherit a fabricated awake/local source. Display sleep alone does not disconnect HID, clear the draft, change lock or stop a previously enabled mouse producer; hidden keyboard focus pauses.

Wake consumption precedes takeover and forbids a consumed gesture from taking ownership. Preserve this ordering for the planned remote extensions: the first whole asleep gesture only wakes, including its delayed click/double/hold, and the next eligible fresh physical gesture takes over. Do not restate this as unconditional first-touch revocation or change it in an API adapter. Virtual device gestures never enter this physical wake path.

### Capture indication and file-only continuation (PC06)

For the current board's planned capture, a readable on-screen capture/recording indicator and local Stop are required for the entire capture interval. Starting from a blank display first requests a readable surface through the display manager and verifies actual application/indication before opening capture; it does not synthesize a physical wake gesture. Capture prevents display blanking; only a qualified readable dim level is allowed. Keep the saved idle preference, expose its temporarily blocked effective blanking state and restore ordinary idle behavior after capture stops. Missing/stalled indication rejects or stops capture; an unacknowledged microphone stop leaves the privacy warning visible. No unqualified alternative LED/indicator is assumed.

`continue_local` is an explicit file-only start policy requiring a bounded background-recording grant, source/destination/format authorization, finite duration/byte/quota limits and successful transfer to a device-owned job. This job is independent of the HID lease and target: losing/changing the input lease cannot silently transfer it to another client or fabricate a stop. Live capture egress stops when its authenticated owner/session is lost even if the approved local file job continues. Lock, local privacy Stop/mute, grant expiry/revocation, indicator failure, resource fault and the original bounds still terminate capture. Default recording behavior remains stop on owner loss; neither background permission nor a saved preference resumes recording after reboot. See the audio/recording contract for grant, media and finalization evidence.

## 4. Epochs, revisions, leases and ordered requests

These are separate identities; no one counter replaces another.

| Identity | Meaning and invalidation |
| --- | --- |
| `boot_id` | Fresh device generation; reboot makes old requests/results/approvals ineligible for execution |
| Channel `session_id` | Fresh authenticated WSS channel after snapshot handshake; reconnect does not resume a lease |
| Principal/scopes | Trusted enrolled identity and grants from the matrix: for example `input.control`, `config.write`, `input.configure`, `connections.manage`, `wifi.configure`, `admin.policy`, `capture.consent` and `update.check/stage/activate`. An input lease adds no administrative or privacy scope |
| `lease_id` | Single remote owner on one live channel, bound to permitted target; at most 3,000 ms from device receive time |
| `epoch` | Input generation changed by mode, lock, owner, link readiness/loss, target/transport replacement, cancellation or relevant configuration. Recheck at admission and before each nonzero report |
| Target/transport generation | Exact BLE peer/link generation or USB connection generation plus selected transport. A different connected host is never a continuation of an admitted job |
| `ui_revision` | Local visible context captured at press; stale clicks cannot affect a new page, group or confirmation |
| Draft/selection revision | Planned explicit semantic revisions for network edits and confirmations; prevent an old browser action from editing/sending a new draft or confirming a changed cell |
| Settings revision | Expected desired configuration revision for compare-and-set, separate from applied and saved revisions |
| Management generation | Device-scoped lifecycle for manager/configuration jobs, independent of a selected HID host, mode and input lease; input-affecting apply still acquires its coordinator barrier |
| Send/job token | One bounded execution instance. An old final callback cannot settle, append to or reopen a replacement job |
| `state_seq` | Snapshot freshness only; never permission, epoch or proof of target delivery |

Input operations retain `v`, `boot_id`, `session_id`, `request_id`, `seq`, `epoch`, `target_id`, `lease_id`, `deadline_ms`, `op`, `args`. Encode 64-bit epochs/revisions/device times as unsigned decimal strings; `seq` is an exact integer 1..4,294,967,295. IDs and channel request-prefix construction follow the remote-control contract. The adapter issues target IDs; callers cannot substitute a Bluetooth address. Management operations instead bind device/boot/management generation, resource revision, session authority and expiry; they do not require a selected HID target or null-target input lease. Both authenticated `POST /api/v1/commands` and WSS use the single registry/admission path defined by the matrix; neither is a parallel direct HID endpoint.

Renew explicitly about every 1,000 ms, at most 3,000 ms from receipt, with a state sequence received within 1,500 ms. Ping is not renewal. Stale state, blur/hide/pointer-capture loss, channel loss, session revocation or remote disable stops input production and lease renewal. On detected close cancel input immediately; silent loss is bounded by lease expiry plus the proposed, unqualified 50 ms scheduling budget. Reconnect requires a new channel, snapshot and explicit acquisition. No offline gesture queue or restarted job is allowed. Independently authorized update/recording jobs follow their manager cancellation policy; an explicitly approved file-only background recording may finish within its original limits after network/lease loss, while live transmission stops. Lock, privacy Stop, revocation and bounds still terminate capture, and boot never resumes it.

### Sequence and duplicate decision

- Only the next sequence may introduce work. An authenticated well-formed request at it consumes the sequence even when rejected. A gap returns `sequence_gap` and the expected value without consumption. Malformed framing closes the channel and revokes control.
- Repeating the same sequence, request ID and canonical digest returns the original cached record without execution, save, deadline extension or lease renewal. Digest all gating fields, operation and arguments. Changed content returns `request_conflict` and closes the channel.
- Retain at most 64 request records per channel, pin executing records and preserve the sequence high-water mark after eviction. An evicted older request is `duplicate_too_old` with `result_unknown`, never new work. Validate the issued prefix and encoded sequence; an old ID cannot be reused at a new sequence.
- Desired-state setters take explicit values and expected revisions. A new request with the already desired value returns unchanged and schedules no persistence. Relative input, text, selection confirm, calibration and proposal execution are effects, even when duplicate admission is suppressed.
- There is no durable input journal and no exactly-once target-delivery claim. After reboot, eviction or a lost result, unknown means possibly affected. Reconcile before a person deliberately issues new input; do not convert timeout into automatic retry.

All deadlines use device monotonic time. Relative move/scroll horizon is 250 ms; ordinary discrete jobs are bounded by 2,000 ms and lease expiry. Reject elapsed or excessive deadlines; queueing does not extend them. Pair/network/calibration/update use separate bounded manager jobs with their own capability-advertised deadlines and generation checks. Required neutral reports survive ordinary deadline/scope/lease loss, but may only clear output. Saturating arithmetic and fail-closed clock regression prevent wraparound from extending work.

## 5. Exact arguments and coordinate systems

| Input | Current meaning and external validation |
| --- | --- |
| Relative mouse `dx`, `dy` | Signed integer HID displacement, -127..127 each; X positive right, Y positive down. Neither host-screen pixels nor display coordinates. Reject floats, overflow and nonfinite values; do not split/replay a large packet implicitly |
| Scroll `wheel` | Signed integer -127..127 on the descriptor's vertical wheel axis. Positive wheel is the HID positive direction; target natural-scroll policy determines its visual result. No horizontal wheel advertised |
| Click `buttons` | Current three-button descriptor bitmask 1..7: left=1, right=2, middle=4, combinations explicit. Atomic down plus zero release. No extra buttons unless a different qualified descriptor advertises them |
| Explicit drag | Current engine supports left hold=1 and release=0 only; movement/scroll preserve the accepted held state. Reject clicks while drag is queued/active. Registry `mouse.drag.begin/end/refresh` provides explicit state/lifetime rather than a retryable toggle |
| Remote hold lifetime | Running until matching release; explicit refresh at most 1,500 ms apart, never beyond lease or original deadline. Initial continuous hold is also capped by the 2,000 ms discrete horizon. These remote timers are planned, absent from the current local drag model |
| Key tap | Modifier integer 0..255; one usage 4..0x65, or usage 0 with a nonzero modifier. Usages 1..3 are HID errors. Usage=0/modifier=0 is not a key action. Automatic zero release, planned hold at most 100 ms. No arbitrary key-down/up, rollover-report injection or unsupported six-key chord |
| Text | Explicit printable US ANSI bytes 0x20..0x7e only. Reject NUL, controls, CR/LF, Unicode, malformed encoding and excessive length. Enter is a separate explicit key/shortcut |
| Local logical touch | Current UI is 480 by 480, origin top-left: x/y 0..479 for an in-display point. Board profiles must publish actual logical dimensions/orientation; validate against that profile. Coordinates describe the device display, never the target screen |
| Touch motion | `APP_TOUCH_MOVE` x/y are relative unscaled displacement on the active touchpad, with matching live gesture. Portable interaction accepts finite displacement -4096..4096 each, divides by 1/4/8, carries fractions and bounds emitted HID to -127..127. This is not the public relative mouse packet contract |
| Touch boundaries | Begin/end are absolute logical points with surface/control context. Physical release/cancel terminates capture even if a contact leaves the display or its normal action becomes stale; an out-of-bounds release never activates a control. Planned `touch.begin/move/end/cancel` owns a separate remote virtual gesture with authenticated channel, lease and UI revision; it cannot inject a physical wake/takeover event |
| Selection | Integer row-major cell 0..8, current page/group plus expected selection revision; nonempty cells only. `selection.focus` changes focus only; planned `selection.activate(cell,expected_selection_revision)` atomically selects/activates that cell under the captured page/group. Two levels: enter group, then append character. No pixel-to-letter or nearest blank-cell guess |
| Host modifier | Explicit `ctrl` or `command`. Copy C=0x06, paste V=0x19, undo Z=0x1d use left Ctrl=0x01 or left GUI=0x08. Tab=0x2b, Escape=0x29, Enter=0x28 use no modifier |

A browser can convert its own CSS movement to legal `mouse.move_relative` deltas, or use the separately qualified remote `touch.*` integration. Virtual touch accepts only declared surface IDs and its channel-bound virtual gesture, not physical gesture IDs, display-wake or takeover tokens. Fine gain applies once at its owning layer: direct relative mouse packets are already HID units and must not be divided again; virtual touch applies the shared divisor/fractional integration once. Never promise absolute positioning, target screen size, pointer acceleration bypass, target focus or successful application shortcuts from these reports.

## 6. Preview, direct text and cancellation progress

Preview editing changes device-local draft state only. Grid selection, case, symbol page, local Delete, Space and Clear never type into the host. `preview.replace`/append/delete/clear require expected draft revision and available edit state, not HID readiness merely to construct a draft. Send requires keyboard mode, a ready selected target, valid ownership, a matching nonempty draft revision and one explicit command. Editing is blocked while sending or settling. Preserve draft/target association; new target confirmation must be explicit before sending retained text.

`APP_TEXT` is a separate direct HID path: it takes up to 32 printable bytes and bypasses preview. Registry `keyboard.text.enqueue` must be unmistakably explicit in UI/MCP, require a direct-text capability and all HID gates, and never silently replace, append to or consume preview. It returns its own progress. The default human workflow remains preview then Send. Neither path appends Enter. There is no implicit fallback from a blocked preview send to direct text.

One preview send snapshots at most 64 characters and uses sequential batches of at most 32 under the same token/epoch. Failure to admit a later batch settles the job and retains never-started text. Cancellation settles the engine counters before allowing edits. Stale tokens, decreasing counters or counts above the snapshot total are invalid; `active:true` cannot reopen a settled token. Mode, lock, takeover, link/generation change and stop prevent residual sends from resuming.

Expose these separate values in the future domain result:

| Field | Evidence |
| --- | --- |
| `characters_started` | Count with accepted key-down submission; current engine `text_accepted` |
| `characters_completed` / `sent_prefix` | Count with accepted matching zero key-up; current engine `text_completed`, accumulated correctly across batches |
| `uncertain_index` | Started character whose key-up did not complete; excluded from automatic retry and presented separately |
| `never_started_suffix` | Snapshot bytes after the started prefix; eligible for a new deliberate send after reconciliation, never automatic replay |
| `reports_accepted` | Selected adapter accepted report submissions for this exact generation; not host receipt |
| `release_pending` | Neutral cleanup still needed or unconfirmed; completion/cancellation alone cannot clear it |

Current `update_send` removes the key-down-accepted prefix from preview, including a potentially uncertain last character, and does not yet expose the planned separate uncertainty record. A failed key-up must therefore never be described as a clean completed prefix or zero effect. Implementing the richer domain result is a prerequisite for promising the remote protocol's pair-completed semantics. Keep this fix shared with local UI rather than compensating in the API adapter.

`job.cancel` targets a scoped job token; `input.cancel` cancels the named active input work; `input.release_all` clears queued input and sends neutral reports only. Input stop cannot undo accepted clicks/characters or cancel an unrelated management job. Ordinary input cancellation invalidates the epoch, clears queued nonzero work and requires both keyboard and mouse neutral reports, including in audio/locked state. Short congestion preserves pending required releases; continuous congestion at least 250 ms cancels once and retains the barrier. Only zero recovery retries occur after a send failure, with the existing three-failure budget. Never clear `release_pending` because the retry timer elapsed or a disconnect was merely requested.

## 7. Exhaustive current action mapping

Current source rows inherit epoch/UI revision, lock, transport-transition, modal focus and mode gates unless a trusted safety/touch boundary is handled earlier. Future input/UI operations use matrix I/U guards; management uses M and configuration uses C, without an HID lease. Safety uses S and capture additionally needs P. `C:key` below means the shared `config.stage/apply/cancel` family with that field's scope, not a separate per-field setter. The current local integers explain source behavior; the wire schema uses the matrix's named fields/enums and explicit desired state.

| Current action | Current local effect / exact argument | Planned API semantic and edge cases |
| --- | --- | --- |
| `APP_MOVE` | Mouse move x/y through engine; local source | `mouse.move_relative(dx,dy)`; -127..127 each, mouse mode, preserve drag |
| `APP_CLICK` | Atomic click with x bitmask 1..7 | `mouse.click(buttons)`; mandatory release, reject during explicit drag |
| `APP_SCROLL` | Vertical wheel x, -127..127 | `mouse.scroll(wheel)`; preserve drag, no horizontal axis |
| `APP_TEXT` | Direct text from 33-byte terminated field; engine admits length 1..32 | `keyboard.text.enqueue(text)`; explicit direct send, separate from preview, never truncate at an embedded NUL |
| `APP_KEY` | One usage x, modifier y | `keyboard.key_tap(usage,modifiers)`; bounds above, no sustained keyboard hold |
| `APP_MODE` | x Mouse/Keyboard/Audio; real change cancels input and closes overlay | `mode.select(mode)`; same value no-op; audio page selection must not advertise working audio |
| `APP_LOCK` | Priority lock true; never unlock | `device.lock(true)` under S(`input.control`); no lease; false returns physical-presence error |
| `APP_RELEASE` | Priority cancellation and neutral barrier | `input.release_all` under S(`input.control`); report pending/failed release truthfully |
| `APP_RECOVER` | Cancel, refresh selected link readiness; no old input replay | `input.recover`; bounded explicit recovery, not host reconnection or proof of delivery |
| `APP_HAPTICS` | x 0/1 on Input settings page; desired preference and coalesced save | `C:haptics.enabled` under `config.write`; expose hardware availability separately |
| `APP_MOTION` | Toggle calibrated local mouse gyro | `input.motion.set` under I(`input.control`); planned owner/producer arbitration permits an authorized remote lease to select device IMU without physical confirmation, never concurrent streams |
| `APP_GAIN` | Mouse touch divisor x in 1/4/8, runtime only | `C:touch.gain_divisor` under `config.write`; no scaling of already expressed HID packets |
| `APP_DRAG` | Toggle left hold; toggle off cancels input | `mouse.drag.begin/end/refresh`; explicit state and remote deadman; end also S, never replay a toggle |
| `APP_MENU` | Legacy Modes/Target/Settings overlays; opening cancels input | `ui.navigate` with current shared-contract route; screen menu and AI double-click open the same on-demand mode/status/settings menu |
| `APP_CLOSE` | Close overlay; stop setup AP when leaving its page | `ui.close`; validate current context and operation ownership; cleanup required on setup exit |
| `APP_KEYBOARD_SUBMODE` | Motion/Touch/Spoken/Shortcuts; cancel, reset grid and local motion selection | `keyboard.submode.select`; choosing Spoken does not enable recognition capability |
| `APP_GRID` | Activate current nonempty cell 0..8 in Motion/Touch keyboard submode: enter group or append character | `selection.activate(cell,expected_selection_revision)` under U must be included in the registry. It atomically follows the shared cell-selection path; `selection.focus` remains focus-only and cannot substitute for this action (PC05) |
| `APP_BACK` | Return to groups, advance UI revision | `selection.back`; keyboard only, no host Backspace/Alt-Left |
| `APP_CASE` | Toggle draft-grid lowercase | `selection.case.set(lowercase)`; no host Caps Lock or transformation of already drafted text |
| `APP_SYMBOLS` | Toggle Letters/Symbols page | `selection.page.set(page)`; explicit enum; reserved Digits page is unsupported |
| `APP_DELETE` | Remove final preview character | `preview.delete(count:1)`; no host Backspace; reject/no-op classification explicit for empty draft |
| `APP_SPACE` | Append one printable space to preview | `preview.append(text:" ")`; capacity and revision checked, no direct host key |
| `APP_CLEAR` | Clear editable preview | `preview.clear`; no host select-all/delete, unchanged empty draft produces no save |
| `APP_SEND` | Begin one preview token and bounded batches without Enter | `preview.send(expected_draft_revision)`; one job, no implicit retry, precise prefix/uncertainty |
| `APP_STOP` | Cancel input and settle unsent preview | `input.cancel` under S(`input.control`); available across modes even though this local preview control is keyboard-only |
| `APP_HOST_OS` | Ctrl/Command setting on Input page; cancel input before change | `C:keyboard.shortcut_modifier` under `config.write`; mapping visible and persisted |
| `APP_SHORTCUT` | Copy/Paste/Undo/Tab/Escape/Enter in Shortcuts submode | `keyboard.shortcut(name)`; shared allowlist/modifier mapping; semantic API need not open Shortcuts page, but cannot bypass a local modal |
| `APP_VOICE_START` | Reports spoken-letter recognition unavailable | `speech.listen.start/stop` under M(`audio.capture`) plus P; unsupported today, future bounded consented listening job |
| `APP_VOICE_CONFIRM` | Reports recognition unavailable | `speech.candidate.confirm(candidate_id,revision)`; unsupported today; future preview append only, never implicit send |
| `APP_AUDIO_KIND` | Select Voice/Speaker placeholder in audio mode | `audio.kind.select` under M(`audio.control`); does not create profile/link/resource support |
| `APP_AUDIO_POWER` | Reports Bluetooth audio unavailable | `audio.enable.set` under M(`audio.control`), P for capture; unsupported today |
| `APP_AUDIO_MUTE` | Reports Bluetooth audio unavailable | `audio.mute.set` under M(`audio.control`), P for microphone unmute, S for mute; no lease needed for safety mute |
| `APP_AUDIO_VOLUME` | Reports Bluetooth audio unavailable; no implemented volume value | `C:audio.output_volume_percent` under `audio.control`; proposed 0..100 intersect qualified gain mapping, no fake applied volume |
| `APP_TOUCH_BEGIN` | Trusted display touch boundary, wake/capture/takeover before action gates | `touch.begin` under U uses a remote virtual gesture/context; it never obtains physical presence or takes over as local |
| `APP_TOUCH_MOVE` | Matching live Touchpad gesture; relative displacement through fine divisor | `touch.move` under U uses its channel-bound gesture and shared fine integration; direct relative HID API remains separate |
| `APP_TOUCH_END` | Finish trusted capture, cleanup survives ordinary stale action checks | `touch.end` ends its remote virtual gesture only; a stale end cannot terminate a newer gesture |
| `APP_TOUCH_CANCEL` | Trusted capture cancellation plus input cancel | `touch.cancel` under S(`input.control`) cancels the named remote virtual gesture; device lease/deadman remains authoritative |
| `APP_CALIBRATE` | Input page: cancel, start stationary sample calibration | `imu.calibrate`; bounded generation-bound job with actual quality/result, no fixed success timer |
| `APP_THEME` | Display page x 0 Light/1 Dark; persist changed value | `C:display.theme` under `config.write`; no write for unchanged value |
| `APP_SETTINGS_PAGE` | Legacy Home/Connectivity/Display/Input/About/Bluetooth/Wi-Fi pages; opening cancels | `ui.navigate` follows shared on-demand menu/settings routes; semantic configuration needs no physical page navigation |
| `APP_PAIRING_REQUEST` | Open/Close/Forget all/Disconnect request; opens confirmation, no immediate effect | `bluetooth.pairing.open/close`, `bluetooth.bonds.forget_all`, `bluetooth.disconnect` under M(`connections.manage`); exact operation's approval policy, no blanket physical step. Full bonds preserved; disconnect retains bonds |
| `APP_CONFIRM` | x 0 cancel/1 approve current local confirmation; revalidate BLE generation or exact Wi-Fi reviewed state | `operation.confirm/cancel` under original scope with exact operation ID/digest/revision; permitted remote approval works, physical-only challenge remains protected |
| `APP_WIFI_SETUP` | x 1 open protected provisioning AP, 0 stop; owns setup page | `wifi.setup.start/stop` under M(`wifi.configure`), stop also S; setup secrets remain private; runtime control must not appear on AP |
| `APP_WIFI_FORGET_REQUEST` | Snapshot current/stored network and storage-known/uncertain flags, open confirmation | `wifi.profile.forget` under M(`wifi.configure`) plus explicit destructive intent; apply only to the reviewed record/revision |
| `APP_BRIGHTNESS` | Display page, hardware brightness present, integer 10..100 step 10; persist on slider release | `C:display.brightness_percent` under `config.write`; desired differs from driver-applied brightness |
| `APP_IDLE_TIMEOUT` | Display page with brightness support; Off/60/300/600/1800 seconds | `C:display.idle_timeout_seconds` under `config.write`; exact enum, no extension by unchanged requests/polling |
| `APP_BLUETOOTH_PAGE` | Nonnegative page of four saved bonds; local code clamps to final page | `bluetooth.peers.list` under `view`; explicit pagination, no outbound connect by selecting a row |
| `APP_TRANSPORT_SELECT` | Connectivity page BLE/USB; cancel, wait old release completion, then apply/persist | `input.transport.select` under I, preference through C(`input.configure`); same selected value no-op, failure keeps old selection/preference |

### Non-enum functions and unsupported capability boundaries

The action enum is exhaustive for the current UI, not the entire requested API. The complete service matrix must also include these domains:

| Domain | Required planning coverage / current limit |
| --- | --- |
| State/capabilities | Read modes, lock/owner/barrier, board/adapter availability, separate BLE/USB/Wi-Fi/audio state, settings desired/applied/saved revisions and redacted diagnostics. Missing measurement is unavailable/null, not zero |
| Remote lifecycle | Enable/disable, enrollment/session/revocation, acquire/renew/release, request lookup and observation subscription. Not implemented by the input owner enum |
| Preview/selection configuration | Revisioned replace/append, focused cell/confirm, layout discovery and schema defaults/reset; these are planned semantic operations beyond present button actions |
| BLE | Saved bond list and real link/pair status exist; `bluetooth.target.request` and per-bond removal are not established by the UI action set. Advertise unsupported until the manager/provider behavior and operation-specific authority exist |
| USB | Selected HID path only, actual attached/ready/suspended/failed and generation. No audio, storage/file transfer, remote wake, generic control transfers or automatic fallback to BLE |
| Network | Provisioning and station state are current. Registry `wifi.*` operations and configuration use M/C(`wifi.configure`) without an HID lease. The network plan owns staging/apply/rollback, AP/radio and secret handling. Never expose passwords in status or ordinary logs |
| Audio/volume | Voice/speaker placeholders do not implement HFP, A2DP, AVRCP, USB audio, codec negotiation, volume or playback. Read truthful unavailable state; reject mutations with unsupported capability |
| Recording | No recorder/media store exists today. Planned recording/capture/media operations are first-class families with explicit local or enrolled `capture.consent` job/session approval, scopes, finite bounds, indicator, ACL and storage/export policy. Media uses a separately authenticated bounded resource, not ordinary command-frame payloads. Approved remote capture/export is permitted when implemented and qualified |
| Firmware/maintenance | No qualified OTA path; current partitioning is not evidence of A/B rollback. Planned `firmware.check/download/activate/discard` requires exact `update.check/stage/activate` scopes and approved policy or fresh explicit approval, not an always-physical step. An enrolled `admin.policy` may authorize bounded unattended updates remotely. Image authenticity/compatibility, inactive slot, release/mute/maintenance, boot confirmation and rollback remain mandatory. No arbitrary memory/flash/GPIO/shell access |
| Mac observation | Separate permitted capture/recognition evidence with target/frame/time/predicate provenance. Device HID acceptance cannot manufacture target-screen observation |

For a function that exists but is temporarily blocked, report `unavailable` with a specific reason such as disconnected, suspended, driver fault or resource busy. Use `unsupported` when implementation/schema/profile is absent. Qualification pending is separate from both. A compiled feature or present peripheral alone cannot set `available:true`.

## 8. Configurable behavior and immutable safeguards

Make practical product preferences configurable through the same validated schema, with units, bounds, defaults, persistence and apply class declared once. Stable keys/scopes come from the complete matrix and shared configuration contract. Rows below refine their profile design, not a second registry or extra per-field setter family. Existing portable validation bounds are source facts; candidate timing/pattern ranges are qualification inputs only, not advertised writable ranges. The effective schema exposes only the owning driver's qualified choices and safety ceiling.

Apply classes:

- **Live**: apply one validated desired-state change, report actual application, no input replay. Persistence is separate.
- **Input boundary**: settle/cancel output, advance epoch, clear gesture/filter/fractional state, establish neutral barrier, then install the entire validated profile. Do not restore motion/drag/send automatically.
- **Gesture boundary**: wait until buttons/touches/pending classification are idle, or explicitly cancel/consume the current gesture before applying. Never reclassify a press using new timing or actions halfway through it.
- **Manager transaction**: one generation-bound operation with release/revocation, apply/rollback and actual persistence outcome; no direct setter bypass.

### Input and interaction configuration

| Field / units | Current value and bounds | Proposed configuration and apply semantics |
| --- | --- | --- |
| Pointer gain, HID units/degree | Default 4; portable configure accepts finite 0..100 | Persistable user input profile; input boundary; gain 0 intentionally produces no gyro displacement |
| Gyro deadzone, degrees/second | Default 0.5; finite 0..10 | Same profile/bounds; input boundary; below threshold clears residual instead of delayed catch-up |
| Low-pass time constant, seconds | Default 0.03; finite 0..1; 0 bypasses smoothing | Same profile/bounds; input boundary; use actual sample dt, no fixed-frequency substitute |
| Selector cell width, degrees | Default 6; finite 1..45 | Same profile; input boundary and recenter selection |
| Selector hysteresis, degrees | Default 1; finite >=0 and strictly less than half cell width | Cross-field validation with width; never allow blank-cell focus |
| Fine touch divisor, dimensionless | Runtime 1; choices 1, 4, 8 | Expose explicit choice and optionally saved default; effective gains 1, 0.25, 0.125; input boundary clears carry; not a second remote-HID scaling layer |
| Post-interaction freeze, ms | Fixed 150 for touch/button/boundary suppression | Proposed integer 50..500, default 150; input/gesture boundary; cannot bypass active touch/menu/haptic suppression or pending keyboard-click freeze |
| Click/double classification window, ms | Fixed 280 after first release | Proposed integer 150..500, default 280; gesture boundary. Idle wake AI guard must derive from this same field, not a second hardcoded 280 |
| Physical hold threshold, ms | Fixed 900 | Proposed integer 600..3,000, default 900 and strictly greater than click window; gesture boundary. Physical lock/unlock remains available and non-remappable |
| Single-button mapping | Mouse toggles motion; Motion/Touch keyboard confirms; other current modes have no action | Proposed per-context allowlist of existing semantic commands or disabled. Default matches source; never allow scripts, hidden send, consent/unlock, duplicate single after double/hold or automatic Enter |
| Double-button mapping | Legacy source opens mode chooser or closes current overlay | Revised default opens the same on-demand shared menu as the screen entry, following the interaction contract. Proposed allowlisted navigation profiles retain that required menu access and recovery; gesture boundary. Double never also confirms a character |
| Hold-button mapping | Trusted physical lock/unlock | Fixed safety binding, not user-remappable to remove recovery. Optional secondary feedback preferences may not replace this action |
| Keyboard submode/default | Motion initially; Touch, Spoken and Shortcuts selectable | Persist default only after explicit configuration, feature-gated. Loading a default never starts motion, recognition or sending; motion intent stays off at boot |
| Character groups | Fixed 9 groups of 4 letters/digits, symbols groups of 3 or 4 | Proposed revisioned named layout profile: 3x3 groups, 1..4 printable ASCII bytes/group, no duplicate byte within a page, no empty entire page, each required declared character present once. Initial built-in Letters covers A-Z/0-9 and Symbols covers supported punctuation. Custom pages must declare coverage; unavailable cells stay unavailable |
| Group contents/order | Letters row-major `ABCD`, `EFGH`, `IJKL`, `MNOP`, `QRST`, `UVWX`, `YZ01`, `2345`, `6789`; symbol contents in owning interaction source | One shared UI/API/MCP layout, not a client-only rearrangement. Input boundary resets to an available group nearest center 4 and nearest available child center (currently 1 for built-ins); changing groups preserves existing draft bytes, requires new selection/draft revision |
| Keyboard encoding/layout | Fixed US ANSI printable ASCII; Ctrl default, Command optional | Initial schema allows `us_ansi` only and explicit host modifier. A future layout is a complete tested mapping profile; no arbitrary Unicode-to-usage guess or OS auto-detection. Modifier change is an input boundary |
| Case/page preference | Uppercase Letters initially; lowercase/page are runtime state | Proposed optional saved view defaults, separate from existing draft; explicit values, revision checks, gesture boundary. Reserved Digits remains unsupported |
| Draft capacity / requested preview limit | Static capacity 64; batch 32 | Matrix `keyboard.preview_limit_chars` may request 1..64, never less than current draft length. Reject rather than drop text. Static buffer growth remains a board/build/shared-model extension, not a writable capacity; no draft flash persistence by default |
| Calibration quality | >=64 samples over >=500 ms; acceleration norm within 0.08 g of 1, per-axis change <=0.03 g, gyro <=5 dps, variance <=0.04 (dps squared) | Board-qualified calibration profile. Not a normal user override that can mark moving/invalid samples calibrated. New profile revisions may change these through qualification; runtime reports actual measured quality |
| Sensor domain and sample gap | Finite gyro within +/-2,000 dps, acceleration within +/-16 g, 0<dt<=50 ms | Board-profile validated limits/sample timing, separate from user gain. Invalid/gapped samples clear carry and reject/reset calibration; no replay of accumulated old motion |

Configuration of a physical shortcut that produces HID must still meet current mode, owner, target, readiness and explicit-send policy. Do not map a convenient primary button to a hidden multi-action macro. All supported mappings are described as typed, discoverable schema values; unsupported contexts return an explicit validation error.

### Feedback, display and settings defaults

| Field / units | Current default / bounds | Proposed configuration and apply semantics |
| --- | --- | --- |
| Theme | Dark; Light/Dark | Existing persisted preference; Live, no write on unchanged value |
| Host modifier | Ctrl; Ctrl/Command | Existing persisted preference; input boundary, shared shortcut translation |
| Haptics enabled | Saved default true; actual enable also requires available motor | Existing persisted global preference; Live disable cancels all phases and requests verified motor off. No pulses, including test, while disabled |
| Haptic event policy | Confirm/mode/lock/error only; no per-motion pulse | Proposed enable mask for these feedback events, default all four. Visual error/lock indication always remains; cannot suppress neutral recovery or motor-off handling |
| Haptic patterns, strength percent and ms | Confirm 35%/24 ms; Mode 55%/45 ms; Lock 65%/65 ms; Error 65% during 0..40 and 105..165 ms; cooldown 200 ms | Proposed named profiles bounded by board maxima, strength integer 0..100 and each envelope <=200 ms, at most two nonzero phases. Minimum cooldown remains >=200 ms unless a newly qualified board profile is stricter. Live change only while motor off; no backlog, retrigger or old-pattern restoration |
| Haptic test | No current application action | Future explicit rate-limited <=200 ms test obeys enable, motor availability, board amplitude/duty limits and IMU freeze. Proposed configuration never enables raw indefinite motor output |
| Brightness, percent | 70; integer 10..100, step 10 | Existing persisted preference when board supports brightness; Live desired/applied states separate. Slider interaction previews locally but commits one chosen value on release, not one flash write per sample |
| Display idle, seconds | 300; 0/60/300/600/1800 | Existing persisted preference; Live changed setting wakes/restarts full deadline, unchanged value does not. Applies to all/unknown power states. Network/gyro/render polling is not physical activity |
| Preferred HID transport | Bluetooth; Bluetooth/USB | Existing persisted preference after successful manager transaction only. Never infer availability, silently fall back, or restore old queued input at boot |
| Startup mode/motion/lock policy | Mouse, motion off; no remote claim | Mouse remains default in initial schema. A future preferred view may be configurable; motion/capture/remote claim must remain off/unclaimed. Saved preference cannot auto-unlock or execute a job |
| Audio volume, percent | No implemented value | Future explicit 0..100 schema with real profile mapping, desired/applied status and safe default. Saving volume never implies audio on, unmute, microphone consent or playback |
| Remote service preference | Proposed disabled initially | After protected initial trust/scope enrollment, an enrolled `admin.policy` may enable/disable the permitted LAN policy remotely. Every boot/channel starts without an input owner or queued-job restore. Revocation is immediate; a client cannot grant itself missing authority |
| Defaults/reset | Device settings currently Dark/Ctrl/haptics on/70%/300 s/Bluetooth | Read defaults and revisioned reset by named group through C/original field scopes. No implicit bond/credential/recording/trust erase. Destructive factory/security reset is a separate explicitly approved scoped operation with a retained-data manifest; do not invent a physical step for ordinary preference reset |

### Schema, profile and persistence lifecycle

The proposed descriptor for every field includes stable name/type, unit, enum/range/step, default, board dependency, scope/presence, apply class, persistence policy, schema/profile version and current desired/applied/saved values or revisions. UI controls and MCP tool arguments derive from this descriptor. Validate the complete candidate, including cross-field timing/hysteresis/layout constraints, before any mutation. Reject unknown fields/versions, unsupported values and partial invalid transactions; no silent clamping or accepting a value the runtime ignores.

`config.stage` is inert: it validates/copies a bounded candidate at its expected resource revision without stopping input, changing connections or saving. `config.apply` rechecks the affected fields' actual scopes, generation, revision, availability and conflicts, then acquires their declared barriers and applies through the owning managers. `config.cancel` cancels only work still cancellable and cannot pretend to undo a commit already begun. One common transaction carries explicit durability choice; an allowed remote application needs no extra physical confirmation unless that particular field/operation declares a real protected challenge.

Keep board profiles separate from user preferences: axis mapping, sensor range/sample schedule, display dimensions/rotation, peripheral presence, USB pins/descriptors, motor driver amplitude/duty limits and capability qualification belong to the board adapter/profile. Users can choose only compatible, qualified profiles; ordinary settings cannot falsify hardware presence or bypass a driver fault. Effective configuration is documented built-in defaults plus selected board constraints plus explicit validated user overrides, never an undocumented per-client override. Snapshots identify every contributing revision.

Current storage is a 16-byte schema-2 record with CRC, not JSON or C structure serialization. Schema 1 migrates only its valid reserved-zero form to Bluetooth preference; unknown future versions, checksum failures and invalid values are retained/reported, not overwritten with defaults. The store initializes runtime defaults before a validated load but does not erase or write on boot. An explicit changed setting schedules a nonblocking save after one second of quiet; identical records cause no write. A save error is visible and does not retry every frame.

Future expanded configuration needs its own explicit versioned codec/migration and bounded storage budget. Keep the current valid record/recovery artifact until the new candidate is verified; define downgrade behavior before enabling the new version. Migration derives new fields from declared defaults and records origin/version without performing HID, unmuting, applying credentials or resuming a job. Migrations and device boot must not become repeated default writes. No per-key, per-motion, lease-renewal or status-subscription flash writes.

Proposed settings apply is compare-and-set against one desired revision, with at most one manager transaction. Report `unchanged`, `applied`, `pending`, `saved` and `failed` separately; a runtime-applied preference may remain unsaved after storage failure. Coalescing may save a later complete revision, but the API must identify which revisions are covered. Rejected/stale/duplicate requests schedule no new save. A previously failed explicit retry uses a new request and retains the desired revision; never imply a cached accepted result proves a later commit. Transport preference remains saved only after safe selection, while network/update use their own staged commit/rollback policy.

Immutable safeguards are authority provenance, physical-only unlock/initial trust enrollment, explicit bounded capture-consent authority, one active HID owner/target/producer, mode/epoch/generation rechecks, mandatory release, deadlines/resource limits, no reconnect replay, truthful progress and no automatic input/capture/motion restore. Valid scoped remote consent/update approval is not a bypass of these invariants. Configurable values stay inside advertised qualified envelopes; expert settings/layouts/profiles/MCP cannot expand authority or disable the safeguards.

## 9. Manager transitions, results and errors

Transport selection first cancels/settles current work, records the old path generation, and waits for both neutral reports and real adapter drain. A newly admitted final zero report is still in flight: use a fresh post-send snapshot and do not switch merely because the engine cleared its local barrier. Current switch timeout is 5 seconds. Old-host connected generation replacement, unavailable target path, suspension/fault or timeout aborts without changing selection/preference. A disconnected old host cannot accept releases; explicit selection may proceed while reporting that limitation. New-path generation needs fresh neutral reports before nonzero output. USB detach does not silently select BLE.

Pairing similarly validates the exact reviewed generation, drains old output and reports actual disconnect/bond operation completion. Disconnect retains bonds. Bond-full does not erase existing hosts. Saved host rows are inspection, not an implemented outbound connect command. Wi-Fi forget compares current/stored network identity and known/present/uncertain flags again after confirmation; changed state requires a new review. Network application that drops the control channel can yield `result_unknown`, not an invented successful terminal result.

| Result | Exact claim |
| --- | --- |
| `rejected` | No new effect was admitted by this request; a prior duplicate may already have effects |
| `accepted` | Validated immutable work and result capacity reserved under current gates; no report/save/application claim |
| `running` | Work began; include counts, generation and deadline. A held mouse remains running until release |
| `applied` / `unchanged` | Device-local transition completed or desired value already matched; include resulting revision/epoch |
| `saved` | Settings commit confirmed for identified revision; hardware/network application remains independently stated |
| `sent` | All reports required by this HID operation, including matching releases, accepted for the exact selected adapter/generation. BLE SDK acceptance and USB submission/completion are distinct adapter evidence, never target UI receipt |
| `cancelled` / `failed` | Job stopped with reason, exact started/completed/uncertain progress and actual release state; does not undo prior effects |
| `observed` | Separate authorized Mac observer result with target/capture/frame/time/predicate and verified/inconclusive/failed evaluation |
| `result_unknown` | Sufficient execution evidence unavailable; neither zero effect nor permission to replay |

USB command results must preserve queued/admitted, submitted and transfer-completed stages. Quarantined in-flight USB state is not recovered by a software retry; actual reset/detach/reinitialization evidence is needed. BLE pending notification/congestion and completion failure remain visible. One transport's accepted result cannot be reused for another generation or elevated to a shared host-delivery guarantee.

| Error family | Required reason examples and behavior |
| --- | --- |
| Framing/schema | `unsupported_version`, `invalid_argument`, `unknown_operation`, `out_of_range`, `text_encoding_unsupported`, `layout_unsupported`; no partial effect or truncation |
| Authentication/authority | `unauthenticated`, `forbidden_scope`, `remote_disabled`, `local_presence_required`, `approval_expired`; validate exact original/enrolled authority, never self-enrollment or a forged physical challenge. A permitted scoped remote confirmation is valid |
| Ordering/state | `sequence_gap`, `request_conflict`, `duplicate_too_old`, `stale_boot`, `stale_epoch`, `stale_revision`, `target_changed`, `stale_job`; provide safe current revision/expected sequence where authorized |
| Ownership/readiness | `owner_busy`, `local_busy`, `lease_required`, `lease_expired`, `locked`, `wrong_mode`, `release_pending`, `recovering`, `disconnected`, `not_ready`, `suspended`, `congested`; do not infer readiness from connection alone |
| Capability | `unsupported`, `unavailable`, `qualification_pending`, `driver_failed`; include operation-specific reason, not a fake applied result |
| Capacity/time | `queue_full`, `job_busy`, `rate_limited`, `deadline_expired`, `deadline_out_of_range`; reject before acceptance if capacity cannot be reserved |
| Execution/storage | `transport_failed`, `release_failed`, `calibration_failed`, `apply_failed`, `save_failed`, `rollback_failed`; preserve actual partial progress and recovery state |
| Missing evidence | `result_unknown`, `observation_unavailable`, `observation_inconclusive`; no automatic resubmit or success-shaped fallback |

Initial command records are bounded to 16 globally; engine queue is separately 32 and one text batch/job is active. Remote frame/result/state/rate limits follow the existing protocol (2,048-byte command, 64 result records/channel, 60 commands/second burst 8, separate bounded safety lane). The new schema must include every real limit and account for copied text/layout/profile memory before admission. No nested batch/macro creates an unbounded implicit queue.

## 10. Acceptance cases and integration decisions

Before new API implementation, the overall matrix/schema must use the 64/32 text limits, one-usage key contract, completed-versus-started progress and explicit `selection.activate` semantics. PC07 requires the unimplemented shared-model/dispatcher extension for remote-controlled IMU and virtual gestures; PC06 requires readable capture indication and explicitly granted file-only continuation independent of the HID lease. Preserve the resolved wake-before-takeover ordering, USB evidence stages and shared configuration constraints: hysteresis strictly below half spacing, and effective preview limit 1..64 distinct from compiled capacity and never below the existing draft. Scoped update/consent authority and shared-menu routes follow the current matrix/interaction owners, superseding older blanket physical-approval/fixed-navigation proposals. Do not leave contradictory schemas or enable these planned capabilities before their implementation and qualification.

Required semantic cases supplement the remote test plan:

1. Every one of the 48 audited actions has the documented domain equivalent, client-only navigation meaning, presence boundary or truthful unsupported result. A new enum requires a reviewed mapping update.
2. Identical valid local/remote domain commands produce the same transition and HID bytes after trusted-source/ownership checks; remote adapter cannot mint local events, bypass mode/modal gates or unlock.
3. Invalid bounds, Unicode/newlines/NUL, unsupported usages/chords/pages, blank grid cells and over-capacity text have no partial mutation, report or write.
4. Case, symbol, group, delete/space/clear and physical selection confirmation alter preview only; explicit Enter/direct text/send remain distinct. Stale draft/selection revision cannot act on new text.
5. Cancel before first key, after accepted down, after matching up and between batches preserves completed/uncertain/never-started distinctions. Late completion cannot reopen or consume a new job.
6. First local takeover/wake gesture, delayed click, double, continued hold, lost edge and modal revision interleavings cannot click/type/unlock accidentally or restore old remote work.
7. Duplicate accept/reject/renew/settings requests, cache eviction, gaps, conflicts, new channel and reboot cannot repeat input, extend hold/lease or create extra writes.
8. Lease/deadline expiry, clock regression, congestion, release failure, old-host replacement during final release, USB quarantine/reset and target/path changes stop stale output and preserve truthful recovery state.
9. Configuration validates whole candidates and cross-field bounds; gesture/profile changes cannot reinterpret an in-flight press, restore motion or change already drafted bytes. UI/API/MCP use the same schema/defaults.
10. Unchanged settings, slider movement, polling, boot/default/migration and lease traffic produce no unintended storage writes. Pending/applied/saved revisions and failed save/retry remain distinct; corrupt/unknown records remain recoverable.
11. Disabling haptics mid-pattern prevents later nonzero phases; unsupported hardware and suppressed tests remain truthful. Pattern customization cannot exceed board bounds or disable motor-off/IMU safety.
12. Audio, volume, recording, outbound BLE connect, arbitrary USB operations and firmware update remain unavailable until their real implementations, consent/rollback/resource plans and qualification exist. Device result never fabricates Mac observation.
13. Once capabilities exist, an enrolled `capture.consent` authority can explicitly approve the bounded job/session, and a matching authorized update policy can admit remote staging/activation without repeated physical prompts. Missing/wrong/expired authority is rejected; ordinary input/admin scope never substitutes for privacy consent. Physical unlock/initial trust, lock, local override and release/mute barriers remain enforced.
14. Core-only mode workspaces and their screen entry/AI double-click share the same on-demand mode/status/settings menu. Navigation uses shared route/revision validation; approved manager/configuration operations do not require opening those pages or an HID lease.
15. PC07: leased remote IMU selection/disable, producer conflict, calibration/not-ready rejection, short/backpressured samples, lease/liveness expiry, mode/target change, current wake-only gesture and subsequent fresh physical takeover all use the shared dispatcher. No physical handoff is required to enable the permitted remote producer; no direct/scripted movement reaches HID while it is selected.
16. PC07: virtual gesture source/channel isolation, stale boot/epoch/revision/target, malformed order, duplicate begin, finite deadline/deadman, lost end, late cancel after replacement, overload and owner loss cannot forge wake activity, local ownership, consent or unlock. Reserved cleanup remains bounded and no virtual gesture survives its lease/deadline.
17. PC05: activating a group changes level without typing; activating a character appends exactly once to preview and recenters. Focus-only, empty cell, stale page/group revision, duplicate activation and full/effective-limit draft cases never become implicit typing or confirmation.
18. PC06: recording blocks blanking while maintaining a readable actual indicator and local Stop; stalled/failed indication stops capture with truthful mute evidence. Explicit bounded file-only `continue_local` survives HID lease loss only with its background grant, emits no live media after owner loss, and still stops on privacy/lock/grant/resource/deadline limits. Default jobs stop; reconnect/boot never resumes them.

Documentation hygiene, local-link checks and action-enum coverage can validate this plan's completeness against the audited source. They do not qualify a server, device, host, security implementation or any proposed configuration range.
