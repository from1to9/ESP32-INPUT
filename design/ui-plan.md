# Focused mode workspace and unified quick panel

Status: authorized focused-layout implementation, 2026-10-21; native and independent integration review in progress. The latest user instruction explicitly replaces the previous fixed bottom tabs and persistent target/telemetry header with core-only mode workspaces and an on-demand shared panel. It preserves Mouse, Keyboard and Audio workflows, the real nine-cell character model and all input safety semantics. This is an authorized design revision, not a layout downgrade to disguise the current display-memory fault.

Open the [interactive focused preview](compact.html). The [earlier prototype](index.html) remains linked for workflow history. [Requirements](../docs/requirements.md) and the revised [interaction contract](../docs/interaction.md) govern behavior. Browser fixtures use synthetic state and neither send HID nor request audio/network/device access. They do not establish native rendering, allocation, hardware capabilities or real gesture timing.

## 1. Shared frame and navigation

Use a 480 x 480 logical canvas, 12 px side margins, minimum 8 px gaps, 12 px control radii, 16 px body text, 14 px metadata and 22 px mode titles. All actionable hit areas are at least 44 x 44. Main controls finish at y 472, leaving an 8 px bottom inset. No permanent bottom navigation or general telemetry strip remains.

| Shared element | Geometry | Rule |
| --- | --- | --- |
| Mode title | x 12/y 8/w 120/h 24 | Mouse, Keyboard or Audio; not a navigation target |
| Current input context | x 12/y 36/w 120/h 18 | Ellipsized BT/USB plus actual safe target label, or explicit unknown/disconnected; full identity in the quick panel |
| Compact entry/kind selector | x 140/y 8/h 48; w 132 for Mouse/Audio, w 76 for Keyboard | Mouse Recenter, audio connection, or keyboard submode; Keyboard adds Keys at x 224/w 48 with the same 16 px font and baseline |
| Critical-state action | x 280/y 8/w 116/h 48 | Visible only for remote ownership, capture/recording, input errors or readiness blockers; bounded text/icon plus safe Stop/takeover/details action |
| Menu launcher | x 404/y 8/w 64/h 48 | Always in the same position; opens the shared quick panel |
| Core workspace | x 12/y 64/w 456/h 408 | Mode functions only; coordinates below include this area's controls |

The current input cue stays directly under the mode title; supplementary context can appear in the mouse pad's noninteractive caption, keyboard preview metadata and audio profile description. Show the real selected transport and a bounded safe target label, or Unknown/Disconnected, without restoring a large header. Full identity, gauge percentage, Wi-Fi values and settings belong in the quick panel or Status. Keep the intended target discoverable before Send; do not display passwords, private draft text, addresses or unrelated management detail in global alerts.

Critical state never floats over a touchpad, grid cell or slider. The reserved action area is outside every core input target. Capture/recording uses a visible Stop action; remote ownership uses Take over/Stop. If several critical conditions coexist, use compact icons/short labels in the same area and route Stop through all relevant safety cancellations, with remaining conditions available in Status. Never silently hide active capture behind a lower-priority notice. Lock is a full-screen safety state, not an ordinary dismissible panel. A single error chip opens real error/recovery detail; it cannot imply recovery merely because it was tapped.

Opening Menu or the same panel through AI double-click suppresses motion, cancels drag/queued input through the coordinator and preserves unsent preview. No edge-swipe, hidden gesture zone or pad navigation is used: pointer deltas must not escape before a navigation gesture is recognized. Closing the panel returns to the prior mode with motion off. Ordinary touch navigation cannot unlock.

## 2. Mouse geometry

| Element | Geometry | Behavior |
| --- | --- | --- |
| Relative touchpad | x 12/y 64/w 396/h 292 | Fine pointer movement; entire surface remains an input area |
| Separate scroll column | x 416/y 64/w 52/h 292 | Tap either 52 x 142 arrow for one step. Slide one finger up or down across the full column for vertical wheel input, including the 8 px gap between arrows. |
| Left click / Right click / Drag or Release | x 12/x 168/x 324, y 368, w 144/h 48 | Explicit buttons; active Release remains admitted independently of ordinary ready state |
| Motion toggle / Fine gain chooser | x 12/x 244, y 424, w 224/h 48 | Motion Off/On and the current approved gain; choose among actual supported gains |

The touchpad grows from 384 x 156 = 59, 904 px to 396 x 292 = 115, 632 px: **93% more touch area**. This is geometric area, not a claim about pointer precision or latency. A tiny local marker/caption is noninteractive and does not shrink hit testing. Gain remains easy to change; a compact chooser or deliberate cycle displays the selected value and never changes on passive rendering.

Touch start and physical AI down suppress motion. Existing click freeze, relative deltas, fractional carry, congestion handling and release recovery stay coordinator-owned. Disabled input needs an actual reason in the critical-state area. The browser's synthetic ready fixture is explicitly distinct from an offline production boot.

The Mouse header places `Recenter` at x 140/y 8/w 132/h 48. The existing
noninteractive pad caption shows the coordinator's aiming-reference state.
See the [Mouse interaction contract](../docs/interaction.md#mouse) for
calibration, readiness, recovery and Recenter behavior. The browser preview
illustrates the geometry and includes earlier Motion copy. This layout uses
the 106-object core, the 160-object dynamic ceiling and the existing pad,
scroll, click and bottom-row rectangles.

### Optional Navigation revision

The user-approved Navigation preference is off by default and persists through
the settings owner. When enabled, Mouse replaces only the Recenter slot with
Home (140, 8, 62, 48) and Back (210, 8, 62, 48), separated by 8 px. All other
Mouse and safety geometry stays fixed. The ordinary layout retains Recenter.
Input places `Calibrate sensor` and `Recenter aim` side by side, with separate
Tap-to-click, Motion settings/Gesture help and Navigation rows; Back and save
status remain at their existing positions. The current Motion reason remains
visible below the haptics text. `Recenter aim` captures the current direction
while Motion is off in the menu.

The Navigation page explains AssistiveTouch Button 3 to Home setup, including
returning to Mouse to register the button. It labels Back as Command+[ for apps
that support it, without promising system-wide Back or touchscreen gestures.
The two independent header buttons add four fixed LVGL objects to the current
114-object baseline, for a **118-object core**. The dynamic panel ceiling remains
160, with native resource checks covering layout changes and teardown. Held contacts are
cancelled before revealing replacement header actions. The
[interaction contract](../docs/interaction.md#optional-navigation-layout) owns
admission and transport semantics; `compact.html` models the same opt-in layout.

## 3. Keyboard geometry and workflow

| Element | Geometry | Behavior |
| --- | --- | --- |
| Compact submode chooser | x 140/y 8/w 76/h 48 | Opens Motion, Touch, Spoken letters and Shortcuts choices; 16 px captions Motion / Touch / Spoken / Hotkeys; chooser retains full names |
| Keys | x 224/y 8/w 48/h 48 | One-tap access to live-host keys and Copy/Paste/Undo in every Keyboard submode |
| Preview | x 12/y 64/w 456/h 64 | Bounded preview, count/target metadata and Clear |
| Clear | x 372/y 72/w 88/h 48 | Disabled while sending or empty; no appended host action |
| Nine character cells | x 12/x 132/x 252; y 140/y 224/y 308; each 112 x 76 | Exactly three columns by three rows; fixed row-major engine indices |
| Groups / case / symbols rail | x 372; matching three rows; w 96/h 76 | Separate from character cells; no loss of nine-cell capacity |
| Send/progress metadata | x 12/y 392/w 456/h 20 | Accepted prefix and explicit no-Enter semantics; bounded text |
| Delete / Space / Send or Stop | x 12/x 168/x 324, y 424, w 144/h 48 | Visible for draft-based entry; Stop replaces Send during a send |

The real groups remain ABCD / EFGH / IJKL / MNOP / QRST / UVWX / YZ 01 / 2345 / 6789. A second-stage group has four valid characters in the engine's row-major cells; empty cells are hidden/non-actionable and excluded from motion focus. The UI must not invent a second character mapping or reorder indices for visual symmetry. Group -> character -> preview -> explicit Send remains unchanged. Sending never appends Enter and never replays the accepted prefix after interruption.

The chooser is an on-demand panel with five 456 x 64 rows at y 64/y 136/y 208/y 280/y 352, with 8 px gaps and Close at the shared top-right position. The first four rows select Motion, Touch, Spoken letters and Shortcuts; the fifth opens Common keys. Motion and Touch use the same grid. Shortcuts uses six 144 x 116 controls at x 12/x 168/x 324 and y 140/y 268, including explicit Enter. Spoken letters preserves preview/actions but displays its real unavailable state until qualified; no sample word masquerades as recognition.

The 2026-10-22 user-requested discoverability revision adds the persistent Keys entry without moving or shrinking the grid, preview, Clear or Delete draft/Space/Send controls. It reuses the Common keys overlay with fourteen controls in a three-column, five-row layout; Copy/Paste/Undo share the panel with the eleven explicit host keys. The capture cue and Menu retain their original header rectangles. The additional button and caption raise the fixed-core budget by exactly two objects (104 to 106); the dynamic-panel ceiling remains 160. See [Common keys](native-wifi-ui.md#11-common-keys-without-shrinking-the-character-grid) for layout and action semantics.

The 2026-10-23 symmetry revision fills the remaining cell with Select all, completing fifteen controls in five rows of three. It follows the acknowledged Ctrl/Command setting, preserves the host draft distinction and leaves the original fourteen hit rectangles unchanged. The fixed core and dynamic-panel ceiling remain 106 and 160 objects.

Each cell grows from 112 x 44 to 112 x 76, with 44 px minimum targets retained everywhere. The local/remote draft revision, focus, accepted prefix, consent and send cancellation remain shared state. An AI single on a hidden keyboard page does not confirm a character.

## 4. Audio geometry

| Element | Geometry | Behavior |
| --- | --- | --- |
| USB / Bluetooth connection entry | x 140/y 8/w 132/h 48 | Owner-observed connection, opening the two-choice connection page |
| Mic card | x 12/y 64/w 456/h 168 | Independent observed state, 136 x 48 start/stop target and prominent microphone mute |
| Speaker card | x 12/y 244/w 456/h 168 | Independent observed state, 136 x 48 start/stop target and speaker volume |
| Capability / privacy note | x 12/y 424/w 456/h 48 | At most two lines; no fabricated stream state |

Audio remains off/muted and unavailable where the actual driver/profile is absent. A future recording indicator is not evidence of recording support; the prototype's explicit fixture only exercises its visual Stop affordance. Supported capture must remain visible on the device and stoppable locally, including in another foreground mode. Speaker playback does not imply microphone upload. POWER and physical lock behavior are unchanged.

This section is the explicitly authorized 2026-10-23 Audio simplification.
Mic and Speaker appear together; Routes, Roles, Voice and recording setup are
absent from primary Audio navigation. The owner chooses Bluetooth profiles and
performs acknowledged transitions. Mouse and Keyboard geometry remain unchanged.
The two independent cards add six fixed LVGL objects to the previous 106-object
core, giving an expected 112-object core. The 160-object panel ceiling remains;
native resource tests must verify the actual object delta, idle allocation/flush
behavior and complete teardown on the combined implementation.

## 5. Shared quick panel, Status and Settings

Menu and AI double-click open the same coordinator-owned quick panel. It covers the workspace completely with an opaque themed background; no hidden mode control remains interactive. The launcher becomes Close in the same 64 x 48 position.

| Quick-panel element | Geometry |
| --- | --- |
| Title | x 12/y 12/w 256/h 32 |
| Input target/connection summary | x 12/y 64/w 456/h 104 |
| Mouse / Keyboard / Audio cards | x 12/x 168/x 324, y 180, w 144/h 104 |
| Status / Settings | x 12/x 244, y 296, w 224/h 64 |
| Lock | x 12/y 372/w 456/h 64 |
| Optional help line | x 12/y 448/w 456/h 24 |

The summary reports selected input transport, actual target/readiness/owner and separate Wi-Fi management state. Unavailable/unknown remains explicit. It does not combine battery, Bluetooth, Wi-Fi, remote access and audio into one Connected badge. Select a mode once, close the panel, release/cancel prior output and preserve unsent draft. Re-selecting the existing mode does not fabricate a new send.

Status and Settings use the same single content slot, title/Close, content at x 12/y 64/w 456/h 344, and Back to quick panel at x 12/y 424/w 456/h 48. Use bounded/paged rows rather than compressed text. Status covers actual input target, readiness/error/release state, ownership, battery/Wi-Fi and audio/capture state. Settings keeps Connectivity, Display, Input and About plus future capability-gated pages. Detailed forms follow their shared setting metadata and existing action/confirmation boundaries; they never write on read/render.

The About page retains one content panel. Six 24 px identity rows show Firmware, Build artifact ID, Board, Device, Developer and Git commit; captions start at x 12, values at x 148, from y 64. Build artifact ID is independent of the Git commit. A valid 40-digit hexadecimal Git commit displays its first 12 digits plus Clean, Modified or Unknown. Developer, repository and Git metadata are bounded coordinator-provided copies, never UI defaults. Missing, unterminated or unsupported/malformed values show Unknown or Unavailable; a malformed commit never inherits a Clean claim.

Repository uses a full-width, 448 x 68 wrapping label at x 12/y 232, with its caption at y 212. The canonical repository URL is readable in full; valid bounded long paths also wrap without ellipsis. Developer metadata supports 1-31 printable ASCII bytes without outer spaces. Repository metadata supports 1-95 printable ASCII bytes: literal `https://`, a dotted DNS hostname with 1-63 byte alphanumeric/hyphen labels and no outer hyphens, an optional 1-5 digit port from 1 to 65535, then `/` and a nonempty path. Path characters are RFC 3986 pchar plus `/`, with complete hexadecimal percent escapes. Credentials, query/fragment delimiters, whitespace, IPv6 literals, single-label hosts, trailing hostname dots and root-only paths are unsupported. Build validation and About display enforce this same contract. Battery and USB-power status remain at y 312 in a 448 x 40 region, uptime at y 360, settings-save status at y 400, and Back at x 12/y 424/w 456/h 48. Close retains its shared header position. Opening About creates only its bounded dynamic labels; hidden metadata updates must not allocate, flush or rebuild the foreground. Mouse, Keyboard and Audio geometry and controls remain unchanged.

Connectivity continues to select Bluetooth/USB for input while Wi-Fi remains independent. The [next-phase connection design](connection-management.md) is entered through Quick panel -> Settings or Status and must reuse this shared shell. Temporary setup secrets remain restricted to their explicit local setup page and are cleared on dismissal, expiry, lock and screen-off. Setup, confirmations and privileged management do not become general navigation shortcuts that bypass authorization.

## 6. Physical AI mapping

| Gesture | Mapping |
| --- | --- |
| Single in Mouse | Toggle motion when admitted |
| Single in Keyboard | Confirm the selected group/character; qualified Spoken behavior stays contextual |
| Single in Audio | Toggle microphone mute when supported; speaker playback control remains unavailable until qualified |
| Double in any ordinary mode | Open the same quick panel as Menu; when a noncritical panel is open, return to the quick panel without activating its hidden mode |
| Hold at least 0.9 s | Global stop/lock; a fresh physical hold while locked unlocks with motion/capture off |
| First whole gesture while screen-off | Wake only, including a hold or rapid AI double sequence |

The existing 280 ms single/double exclusion window remains subject to hardware qualification. Do not emit single first and undo it on double. Modal confirmations retain ownership; double-click cannot approve a pending operation or unlock. Physical AI down and the first local touch take over from a remote owner, consuming that gesture; continued AI hold may still lock. POWER retains hardware semantics. Do not assume BOOT is a runtime input.

## 7. Runtime lifecycle and allocation plan

The current production construction was measured at **315 objects, 84 action bindings and 663 color bindings**. The linked UI's static internal SRAM footprint is 19, 016 bytes, including 14, 336 bytes for the fixed color registry. These are baseline source/native object and target-map observations, not the dynamic target heap total. Exact run artifacts stay in `.agent/local/`; they must not be confused with a performance result for this proposal.

Keep the three primary pages and safety/navigation shell fixed, with at most one dynamically constructed panel or Settings category. This avoids destroying a primary input surface during a gesture while removing all eagerly constructed hidden settings and overlays. Domain state, drafts, mode, confirmation identity and settings remain in the coordinator. The focused-layout baseline progressed from 102 objects through speech Reject (104), Keys (106), the two Audio cards (112) and Mic level (114). Navigation adds four fixed objects, making the current architectural ceilings **118 with no panel** and **160 with one panel**. Native panel-peak and repeated-navigation measurements must confirm these ceilings before acceptance. Required controls are not removed to meet an obsolete proposal.

Object lifetime changes require a dedicated reviewed lifecycle, not a touch workaround:

1. A navigation intent captures the existing gesture, epoch and UI revision; only the coordinator can accept the transition and cancel/release affected output.
2. During the current LVGL input dispatch, callbacks only enqueue intent. Never delete a pressed object, its binding or its page from that callback.
3. Apply accepted page replacement under the LVGL lock at a safe post-dispatch boundary. Wait for pointer release/cancel or explicitly end the old gesture through the existing coordinator contract before destroying its page. Reject its delayed callbacks by revision; a held finger cannot press the new page.
4. Keep a stable top-level safety/wake surface and generation-scoped binding storage. No pointer to a deleted binding survives. A remote/physical mode change during touch must cancel the old gesture before rebinding.
5. Allocate bounded page/binding resources on deliberate transitions, release them on close and prove repeated mode/panel changes do not accumulate memory. Allocation failures remain failures under the LVGL allocator/assertion contract; this change does not claim a separately qualified graceful out-of-memory UI. Target memory and recovery qualification remain necessary.

Use semantic color tokens for surface, text, selection, focus, disabled, warning and error roles. The bounded color registry is reduced from 896 to 512 entries; action bindings from 112 to 64. An unchanged color binding does not call LVGL style setters; theme changes restyle live objects while preserving gesture identity. Each label retains its exact source text because LVGL DOT mode mutates its display buffer. Source text is compared before changing the label, allocated only on construction or actual content growth, and freed with its owner. This preserves ellipsis while preventing repeated label allocation/invalidation.

Compare incoming state by relevant revision/value and update only visible affected widgets. Do not call formatted label setters, style/position/visibility setters, slider setters or page builders when their effective value is unchanged. Hidden About uptime, Bluetooth pages, Wi-Fi setup data and unrelated audio state must not allocate or invalidate the foreground. Keep required security cleanup and state ingestion independent of widget updates: hidden setup credentials still clear immediately. Use bounded stack buffers or cached strings for changed text. No per-tick allocation, rendering-triggered persistence, periodic flash heartbeat or per-frame haptic action is permitted.

Internal/DMA allocation and the current panel flush OOM are managed separately by the board/config owner. Moving suitable UI metadata to PSRAM may be evaluated through the owning allocation contract, with actual memory capability measurements. Do not shrink the pad/grid, disable checks, remove workflows or change touch admission to hide that failure.

## 8. Measurement and review gate

The pinned board's TE_SYNC adapter selects **FULL render mode**, so any invalidation can trigger a complete 480 x 480 transfer. This proposal makes no partial-dirty-rectangle bandwidth claim. Native tests must exercise both the existing hit-testing path and a target-equivalent FULL rendering fixture. Hardware testing remains necessary for SPI/DMA/TE scheduling and touch behavior.

Before/after measurements must use the same source/configuration assumptions and record LVGL version, target/render mode, object/binding/style count, allocation count/live bytes/peak, target internal free/largest block and PSRAM separately. Preserve the original 315/84/663 baseline as history; establish a repaired-hardware baseline before attributing target improvements to the UI revision.

| Check | Required evidence |
| --- | --- |
| Idle identical snapshot | After construction/layout/initial refresh and animations settle, replay identical snapshots for a fixed 10 s interval; **zero new flush callbacks** and zero UI allocation/free operations |
| Hidden-page changes | Change only hidden About uptime and unrelated settings/connection detail; zero foreground flushes or UI allocations unless a required visible critical state changes |
| Actual visible change | Only changed text/state is updated; in FULL mode count the actual full-frame flushes/bytes instead of claiming a partial transfer |
| Navigation/resource lifetime | Cycle all modes, quick panel, Status, Settings and confirmations; bounded live objects and no retained allocations, stale bindings, lost release or use-after-free |
| Geometry/interaction | Real LVGL 480 x 480 render in both themes; hit-test every target, nine cells/four valid characters, preview/send/cancel, separate scroll and no pad navigation |
| Safety | Whole wake gesture, remote takeover, physical hold lock, stale revision, press across panel change, unavailable audio, error/release state and credential cleanup |
| Device integration | Same repaired board/config; measure target heap/largest blocks, actual flush errors/lock waits and gesture delivery. Host/browser success does not establish this gate |

The geometry and context-cue adjustment have been reviewed and implementation is authorized. Browser visual inspection is unavailable in this run and is not claimed as evidence; actual native LVGL renders and hit tests are the acceptance evidence. The UI owner maintains `app_ui.c/h` and design documents, the coordinator owner maintains navigation admission, and the test owner maintains native regressions. Complete API/configuration planning still precedes new service/interface implementation. Review the final production change and exact validation evidence before delivery; no arbitrary FPS claim or unmeasured after-count is acceptable.

## 9. Theme and navigation implementation notes

Dark uses background `#080b10`, surface `#111a24`, text `#ecf4fb`, muted text `#b6c4d2`, light-blue accent `#8dd8ff`, focus `#244b63`, amber warning `#e7bd7c` and coral error `#ffb4ab`. Selected controls pair the blue fill with dark `#082b40` text; motion focus also has an explicit border. Light retains the original green palette. This follows the user request to make the dark-theme accent easier to distinguish; color alone does not encode availability or a failure.

The shared panel reuses `APP_OVERLAY_MODES`; Status reuses `APP_OVERLAY_TARGET`. `APP_OVERLAY_KEYBOARD` and `APP_OVERLAY_AUDIO` are appended without changing prior enum values. Menu, Status, Settings and Back use coordinator-admitted `APP_MENU`; chooser selections use the existing `APP_KEYBOARD_SUBMODE`/`APP_AUDIO_KIND` actions and close only after acknowledgement. Gain cycles the existing 1, 4 and 8 denominators. AI double closes an already open quick panel, returns other ordinary panels to it, and cannot dismiss/approve a privileged confirmation. Critical capture/readiness cues remain visible in panel headers as well as primary pages; current capture Stop explicitly locks and mutes through `APP_LOCK`.

The first FULL-render resource pass reproduced 100 flushes per 100 identical snapshots due to comparison against LVGL-mutated ellipsis text. After preserving original label text, all 12 combinations of three modes, two themes and identical/hidden-only changes measured zero new flushes, allocations, reallocations and frees over 100 snapshots / 10 simulated seconds. Object counts and live allocation sizes stayed stable. This is host evidence; the full independent navigation/resource suite and repaired target checks remain separate gates.

A source-including host observation pass measured 102 objects / 36 action bindings / 261 color bindings for the fixed core, with maxima of 142 objects, 46 action bindings and 338 color bindings across every panel and Settings category. All 15 panel/core samples had zero allocation, reallocation, free or FULL flush activity during 100 identical snapshots each. After warming bounded label capacities, 50 full panel cycles returned to exactly the same core object count, live block count and live bytes. LVGL teardown released every observed block. This uses the host CLIB allocator observer and ASan/UBSan, not target heap accounting. Independent native regressions remain the delivery gate.

Status detail lists connection error, pending input release and disconnection together when they coexist. The bounded primary alert may prioritize release or capture, but the detail page never replaces an actual transport fault with a generic waiting-for-host message.

A held control that becomes disabled enters LVGL wait-for-release before disabling: the actual pointer release produces one `PRESS_LOST` / `APP_TOUCH_CANCEL`, while the old object and binding remain alive. This addresses the pinned LVGL rule that ordinary `RELEASED` is suppressed for disabled objects; it does not synthesize an early release or permit stale clicks. An opaque panel or safety surface also suppresses every covered core presentation update, including the mode header, target cue and selectors. Snapshot ingestion and immediate credential clearing remain independent.
