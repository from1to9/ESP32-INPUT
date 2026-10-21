# Original design conformance and acceptance

Status: acceptance checklist and integration review, 2026-10-21. **The diagnostic
Foundation UI and independent review v2 are not a delivery candidate for the
original product design.** This document does not approve the replacement
implementation as a completed device. Independent source, host and native LVGL
review of the replacement is complete with no unresolved code-review blocker.
Feature-level acceptance boxes remain open until the required device/target
evidence is recorded.

## Authority and baseline

The reference is initial commit
`21c30f1b7345875019ec26cbc7ce08b416cb0f69`, specifically its
`docs/requirements.md`, `docs/interaction.md`, `docs/architecture.md`,
`docs/validation.md`, and `design/{README.md,index.html,styles.css,app.js}`.
Inspect the historical files with `git show 21c30f1:<path>`; current-file links
are provided for navigation, not as substitutes for that historical baseline:
[requirements](requirements.md), [interaction](interaction.md),
[architecture](architecture.md), [validation](validation.md), and
[prototype](../design/README.md).

The original requirements define P0 input, P1 spoken letters/independent audio,
and P2 concurrency/remote extensions. A later diagnostic implementation does
not reduce P0 to sample text and directional buttons. Later accepted safety,
haptic-switch, board-portability and reliability requirements are additive;
they do not replace the original interaction design.

Gap observations refer to the diagnostic code reviewed in v2, whose complete
code inventory digest is
`de0f186358f8458994b4f3fd921b2fdf1f704e5e8117c2ba1e69386cfc4c0a8a`.
At the initial gap assessment, its `app_ui.c`, `main.c` and `board_mosaico.c`
still matched that snapshot. Those files have since been replaced or extended;
the gap column deliberately retains the rejected baseline for comparison.
The existing [review record](review.md) establishes
only its stated source/host checks, not original-design conformance.

The browser prototype is an interaction reference. Its pointer, connection,
target names, speech candidates, audio and send progress are simulations. A
matching screenshot, renamed button, mocked sensor, successful compile or
passing transport test cannot establish a working product feature. First-party
firmware remains in English under [engineering standards](engineering.md);
translation does not authorize changing the original structure or behavior.

## Evidence rules

Use these distinct labels in every acceptance result:

- **H:** production logic exercised by host tests, with time, sensor, button,
  transport and hardware failures substituted only at real boundaries.
- **V:** actual LVGL product screens rendered at 480 x 480 and exercised with
  touch/physical focus routing. A browser prototype image is reference only.
- **D:** the identified physical board runs the exact reviewed image, with
  sensor, touch, button, motor and scheduling observations as applicable.
- **T:** a named target model and OS show the expected pointer, key, shortcut or
  audio result. Submission and notification completion are not target outcome.

Record exact source/configuration hashes, SDK/BSP pins, stimulus, expected and
observed state/report traces, failures and evidence locations. Keep identifying
details and raw captures local under the [knowledge contract](../.agent/shared/KNOWLEDGE_CONTRACT.md).
Use synthetic text. Record unavailable, not run and failed separately. An
unavailable indication is honest failure handling, not a pass for a required
P0 capability. No feature inherits a pass from another feature or an older image.

## P0 functional acceptance

The gap column describes the rejected diagnostic baseline, not a prediction of
the work in progress. A partial implementation still leaves the row open.

| Check | Required behavior from the original design | Diagnostic baseline gap | Evidence required to close |
| --- | --- | --- | --- |
| [ ] P0-BOOT | Cold boot selects Mouse, motion off, audio off/muted and no authorized remote owner. Local mouse and keyboard work without Wi-Fi, a Mac or a proprietary target client. | Core defaults exist; usable local mouse/keyboard workflows are incomplete. | H startup/restart and stale-state tests; D standalone boot; T actual input without a helper application. |
| [ ] M01 | Live wrist rotation produces relative movement after stationary gyro-bias calibration. Use actual monotonic sample intervals, tunable axes/deadzone/filter/gain/click-freeze parameters, fractional carry and bounded deltas. Reject invalid/stale samples and never catch up a backlog. | Portable integration exists. Board only probes/initializes the IMU; no live stream, calibration workflow or coordinator motion producer. | H timing, bias, axes, invalid samples and freeze/reset transitions; D 60-second stationary test, multiple grips and slow/fast rotations; T observed pointer behavior. Record actual drift and tuning, not assumed zero drift. |
| [ ] M02 | A large single-finger relative touchpad provides 1x, quarter and eighth gain. Any touch suppresses IMU pointer output; release/cancel starts a short freeze and clears stale motion. Fine-control exit may restore the prior motion intent only in the same valid Mouse session. | Directional nudges and one-unit horizontal buttons replace the required touchpad and gains. | H equal paths at each gain with fractional carry, pointer identity/cancel and suppression; V touchpad/scroll separation; D actual touch release; T relative movement and precision. The initial 150 ms freeze is a tuning starting point, not measured latency. |
| [ ] M03 | Left/right click, explicit drag/release state and an independent scroll area work. Clicks end with button release. Drag survives intended pointer movement and is released on menu/mode/lock/disconnect/target boundaries. | Click and scroll actions exist, but no explicit drag workflow. | H complete button-state traces and interrupted drag; V visible drag/release feedback; D/T held drag, touch movement, scroll isolation and every interruption. |
| [ ] K01 | Mouse and keyboard use distinct Report IDs in one stable composite HID service. Switching modes requires no re-pairing or service restart. | Descriptor/transport host tests exist; real mode workflows and target compatibility are unqualified. | H descriptor and characteristic wiring; D discovery/security/readiness; T repeated mouse-keyboard switches and releases on each claimed host family. Keep paired, connected and input-ready distinct. |
| [ ] K02 | Motion and touch use the same two-level group -> character grid. Motion changes local focus only; AI single-click confirms. Relative angular integration, center reset after confirmation and boundary hysteresis prevent jitter or dwell from adding characters. | No selector, shared grid, focus model or physical single-click path. | H selection, hysteresis, center reset and mode/subpage boundaries; V focus highlight and group/back behavior; D real IMU and touch selection; T report trace proving selection emits no host pointer or character until explicit Send. |
| [ ] K03 | Users compose A-Z/a-z, digits, space and common ASCII punctuation; delete edits the draft, and clear empties it. Motion and touch share case, symbol pages and the same draft. | A fixed sample and direct host Backspace/Space replace editable composition. | H reachability/encoding of printable US ANSI characters and bounded edits; V shared draft/case/symbols; D composition through both input methods. Prototype capacity is 64 characters: retain that reference capacity or resolve a deliberate difference explicitly; a 32-action transport batch is not a justification for silent draft truncation. |
| [ ] K04 | Explicit Send uses the visible selected target and draft, emits key-down/key-up pairs, never appends Enter, shows progress and allows Stop. Preserve only the unsent suffix after interruption and show the accepted prefix without automatically replaying it. | Engine counters/cancellation exist, but no editable draft, send-session UI or prefix/suffix reconciliation. | H full/partial send, refusal before/after key-down, cancellation, mode/lock/disconnect/target change, overflow and repeated Send; V preview/progress/Stop; T controlled-focus text and modifier release. Reconcile progress after every boundary without losing or duplicating a character. |
| [ ] K05 | Copy/paste/undo use a visible target-system configuration for Ctrl versus Command; Tab/Escape/Enter are separate deliberate actions. Shortcuts do not send the draft. | Direct Tab/Enter exist; no host profile or configurable modifier shortcuts. | H both modifier families and release pairs; V profile/shortcut controls; T each configured OS mapping with English US ANSI input. A profile label must not pretend to identify the connected peer. |
| [ ] B01 | AI down freezes motion immediately. One click toggles Mouse motion or confirms the active Keyboard selection. A double-click opens the mode chooser with no preceding single-click effect. A hold locks globally; a later hold unlocks safely. | Only a 1200 ms long-press callback is registered; no down/up, short/double arbitration or modal dispatch. | H debounce, approximately 280 ms single/double decision window, hold threshold at least 0.9 s, boundary timing, bounce, held-button boot and no click after a consumed hold; D measured real-button behavior. Record chosen tuning; do not silently convert the physical button into lock-only behavior. |
| [ ] S01 | Lock cancels producers/queued text, releases all keyboard/mouse state, stops recognition/remote work and mutes available audio. Only a new physical hold unlocks; mode/draft remain, motion/audio stay off and tasks never resume automatically. | Physical-only lock and release barriers are host-tested; new producers, draft and overlays are absent. | H saturated-queue priority, simultaneous lock/hold, delayed callbacks and every producer; V blocking lock layer; D/T lock during held drag, modifier and partial text. Release retries remain bounded; an accepted release request is not proof of host release. |
| [ ] S02 | Disconnect or target replacement cancels old input, preserves only unsent draft, mutes/stops producers and keeps motion off. Reconnect never replays clicks, keys, text, motion or old recognition results. | Transport generations, bonded subscriptions and release barriers exist; product-level draft/selection/motion recovery is missing. | H replacement between coordinator polls and during submission, stale UI/sensor events and restored subscriptions; D/T real bond reconnect, sleep/wake and replacement. New input waits for encryption, subscriptions and release recovery. |
| [ ] H01 | Short haptics are limited to mode changes, confirmation, lock and errors. Freeze IMU output while a pulse is pending/active or off is unconfirmed; no per-frame vibration or queued pulse replay. The later global disable must cancel pending pulses. | Bounded policy and dispatch safety exist; there is no live IMU producer to prove the vibration gate. | H exact producer gating, second error pulse, disable/write races and failed off; D real pulse/stop and IMU behavior. Missing motor is unavailable; failed off remains conservative. |

The original validation plan also calls for a 100-character keyboard workload.
Exercise it across bounded drafts/batches if the preview remains 64 characters;
do not silently drop the tail or claim an unbounded queue. Distinguish a locally
queued character, transport-accepted prefix, completed release and observed
target text. Keep enough visible progress/context to explain an interrupted
send; never label mere submission as successful entry in the intended field.

## Visual and interaction acceptance at 480 x 480

The reference structure is `device-top`, `view`, `device-status`, `modeNav` and
`overlay` in the original HTML, with screen geometry and styling in the original
CSS. Its responsive browser shell and external simulator controls are not part
of the physical screen. Match the structure and interaction hierarchy; do not
substitute a diagnostic control panel or present a mock layout as working input.

| Check | Required product screen and interaction | Baseline gap and required proof |
| --- | --- | --- |
| [ ] UI01 | Persistent bottom navigation for Mouse, Keyboard and Audio; selected mode obvious. Top bar provides the actual HID target/status and Settings. Pairing, HID readiness and any future audio state remain separate. | Foundation places mode buttons near the top, omits top target/settings controls and uses diagnostic status/footer text. Capture actual LVGL screenshots at 480 x 480 for every mode and connection state; no invented iPad/host name. |
| [ ] UI02 | Mouse shows a motion toggle, 1x/quarter/eighth gain, dominant touchpad, separate scroll strip and left/drag/right controls with explicit release feedback. | Nudge buttons are not a touchpad. Exercise every visible control and compare emitted reports, not only pixels. |
| [ ] UI03 | Keyboard shows Motion/Touch/Spoken letters/Shortcuts subnavigation, shared preview, group/character grid, case/symbol/back controls and persistent Delete/Space/Send-or-Stop footer. | Sample-text buttons are not selection or a draft. Show both grid levels, focus, full/empty draft, sending, cancellation and target-unready states without overlap or clipped controls. |
| [ ] UI04 | Target, Settings and mode chooser are overlays, not extra foreground modes. Close returns to the original mode. Underlying controls are inert; only the active page/modal owns physical input. | No overlay focus model exists in the baseline. Test AI click/double/hold and touch through each overlay, hidden Keyboard/voice pages and stale callbacks after page replacement. Closing Settings alone must not change mode or resume motion. |
| [ ] UI05 | Lock covers all interactive content and explains physical-hold unlock. Current mode, disabled output and retained draft are truthful. Visual focus is high contrast; normal touch targets are about 44 px and text is readable on the actual 2.16-inch display. | Disabling mode buttons is not a complete blocking lock layer. Inspect the real screen at native size, including longest labels, alerts, left/right scroll placement if offered, and any advertised theme. Do not claim unimplemented appearance/handedness settings. |
| [ ] UI06 | Audio remains the third main page with the original Voice/meeting versus Speaker distinction, mute, volume and separate capability/link status. Spoken letters remain a Keyboard subpage. Unavailable capabilities have explicit reasons and disabled operational controls. | An unavailable label is honest but does not implement audio. Keep the intended navigation without fake playback, listening, candidates or link indicators; P1 rows remain open. |

## Cross-feature safety cases

These cases are required in addition to isolated feature tests. Check emitted
reports throughout the sequence; a correct final state cannot excuse an earlier
unintended report. Every case is currently **pending for the replacement build**.

| Case | Stimulus | Required outcome |
| --- | --- | --- |
| [ ] X01 | Motion on -> touch any control/pad -> drag across/outside -> release/cancel | No concurrent IMU pointer; correct relative touch behavior; release freeze; no accumulated movement burst. |
| [ ] X02 | Single, double and long press at timing boundaries while moving | Immediate down freeze; exactly one eligible action; double never first toggles motion/adds a character; hold never later emits a click; held press cannot both lock and unlock. |
| [ ] X03 | Keyboard subpage -> Mouse -> AI click; open Settings/target/mode chooser and click AI | Only current foreground/modal context handles input. Hidden voice/selector state cannot capture the event; underlying controls receive none. Hold remains a global safety action. |
| [ ] X04 | Drag or text send -> mode/menu/lock; delayed UI or sensor event arrives afterward | Release/cancel and generation invalidation precede new output. No stale draft confirmation, pointer movement or old button state survives. |
| [ ] X05 | Partial send -> transport failure/disconnect -> reconnect or different target -> Send | Show truthful accepted prefix and remaining suffix; no automatic resend. Explicit new Send admits only intended remaining content to the current target after release recovery. |
| [ ] X06 | Local AI down/touch while a remote owner is active, when that capability exists | Immediate takeover consumes the first action; no underlying click/character/toggle. Further hold may lock. Remote tasks stay stopped. An unavailable remote service must not be represented as an active tested feature. |
| [ ] X07 | Sensor stale/error, missed button edge, queue full, busy BLE or delayed UI | Fail closed without stale replay; preserve lock/release admission and bounded resource use. Invalid sensor evidence cannot become a zero-drift success. |
| [ ] X08 | Confirm character/mode/lock during motion; disable haptics during either error pulse | Correct short feedback only, motion suppressed for pending/active/uncertain stop, no delayed replay on re-enable. |
| [ ] X09 | Audio page or unavailable Spoken letters -> AI click, touch, lock and reconnect | No capture, playback, generated candidate or host input is fabricated. No Mouse motion or Keyboard output leaks from independent Audio mode. |

## P1 capability gaps remain explicit

These original requirements remain required for P1. They do not justify delaying
P0 input completion, and their unavailable UI is not a completion claim. P2
concurrency, multiple targets and arbitrary dictation must not be substituted
for the original P1 scope.

| Check | Original requirement | Current gap and acceptance evidence |
| --- | --- | --- |
| [ ] V01-V03 | Explicit short listening window -> Alpha-Zulu/optional digits candidate -> confirmation into the shared draft, never automatic Send. Prefer local ESP-SR mn7_en; aliases and rejection thresholds require measurements. | No real capture/recognizer. Preserve unavailable status until actual microphone frames drive recognition. Require H stale-session/cancel gating and D per-letter confusion/rejection results across speakers, distance/noise; a selectable Alpha sample is demonstration only. |
| [ ] A01 | Independent HFP HF microphone/meeting mode through the board codec, first with Windows/macOS capture, playback and mute. | No operational audio manager/profile. Require D/T actual profile negotiation, captured/playback audio and verified mute; pairing or codec initialization alone does not count. |
| [ ] A02 | Independent A2DP Sink/SBC speaker mode with suitable mono mixing and no microphone upload. | No implemented playback path. Require D/T streaming, audible output and absence of microphone capture/upload. Do not claim stereo speakers. |
| [ ] A03 | Local mute, volume, separate audio target/link and resource ownership. One manager owns codec/capture; defaults off/muted. | Controls must remain unavailable until they act on real owned resources. Require H ownership/state transitions and D/T audible volume and mute effects. AVRCP playback control remains separately unimplemented until evidenced. |
| [ ] A04 | Duplex requires AEC and acoustic validation; otherwise state the separated-output or half-duplex experimental limitation. | No acoustic proof. Record D/T echo results and permitted configuration before enabling duplex or claiming meeting usability. |

Audio off/unavailable must still leave P0 local input usable. Independent Audio
mode suppresses HID producers. Background audio in Mouse/Keyboard is unavailable
until the specific same-host profile combination is qualified; a UI checkbox
or successful build cannot enable a verified-capability flag. Recordings remain
short-lived memory by default, with no persistence/upload or always-on listening.

## Replacement implementation review

The replacement now implements the original navigation, relative touchpad,
two-level selection grid, shared 64-character draft, explicit Send/Stop,
Ctrl/Command shortcuts, physical gesture classification, live IMU adapter and
explicit drag state. Its presence does not close the D/T acceptance rows.
The user-requested dark theme uses a black background and light-blue accent;
the light theme retains the prototype's green palette. Theme selection is
session-only and does not recreate controls during a touch.

| Area | Current independent evidence | Remaining acceptance |
| --- | --- | --- |
| M01/M02/K02 | H production interaction/coordinator tests exercise calibration rejection, actual sample intervals, relative focus, fractional touch gains, stale samples and suppression. Board tests exercise the actual adapter with driver doubles, failure injection and 100,000 cached samples. IR05 is fixed and independently retested. | D/T axis direction, stationary drift, grip, gain, real sampling jitter and physical focus confirmation remain unverified. |
| M03/K01/K03/K04/K05 | H production HID/coordinator tests exercise drag/release, the original groups and printable mapping, 32-character transport chunks, shortcuts and exact prefix/suffix reconciliation. An independent probe passes all 129 report boundaries for each of five interruption types: 645 send/cancel/resume cases for a 64-character draft. | D/T actual host input, target focus, modifier mapping and reconnect remain open. Accepted reports do not establish delivered characters. |
| B01/S01/S02/H01/X01-X09 | H covers queue saturation, physical-only hold, lost edges, delayed contexts, touch completion, cancellation, local takeover, transport generations and haptic gates. The five independent behavioral findings below are fixed and retested. | Real button, motor, touch and radio timing remain D/T work; no remote service is claimed. |
| UI01-UI06 | V production `app_ui.c` renders through pinned LVGL 9.5.0 at 480 x 480 in both themes. Real LVGL hit tests exercise modal coverage, relative touch, stale press context, theme changes, explicit Enter/Stop, busy controls and unavailable speech/audio. Forty-two suite screenshots plus ten independent boundary screenshots were generated; representative screens and all repaired text boundaries were inspected at native resolution. IR06 is fixed. | Physical readability and touch calibration remain D. Voice/audio remain unavailable P1 capabilities. The bounded preview uses the prototype's one-line ellipsis while progress remains visible. |

The independent host run includes input core, HID engine, interaction, haptics,
descriptor, BLE adapter, coordinator, board adapter and device-smoke parser
checks with applicable address/undefined sanitizers, plus repository hygiene,
prototype JavaScript syntax and diff checks. The full host suite and independent
probes passed again after the behavioral fixes. Native LVGL checks passed after
the layout fixes. Code hashes were unchanged during the final runs. These
results do not qualify device behavior or target compatibility.

| Finding | Reproduced failure | Current disposition |
| --- | --- | --- |
| IR01, P1 | A short notification-congestion interval closed the interaction send ledger while the coordinator continued queued chunks. A 40-character draft produced 64 key-down reports and retained 39 characters. The same edge disabled model motion while the engine still showed it enabled. | Fixed: temporary readiness suppression no longer creates a canonical state boundary. Production short-congestion send and motion regressions pass. |
| IR02, P1 | Selecting the already active Keyboard tab settled the model session but left queued engine text running, so accepted progress and retained draft diverged. | Fixed: same-mode/invalid selections preserve an active send; changing modes still cancels. Production regression and exhaustive interruption/resume probe pass. |
| IR03, P1 | Raw-button overflow lost an UP edge, leaving the model down after the physical button was up. A later timeout could unlock without a new physical hold. | Fixed: overflow locks and discards the incomplete sequence, waits for observed UP and requires a fresh hold. Both final physical levels and a subsequent legitimate hold pass. |
| IR04, P1 | A queued double-click opened a modal using the later frame clock; a following queued DOWN then appeared to regress time and suppressed a genuine 900 ms hold. A callback newer than the frame timestamp had the same risk. | Fixed: handlers use each edge's clock, followed by the maximum frame/edge time. Production queued-double/hold regression and independent newer-than-frame edge probe pass. |
| IR05, P1 | A Keyboard cell selected at physical DOWN could change during the 130 ms between the 150 ms release freeze and 280 ms single-click decision. With the normal filter and 80 dps wrist movement, pressing on A confirmed B. | Fixed: keyboard focus remains frozen while click classification is pending. Independent production reproduction now confirms A with zero HID reports. Added regressions cover group/character selection, nonzero focus, the exact deadline, double-click, hold and clean motion resumption; the Mouse freeze remains 150 ms. |
| IR06, P2 | A full 64-character draft wrapped into the progress/count row because the LVGL preview had no bounded height. A long status message extended behind navigation; the unavailable-haptics label wrapped its final letter. | Fixed: preview/progress/status labels have explicit single-line height, and the haptic label has sufficient width. Both-theme native renders and geometry/hit assertions pass for maximum-width drafts, long status and unavailable settings. The underlying draft remains 64 characters. |

The final review inventory contains 87 first-party code/configuration/build/
test/prototype files and 29 documentation/license context files. The code
inventory SHA-256 is
`6c5a0b8e3d44e3adbbec964ccc1825fc89bbe09ebb3c342fafd044eadf394f0b`.
It hashes sorted UTF-8 lines containing each file's SHA-256, two spaces, its
repository-relative path and LF. Thirty-five code files changed or were added
since v2 and were reviewed with the integrated behavior; unchanged files were
verified against the earlier reviewed hashes.

Detailed manifests, reproduction programs, logs, native renders and original
failed captures remain local under the knowledge contract. The integrator's
post-fix target build completed with the pinned SDK/BSP; its log and image are
recorded separately from the reviewer's host execution. No unresolved source
blocker remains in this exact inventory. Any code change requires a new hash
comparison and appropriate review; this conclusion does not close D/T or
powered-reliability acceptance.

## Review and delivery gate

- [x] Owners finish the actual UI, portable interaction, board input and
  coordinator integration; record the resulting complete source inventory.
- [ ] Map each P0 and visual row to production code, meaningful host tests and
  feature-level device/target evidence. Keep absent evidence open; do not
  downgrade M01/K02 to unavailable and call P0 complete.
- [x] Independently review all new/changed producers, state transitions,
  queues, focus routing and adapters, together with shared HID/release semantics.
  The prior v2 approval cannot be inherited by the replacement implementation.
- [x] Run the applicable strict/sanitizer suites and pinned target build on
  the reviewed code. Verify native-size LVGL appearance and actual interaction;
  document untested target families rather than extrapolating compatibility.
- [ ] Record each claimed iPhone, iPad Pro 2018, Windows and macOS configuration
  separately, including English/US ANSI layout and required accessibility
  settings. Windows 10 remains conditional on an available host; missing access
  is not a pass. No claim covers unavailable hosts by inference.
- [ ] Preserve failed captures and earlier review records. Reconcile
  [coverage](test-coverage.md), [validation](validation.md) and [review](review.md)
  with the final image. Feature completion, powered reliability and publication
  remain distinct evidence levels; the later 24/72-hour gates are not replaced
  by a short smoke test or simulated cycles.

This assessment records source/host/native-render review and the original
acceptance criteria. It performs no device write. Device and target outcomes
must be recorded for the exact reviewed image before the corresponding product
acceptance rows can close.
