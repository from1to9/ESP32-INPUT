# Remote control validation plan

Status: proposed tests, 2026-10-21. No remote firmware, network security, BLE delivery, audio or screen-capture qualification is claimed by this plan. The Wi-Fi foundation is published at d0f1f6; current product/coordinator/BLE/USB source is distinct from the still-planned authenticated LAN/MCP interface. The proposed contract is [remote-control.md](remote-control.md); existing local acceptance remains in [validation.md](validation.md). The [complete matrix](api-function-matrix.md), [shared configuration](configuration.md), [network/update plan](wifi-management-ota.md) and [audio/recording plan](api-audio-recording.md) define full planned coverage. Finish these contracts before implementing new interfaces.

## Evidence and harness

Test the production coordinator, parser, admission logic, queue and scheduler. Substitute monotonic time, authenticated transport and BLE send results only at their real boundaries; do not add test-only behavior or a second remote-to-BLE path. Record accepted/nonzero/neutral report traces and state revisions so an apparently correct final snapshot cannot hide an earlier unintended emission.

Use three levels, recording each separately:

1. **Host protocol tests:** deterministic time, actual parser and production coordinator, fault-injected transport/manager adapters, sanitizer and strict compiler checks where supported.
2. **Target integration:** pinned SDK/BSP/firmware configuration on the actual S31 board; real TLS/WSS, heap/task-watermarks and Wi-Fi/Bluetooth contention. Use two independent client processes and an adversarial browser origin.
3. **Host compatibility:** actual iPad Pro 2018, iPhone, Windows and macOS versions as available; controlled focus in a test field, physical takeover and real drag/key release. Qualify audio combinations and capture paths separately.

A fake BLE adapter proves scheduler behavior, not radio delivery. A BLE API acceptance trace proves `sent` as defined by the contract, not the target UI outcome. A target screenshot or manual observation proves only its recorded predicate. Do not replace missing measurements with zero or infer qualification from compilation.

Every case starts from a captured boot ID, epoch, target, mode, lock, owner, release barrier, capability set and known queue. Observe both the request ledger and emitted report trace. Check that secrets and private payloads are absent from retained logs. Use synthetic text and test credentials; keep actual machine/network evidence in `.agent/local/`, outside tracked source.

## Coverage matrix

All rows are **planned / not run** until an evidence record says otherwise. `H` means host tests, `D` device integration and `C` actual target compatibility. All safety-relevant rows require their listed levels before the corresponding capability becomes available.

| ID | Scenario / stimulus | Required result and evidence | Level |
| --- | --- | --- | --- |
| B01 | Cold boot, no Wi-Fi, no Mac; saved credentials also present in a separate trial | Local mouse default, motion off, audio off/muted, remote disabled or explicitly configured service but unclaimed; no old input/session/job replay | H/D/C |
| B02 | Query capabilities for each absent/local-only backend and compiled-but-unqualified API/provider combination | Absent/pending/failed/unavailable distinguishable; buttons disabled with reason; no pretend pairing, capture or sending | H/D |
| B03 | Same bond remembered for BLE and Classic, only one link active | Separate target/profile/readiness indicators; no aggregate connected/qualified assertion | H/D/C |
| A01 | No credential, wrong credential, expired/revoked credential or invalid scope on state/WSS/command | No sensitive first snapshot and no admission; bounded errors; revoked owner cancels and releases immediately | H/D |
| A02 | Browser login/logout, session idle/absolute expiry during a drag/text job | Cookie flags and no-store correct; logout/expiry closes channel, invalidates lease, releases and cancels; old cookie/channel cannot continue | H/D |
| A03 | Enroll without local window, wrong/expired setup material, sixth failed attempt, replay enrollment | No trust change; window expires/closes per policy; fresh local presence required; setup material never logged | H/D |
| A04 | MITM certificate, changed pin, name mismatch, expired direct-browser certificate | Connection fails visibly, no downgrade or input. Legitimate re-enrollment/renewal requires the documented trust path | D |
| A05 | Hostile web origin opens WSS with victim cookies; missing/null/lookalike Origin; rebinding Host | Reject before upgrade/state disclosure; exact allowlist, no wildcard/substring match; native missing-Origin exception requires native credential | H/D |
| A06 | Cross-site form/JSON mutation, missing/wrong CSRF header, CORS preflight, GET mutation attempt | No state change; login/bootstrap also protected; read-only state not exposed cross-origin | H/D |
| A07 | Reuse/expire a WSS ticket or attach it to another session; send before initialized channel | Denied before control admission; ticket is single-use/session-bound, no query/log secret leakage | H/D |
| A08 | Login as viewer, submit control/config/pair/update or inspect private preview | Scope rejection; no lease, preview payload or side effect; UI and MCP availability agree | H/D |
| A09 | Access via provisioning AP, unexpected interface, arbitrary-origin local HTML or unintended Mac bind address | Production control unavailable on setup service; permitted interface/loopback/Host rules enforced; no input leakage | D |
| P01 | Oversized length, fragmented oversize total, excessive depth/members, binary/compressed/batched messages | Bound before allocation; reject/close and revoke as specified; no partial command; healthy local operation remains available | H/D |
| P02 | Invalid UTF-8/JSON, duplicate keys, unknown fields/op/version, null/type confusion, NaN/Infinity, huge/fractional integers | Parser/schema rejection without mutation or truncation. Duplicate JSON keys must be rejected, not last-value-wins | H/D |
| P03 | Boundary values for deltas, wheel, text, key count/modifier, revisions and deadlines | Exact legal values accepted; immediately adjacent illegal values rejected; no signed overflow, wrap, clipping or implicit newline | H/D |
| P04 | Valid request repeated with same ID/sequence/digest before completion and after result | Return original running/terminal record; one admission/effect only; no lease/deadline extension | H/D |
| P05 | Same ID or sequence reused with changed body/gates; old ID at new sequence | `request_conflict`, close/revoke; never execute changed request or treat it as idempotent | H/D |
| P06 | Fill/evict 64-result cache, retry evicted sequence; pin executing entries | High-water rule rejects old sequence as unknown/too old; no replay, bounded memory, active record retained | H/D |
| P07 | Sequence gap, rejected next request, counter exhaustion; then valid next request | Gap not consumed; processed admission rejection consumed; client receives correct next sequence; channel rotates before wrap | H |
| P08 | Old boot/session/epoch/target or stale async manager/BLE acknowledgement | No nonzero HID or wrong-target state mutation; stale acknowledgement cannot clear a new release barrier | H/D |
| P09 | Deadline past, at exact expiry, beyond horizon, expiry while queued/between text characters | No new nonzero report at/after deadline; remaining work cancelled with truthful prefix; required neutral release still runs | H/D |
| P10 | Change client wall time/timezone; delayed state used for deadline estimate | Device monotonic checks unchanged; stale/overlong request rejected; no deadline extension on retry | H/D |
| O01 | Owner acquisition without local allowance, while local drag/send/motion/modal/listening active | `local_busy`/not allowed; no preemption, hidden cancellation or unexpected motion | H/D/C |
| O02 | Two clients acquire simultaneously; observer continues reading | Exactly one owner; loser sees `owner_busy`; observer has no control path or secret lease material | H/D |
| O03 | Renewal wrong client/lease, stale state sequence, late renewal, ping-only liveness | No extension. Expiry cancels/releases; measured initiation within 3,000 ms lease plus proposed 50 ms scheduling budget | H/D |
| O04 | Asleep whole first wake gesture, then fresh local AI/touch during remote movement/drag/send | Whole asleep wake gesture changes no ownership/lock/input. Fresh post-guard gesture revokes input/epoch and is consumed for takeover; next deliberate local action works; no automatic reacquire | H/D/C |
| O05 | Hold AI through takeover to global lock; try remote unlock and fresh AI tasks | Local long press locks, releases/cancels/mutes. Remote unlock rejected, including admin/AI; no lease while locked | H/D/C |
| O06 | Local long press unlocks after queued remote work | Mode retained, motion off/mic muted, queue empty, local allowance closed; no resumed task or output | H/D/C |
| O07 | Remote attempts raw physical-button/local-source/presence fields; hidden page activation | Schema rejects spoofed source; semantic action obeys visible focus/mode and presence rules | H/D |
| O08 | Remote mode change or output-affecting setting while old commands are queued | Epoch increments; old queue cancelled, neutral barrier holds; next command must use returned epoch; no cross-mode output | H/D/C |
| T01 | Clean WSS close, Wi-Fi loss, AP reboot or TCP blackhole during drag/key/text | Detected channel close revokes exact input authority and live channel consumers; same-session HTTP management/file recording remain independent. New channel requires fresh acquire; old callbacks/PCM cannot bind to replacement. Silent loss is bounded by lease/deadman. Trace releases, cancellation and precise progress | H/D/C |
| T02 | Lose only outbound state/results while inbound renewals/commands still work | Fresh-state requirement expires lease; bounded output cannot accumulate indefinitely; UI stale and stops; lost result is unknown | H/D |
| T03 | BLE disconnect, sleep/wake or selected target replacement with queued input | Revoke/cancel input, change epoch, retain only safe unsent draft; reconnect starts neutral and motion off. Independent audio jobs retain their own link/resource/privacy policy | H/D/C |
| T04 | Reconnect with same auth session and retransmit prior buffered messages | New channel session, no lease, fresh snapshot/ack; old envelopes rejected; browser/bridge sends no offline queue | H/D |
| T05 | Restart bridge, browser or device after accepted reply but before terminal result | Known record returned if available; otherwise `result_unknown`. No automatic retry or fabricated failed/sent result | H/D |
| T06 | Pointerup never arrives; tab blur/hide, pointercancel or input capture loss | Best-effort stop/release sent; mouse hold watchdog releases even if notification lost; reentry needs explicit acquire | H/D/C |
| F01 | BLE send API rejects mouse/keyboard neutral, transiently and persistently | Release barrier retained; bounded retries, then real link-teardown attempt/result; no nonzero report until new neutral barrier accepted | H/D/C |
| F02 | Key-down accepted, key-up fails during text; cut link at each report boundary | started_prefix follows key-down acceptance; completed_pairs counts matching releases; uncertain character isolated and never-started suffix retained without auto-send; possibly typed characters never reinserted/retried | H/D/C |
| F03 | Full data queue/rate limit, then local lock, remote stop and release | Overflow rejected before acceptance; reserved safety lane still works; no dropped key-up or accepted action disguised as complete | H/D |
| F04 | Delayed asynchronous success arrives after cancellation/replacement | Old job stays cancelled; no resurrection, new-target update, consent reuse or barrier clearance | H/D |
| F05 | TLS handshake flood, slow reader, malformed-frame flood and connection limit | Caps hold; healthy owner not evicted for a newcomer; close stalled/abusive channel, safe release, bounded heap/tasks | H/D |
| F06 | Long pairing/calibration/network job times out or loses session | Job has its own bounded deadline; cancels safe work, ignores stale callback, exposes failure/unknown honestly; no silent permanent busy | H/D |
| U01 | Mouse click/drag/scroll/fine control; remote requests gyro activation | Matching production semantics and HID units; no double click from one request; gyro starts only after an authorized exclusive producer transition and new epoch; no bypass of the existing production coordinator | H/D/C |
| U02 | Keyboard selection stale revision; case/symbol/delete/space/clear; edit while sending | Stale confirm rejected; shared preview rules; no direct HID from focus motion; active job snapshot immutable and conflicting edit rejected | H/D/C |
| U03 | 64/65-character preview and 32/33-byte engine batches, Unicode/control/newline, explicit Enter | Exact limit, no truncation; unsupported text rejected; text never appends Enter; Enter only through explicit key action | H/D/C |
| U04 | Host layout and Ctrl/Command profile differ from displayed selection | UI identifies profile/layout; unsupported combination unavailable; no silent wrong shortcut mapping | H/D/C |
| U05 | Scoped remote pairing, bond deletion or target change; wrong/expired approval or required new-trust challenge | Exact manager authority/revision and destructive intent enforced; permitted remote approval needs no blanket physical step. Physical-only/host challenges cannot be fabricated; timeout/cancel leaves pending trust unchanged | H/D/C |
| U05a | Required physical confirmation of a remote proposal causes input takeover; try stale continuation/digest/client/target | Wake routing precedes takeover; first eligible takeover action is consumed and old input cancelled. Explicit review creates a new local intent or exact one-use approval. Input continuation needs a fresh lease; management uses its own generation. No stale callback bypasses lock/takeover | H/D/C |
| U06 | Calibration moving/nonfinite samples; invalid IMU settings/revision; cancellation | Output suppressed; quality-based failure with no bogus bias; invalid candidate changes nothing, actual apply/restore verified per manager, stale result ignored | H/D |
| U07 | Speech result after window closed/lock/target change; no recognizer present | Stale candidate rejected; absent capability says not implemented; valid candidate requires confirmation and separate send | H/D/C |
| U08 | Remote unmute/listen without consent; consent expiry/disconnect/lock; mute without lease | Unmute denied without purpose-bound consent; default session-bound capture stops/mutes on loss, explicitly granted finite file-only continue_local follows its policy; lock/local Stop/revoke always terminate; indicator remains truthful | H/D/C |
| U09 | Audio start with unsupported profile, unavailable codec, no concurrency qualification | Reject reason; HID remains safe; no fake audio success; same-host HID+HFP and HID+A2DP qualified separately | H/D/C |
| U10 | Speaker enable/volume/playback with missing AVRCP; speaker-only operation | Supported desired state applied only after manager result; unavailable playback disabled; no unintended mic capture/upload | H/D/C |
| U11 | Stage invalid Wi-Fi, cancel, apply valid/invalid config, fail rollback | No credential exposure; common config stage/apply guards and actual runtime/saved state; independent Wi-Fi change does not select/disconnect HID; bounded attempt/restoration result is not guaranteed rollback | H/D |
| U12 | Diagnostic snapshot/capture, resource saturation and log inspection | Redacted bounded output; opt-in capture caps; no key/text/mic/credential leakage; diagnostics do not change input behavior | H/D |
| U13 | Toggle global haptics off/on from device and remote settings, then mode/confirm/lock/error interactions | One authoritative saved setting; off means no pulse, on emits only bounded interaction feedback; no pulse for each pointer/IMU frame; IMU freeze/reset covers motor activity | H/D/C |
| U14 | Haptics test when disabled/missing/faulted; repeated test at rate limit; hold driver active | Unavailable/disabled reason, no success stub or continuous buzz; 200 ms maximum plus bounded driver shutdown; visual status remains accurate | H/D |
| U15 | Concurrent settings edits, power loss during commit, flash/apply failure, schema migration and corrupt settings | Inert stage, revision/dependency validation and manager-confirmed apply; acknowledged/read-verified save or sticky storage uncertainty; errors can occur before/after mutation, no guaranteed old-record preservation or atomic hardware rollback; explicit recovery/migration preserves evidence | H/D |
| U16 | Reboot after enabled motion/unmute/remote lease; import incompatible settings | No session/hold/job/active-consumer/motion restoration; saved grants remain permission only, mic muted, remote unclaimed; board/schema import validated and no imported presence/unlock | H/D |
| V01 | Build portable coordinator/protocol tests against Mosaico and a second board adapter with different pins/drivers | Shared domain/remote behavior unchanged, no Mosaico headers or pin constants in domain; board build and initialization separately evidenced | H/D |
| V02 | Adapter reports absent motor/display/mic/IMU, failed driver, or BLE-only MCU without Classic | Accurate API/UI/MCP capability reasons; no fake operation. ESP32-S3 Classic HFP/A2DP unavailable; display absence does not fabricate a local confirmation | H/D |
| V03 | Headless/no-button board lacks qualified physical takeover/consent mechanism | Remote input and capture remain disabled; browser/AI cannot satisfy local presence. Qualified alternative hardware mechanism needs its own tests | H/D |
| S01 | Drop/duplicate/reorder state events or miss heartbeat | Counters/generation detect stale state; controls disabled by 1,500 ms; replacement snapshot requested, no input reconstruction | H/D |
| S02 | Send accepted but no reports; reports accepted but target focus wrong; capture unavailable | Three distinct outcomes: accepted, sent, observation unavailable/failed/inconclusive. Never display target success from transport alone | H/D/C |
| S03 | Read-only observer while private preview/capture/another client's result exists | Only permitted metadata visible; no private content, secrets or cross-principal request history | H/D |
| S04 | Queue cancellation, recovery, unqualified audio and missing health measurement | Exact queue/error/readiness state; unknown measurements null with reason; disabled controls explain availability | H/D |
| M01 | MCP tool valid/invalid arguments, missing capability, denied scope or device lock | Same device policy enforced; typed structured result; no tool path calls BLE or unlocks directly | H/D |
| M02 | Tool timeout/cancellation, caller dies or stops consuming state | Input/session-bound work cancelled with best-effort stop; finite explicitly policy-bound update or file recording lifetime enforced separately, no perpetual renewal; emitted prefix/unknown preserved | H/D |
| M03 | Two AI callers or human tabs behind one bridge compete | Explicit logical owner arbitration; loser cannot borrow device lease; local device override still wins | H/D |
| M04 | Model/screen text requests hidden privilege, another target, arbitrary URL, secret or unlock | Data cannot become policy; tool/schema/device scopes reject; secrets never reach model/tool output | H/D |
| M05 | Wrong/old capture target, stale frame, revoked Mac capture permission, OCR ambiguity | Observation names provenance and becomes inconclusive/unavailable; dependent AI plan stops without guessed clicks | H/D/C |
| M06 | Bridge restart, changed device pin or LAN address; loopback hostile-origin access | No insecure reconnect or lease resume; new state/trust checks; local Host/Origin/CSRF/session isolation enforced | D |
| W01 | Ordinary control credential attempts firmware upload/reboot/update | Common registry rejects missing update/admin scope. A matching current scoped background policy authorizes a job without a per-update physical prompt; ordinary input tools gain no update authority | H/D |
| W02 | Unsigned/wrong target/oversized/downgrade image or interrupted upload | Reject before boot selection; old app stays bootable; no arbitrary partition/bootloader writes | H/D |
| W03 | Update entry during input, power cut, failed first-boot self-check, rollback | Release/mute/revoke first; qualified A/B recovery and explicit post-boot confirmation; new boot remains unclaimed | D/C |

## Full registry/configuration/privacy/update cases

All rows are planned, with H/D/C evidence levels as above. These extend the older input-focused matrix and inherit every schema/session/generation/capability test.

| ID | Scenario | Required result | Level |
| --- | --- | --- | --- |
| API01 | Enumerate current app_action_t, registry rows, generated UI/API/MCP metadata | Every current action covered exactly once; every planned family/status/error present; unsupported actions fail truthfully | H |
| API02 | Same configuration change via UI, POST commands, WSS and MCP; unknown/out-of-range/dependent values | One config.stage/apply/cancel validator/owner; same choices/reasons/effective state, no duplicate setters | H/D |
| API03 | Inert stage, concurrent apply, manager failure/reconnect/restart, repeated same-value saves | Stage has no hardware/storage side effect; accurate manager apply/restart/durability, coalesced changed-only writes | H/D |
| API04 | Flash set/erase before/after-write failure, corrupt/newer profile, power loss | Runtime and stored evidence separate, sticky uncertainty survives radio/UI success; explicit verify/recovery, no partition erase or invented rollback | H/D |
| API05 | Wi-Fi join/loss/profiles/API with BLE/USB mode/target switching | Network and HID identities/generations independent; no redirected input, remote lease loss handled through coordinator | H/D/C |
| API06 | Authorized remote admin enables/narrows/revokes background update policy | No forced per-update physical prompt for matches; source/principal/version/size/time/maintenance/restart budget enforced, no self-scope expansion | H/D |
| API07 | Policy expiry/revision change/revocation/local lock during update check/stage/activation | Before-commit cancellation and cleanup or explicit too-late/boot uncertainty; no unlock, hidden retry or unverified boot selection | H/D |
| API08 | Recording/mic start with missing/expired/wrong-source/destination grant; explicit standing grant | Explicit consent enforced; in-grant starts need no repeated local confirmation, outside-grant denied; preferences alone never activate capture | H/D |
| API09 | Capture indicator fails/stalls or idle screen blank requested during capture | Capture rejects/stops unless a qualified visible indicator remains; local Stop/mute/lock always work, no false mic-off acknowledgement | H/D |
| API10 | Media owner-channel/session loss versus HID lease loss, with default or granted finite continue_local | Default stops on its own owner loss; HID lease loss alone never supplies audio authority or changes media lifetime. Live egress stops on owner loss; only explicitly transferred file-only work with source consent plus distinct background grant continues within both bounds; boot never resumes | H/D |
| API11 | Recording size/time/quota, unsupported-v1 pause/resume/seek, slow storage/reader, finalization/power loss | Unsupported operations reject without stop/start aliases; no unlimited capture, silent dropped samples or false complete artifact; partial/uncertain state and protected recovery | H/D |
| API12 | Cross-principal recording list/range/export/delete, untrusted IDs/paths, delete failure | ACL/scope, bounded immutable reads, no arbitrary files/URL or secret leakage; deletion not a secure-erase claim | H/D |
| API13 | Every advertised audio format/frame/ring/route and shared-resource combination | Exact capability tuple and memory/frame bounds; no relabeled samples, fake gain/mute/AEC/stereo or incompatible fan-out | H/D/C |
| API14 | Input-first/balanced/management-first requested above ceilings; scan/TLS/flash/audio pressure | Shared schema limits, immutable stop/release priority, finite visible deferral and maintenance policy; no hardcoded client divergence | H/D |
| API15 | All event topics/statuses, slow consumer, reconnect, schema/profile pagination | Scoped bounded replacement state; accurate unknown/unavailable/accepted/applied/saved/sent/observed; no input/media replay | H/D |
| API16 | Fresh native network selection, editor beyond 30-second list TTL, replaced list, five-minute ticket expiry/release/replay | Immutable original SSID/auth retained only by explicit ticket; ordinary stale rows still reject, no silent manual conversion, no password in HID/intents/state/logs, editor close cannot cancel a consumed join; local ticket is not remotely borrowable | H/D |
| API17 | Private local name submission, same change through future common config, tap setting change during a gesture | Shared name validation/manager ACK/save evidence; no forged local editor/gesture, no host-cache claim, no config-triggered click or active-gesture reinterpretation; all current enum actions covered | H/D |

## Invariants checked across faults

- No nonzero report when locked, release-pending, disconnected, wrong mode/target/epoch, lease-expired or request-expired. Releases use the dedicated neutral-only path.
- Every accepted input request has one bounded record and ends sent/cancelled/failed, or is honestly unknown to a caller that loses evidence. Rejection before admission emits nothing from that new request.
- Each admitted keyboard down has a scheduled release; each held mouse button has an independent deadman. No normal queue, result flood or diagnostics can displace safety handling.
- Local takeover and lock prevent later stale callbacks or frames from restarting output. Unlock/reconnect never restore old tasks, capture or motion intent.
- Replay suppression cannot become fresh execution after cache eviction, reconnect, epoch replacement or reboot. Do not claim exactly-once target delivery.
- `applied`, `sent` and Mac `observed` retain their stated evidence boundaries. An absent or failed capability stays unavailable in API, browser and MCP.

Property-based state-machine tests should generate interleavings of accept/send/release, clock advance, local takeover, lock, reconnect, target replacement and stale callbacks. Seed failing traces and minimize them into meaningful regression cases. Fuzz the real bounded parser separately. These techniques exercise production paths rather than compare a copied implementation with itself.

## Load and hardware qualification

Before remote readiness is advertised, measure the exact firmware with two clients, one TLS handshake at the cap, maximum legal pointer traffic, a text job, LVGL updates and repeated local takeovers. Repeat with Wi-Fi interference and Bluetooth host sleep/reconnect. When audio becomes implemented, test independent audio first, then same-host HID+HFP and HID+A2DP separately; different-host audio is a separate later qualification.

Record input/neutral-report scheduling latency distributions, watchdog overrun, queue high-water, minimum free internal heap, largest free block, task stack high-water, TLS memory and handshake time, radio disconnects, audio underruns and recovery outcomes. Proposed acceptance is no missed safety boundary, no leaked hold, no replay and no continuing resource growth. The 50 ms expiry scheduling allowance, queue/frame/client limits and coexistence readiness must be evidenced under worst tested load; reduce availability or fix the owning layer if they fail. Never relabel an overrun as a pass by changing only the test comparator.

Use [existing device stages](validation.md#hardware-qualification-stages) for durations and host records. A planned 30-minute stress run or 8-hour soak is not a completed result. Record power, board revision, exact OS/Bluetooth stack, firmware/BSP/SDK commits and enabled profiles. Direct browser HTTPS also needs actual certificate setup/renewal checks on supported browsers; Mac pinning success does not qualify direct Safari access.

## Evidence record and release gates

```text
Case IDs / date / tester:
Firmware commit / BSP commit / SDK commit / build configuration:
Board revision / power / host model and full OS / Bluetooth adapter:
Client/browser/bridge versions / TLS trust path / network conditions:
Initial boot ID, epoch, target, mode, owner and release state:
Stimulus and fault timing / expected admission, execution and observation:
Report trace / authoritative state trace / target observation:
Measured timing and resource limits / local evidence location:
Pass, fail, blocked or not run / precise reason / next action:
```

Do not store real secrets, private text, audio or screen recordings in this tracked template. Each test result states its evidence level and exact build. A later implementation or dependency change invalidates affected qualification until rechecked.

Implement no new interface until the complete function/configuration/audio/network/update plan and shared schema are reviewed. Release input only after R1-R3 safety/security/protocol cases pass on the intended target and outstanding failures have explicit capability restrictions. MCP release adds M01-M06; audio adds U07-U10 and combination stress; configuration/pairing adds the relevant U/A cases; update release adds W01-W03 and its separate security/recovery review. Disabled/unimplemented features do not need invented success records, but their unavailability behavior must pass. Documentation-only work runs repository hygiene, link and diff checks and makes no runtime qualification claim.
