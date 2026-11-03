# Native product UI checks

Run `python3 tests/ui/run.py` from the repository root after resolving the locked firmware dependencies. The runner finds CMake and Ninja in the project toolchain or system PATH. It builds the unchanged production `firmware/main/app_ui.c` against managed LVGL 9.5.0, with Montserrat 14 metadata, 16 body and 22 title fonts and the RGB565 software renderer. Firmware must enable those same font sizes. No browser reconstruction, image mock, hardware connection or device write is involved.

The CMake targets enable strict compiler diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer for production UI code and LVGL. LeakSanitizer is enabled where supported; Apple's bundled ASan does not support it. A separate allocator observer counts every live LVGL CLIB block on all supported hosts, including at teardown. Build output is retained in `.build/ui-host/build.log`.

The [UI layout contract](../../design/ui-plan.md) defines the 480 x 480 grid, colors, settings hierarchy and coordinator boundary. Dark uses a black background, light blue selection and near-white text; Light retains the original green palette. The actual objects are restyled in place when the coordinator acknowledges a theme change.

The pointer driver performs actual LVGL hit testing. Assertions cover:

- Exact focused-workspace geometry: three-by-three keyboard columns, utility rail, draft actions, enlarged mouse pad and separate scroll, compact context and fixed Menu entry. Every visible label/button stays inside its parent and screen; buttons retain minimum 44 px targets.
- Relative touchpad movement, the pad/scroll gap, drag/release, press-time revision capture, modal ownership, physical-only unlock and in-place theme changes during touch.
- Nine-cell group selection, four valid second-stage characters and inert empty cells, explicit Enter versus Send, Stop, 64 wide-character preview/counter separation, long status truncation and unavailable speech/audio controls.
- Busy transport draining between press/release, first-touch control delivery, and active Release/Stop while ordinary input is unavailable.
- Settings page intent admission; all timeout values; brightness endpoints and release-only submission; acknowledged state versus pending actions; defaults, saved, saving and persistence failure.
- Pairing count/capacity uses the provider limit; full or over-capacity and unknown count/capacity disable Add without eviction. Closing an already open pairing window remains possible.
- Pairing open/close/forget, disconnect and Wi-Fi forget confirmation intents. The 32-entry Bluetooth list covers current connection, first/next/last page, back navigation, empty and unavailable states without treating missing evidence as an empty list. Pressing a request cannot directly confirm it or invent an updated connection state.
- Wi-Fi cleanup retry and uncertain/corrupt saved-network messages stay visible, including from Connectivity. Forget remains available when the coordinator reports that cleanup of uncertain storage is useful.
- Protected setup starting/active/ended, bounded synthetic SSID/passcode displayed in full, and credential label clearing on close, expiry and lock. All setup strings are test-only data, not real credentials.
- Input transport selection from all three modes, independent Bluetooth/USB state, pending transitions, unavailable USB, selected-path errors, no optimistic selection or duplicate input, and no inferred audio capability.
- Screen-off black framebuffer, first whole touch consumed on wake, and a subsequent fresh touch delivering its action.
- Shared quick-panel mode/status/settings intent routing, Back through the coordinator, keyboard/audio chooser intents and the documented gain cycle. A tap cannot acknowledge its own requested state.
- Actual top-right confirmation Close emits cancellation rather than an inadmissible generic close. Held controls survive deferred panel replacement, preserve press-time identity and cannot activate the replacement page. Wake cannot open Menu.
- Setup credential cleanup checks live allocator-owned bytes, including cached string tails beyond their NUL terminator. Lock, screen-off, navigation and expiry during a held Stop setup press must clear secrets and produce exactly one matching END/CANCEL; expiry must not activate the disabled control.

The default runs `ui_visual`, `ui_resources` and `ui_presenter_integration`. New timestamped evidence lives under `.agent/local/ui-focused-workspace/`; `--output-dir` accepts a new or empty directory and refuses to overwrite evidence. `--suite visual`, `--suite resources`, `--suite keys`, `--suite motion` and `--suite about` and `--suite navigation` are focused runs, not complete acceptance. The Keys selection runs the existing common-key pointer, alignment and safety checks in both themes, with the same production UI and final allocation cleanup; it does not run or claim the complete legacy screenshot or resource suite. The runner checks source and executable hashes across execution, records command outcomes and PNG hashes, and fails if any of the 174 retained focused-workspace render names in [legacy-scenarios.json](legacy-scenarios.json) are missing. Native network/editor/common-key scenes supplement those cases. The synthetic peers and setup credentials are test-only data. The About selection runs bounded developer/repository/Git fixtures plus the existing settings checks in both themes. It verifies complete repository wrapping, distinct Git and artifact IDs, unknown/invalid/unterminated snapshots, unchanged Back/Close hit targets and passive metadata labels. The resource suite also exercises visible Git states and malformed metadata, and changes developer, repository and Git fields while About is hidden. The Motion selection runs the grip, Motion-reason/tap, settings, common-key header regression and congestion checks in both themes; the default full suite still includes all of these checks. Grip cases use real pointer hits for Recenter, stop while waiting, independent sensor calibration, revoked/stale/covered/wake/lock/remote admission, and unchanged Mouse geometry. The resource suite additionally holds all five reference states idle in each theme.

About metadata cases also check the shared build/display grammar: valid HTTPS DNS-host URLs with explicit port and percent-encoded paths remain fully readable; malformed hosts, unsupported ports, credentials, query/fragment delimiters, whitespace and invalid escapes cannot become displayable repository metadata. Developer values reject outer whitespace and unsupported characters while preserving internal spaces.

The [native setup extension](../../design/native-wifi-ui.md) adds pointer and image cases for four long/duplicate/escaped Wi-Fi rows, immutable scan identities across a held press, paging, cleanup and every scan phase, supported/unsupported/expired details, and current-versus-saved network and LAN-versus-service status. The service-ready screenshot is an explicit synthetic capability fixture, not a product service claim. The persistent Keys entry is checked in all four Keyboard submodes, including exact hit rectangles and full-width caption readability. The symmetric 5 x 3 panel checks all fifteen hit targets: eleven independent HID usages with zero modifiers plus Copy/Paste/Undo/Select all identities under both Ctrl/Command settings. Coordinator tests separately prove the actual modifier/down/up reports. Host Delete/Enter and shortcuts remain distinct from Delete draft/Send; disconnected, sending, release-pending, held-revision, wake and lock cases cannot invent an action. Motion reason/Recenter and tap preference checks retain coordinator-owned admission, storage errors and movement-only touch intent routing. Genuine stationary pointer samples exercise the minimum 50 ms zero-delta contact heartbeat; panel cancellation and later timer activity cannot invent further contact or a click. The coordinator remains responsible for timestamps and gesture recognition.

Private editor cases enter all 95 printable ASCII characters through actual two-level pointer hits, enforce 8..63-byte password and 1..18-byte name limits, exercise every private callback result, preserve UTF-8 name deletion, and reject held/expired/replaced/waking submissions. The ordinary intent capture permits only touch boundaries during editing; passwords never use its text payload or HID actions. Accepted/stale submission, Close, navigation, lock, sleep, deadline expiry and session/purpose replacement must clear actual drafts, including tails. The public HID draft stays unchanged.

`ui_private_probe.c` includes the unchanged production UI translation unit to make read-only observations of its static private buffers and public snapshot. It does not substitute algorithms, invoke private handlers, reset state or add production branches. A live-draft positive control prevents an allocator-only false pass: private passwords reside in static storage, while `ui_memory_contains` independently checks allocated label/cache bytes. Cleanup requires the whole private buffers to be zero; delete checks the bytes beyond the terminator. Temporary private-request clearing remains source-reviewed at its owning callback because reading expired stack storage would be invalid. Only synthetic test data is inspected; physical memory erasure remains outside host evidence.

Both fixtures use FULL rendering, RGB565, a 480 x 480 draw buffer, a 15 ms refresh period and 60 DPI, matching the relevant pinned firmware settings. The resource fixture advances the normal LVGL timer path for 100 snapshots over 10 simulated seconds after settling; it does not force a redraw to manufacture either activity or a pass. It counts actual flush callbacks/bytes, objects, timers, allocator calls, live blocks, requested live bytes and peak requested bytes. Visible-change and allocator positive controls ensure the observers work. It requires zero new flushes, allocations, reallocations and frees for settled identical state, hidden About/bond changes, screen-off updates and target changes behind Settings Home. All panels and Settings categories also receive identical-state checks in both themes.

The documented ceilings are 114 objects for the fixed core (the Mic level label and bar add two to the prior 112-object core) and 160 with one dynamic panel. The resource fixture warms bounded label capacities, then performs 1,000 panel open/close cycles per theme across the registered variants on the same UI instance, including native list/detail, both private editor purposes, Common keys and remote review. Each close must return to the same core objects, timers, live blocks and requested bytes with cleared private drafts; teardown must release every tracked block. Hidden Wi-Fi labels, Bluetooth-name metadata and audio connection/volume metadata join the existing hidden-only checks. Rendering and deferred-touch lifecycle checks remain separate so a safe final allocation total cannot hide a stale action.

`ui_memory.c` observes the CLIB allocator boundary and delegates allocation/reallocation/free to the unchanged managed LVGL backend. CMake renames only that backend's three entry points so the observer can interpose; production `app_ui.c` and LVGL decisions are unchanged. The observer uses bounded static bookkeeping and fails on overflow or unknown pointers. Counts include LVGL drawing work, exclude test output/framebuffers and allocator overhead, and do not measure target internal/DMA memory or PSRAM. No source-only product branches, replay corrections or success-returning device doubles are used.

Inspect the generated PNGs after changes. Passing assertions cannot establish visual quality, physical display readability, sensor accuracy, radio compatibility or device wake timing. These require separate integrated target/device checks. Earlier evidence in `.agent/local/ui-visual/`, `.agent/local/ui-refresh/` and `.agent/local/ui-nine-transport/` records previous layouts and does not validate the focused workspace. The runner leaves visual review marked pending until an independent inspection is recorded.

## Media and capture presentation

The native suite exercises owner-derived routes, role/mute acknowledgement,
volume/gain admission, immutable recording profiles and limits, list/object/job
identities, separate start/delete/discard review, playback stop and bounded
spoken-letter confirmation/rejection. Synthetic snapshots are UI evidence only.
The UI submits typed requests; it never starts a codec, storage or recognizer.
Press-time identities must survive asynchronous updates without retargeting.

`--suite audio` runs `ui_audio_core.inc`, retained media/speech/storage checks and shared
navigation checks in both themes. Actual pointer hits independently start/stop
Mic and Speaker, preserve immutable jobs, reject stale/wake/lock actions, enforce
safe speaker volume and keep unknown mute acknowledgement truthful. USB/Bluetooth
selection uses the owner operation and does not start capture or change HID.
Disabled Start renders the owner's per-role admission reason, distinguishes an
unopened USB host stream from unavailable hardware or link, and keeps active
jobs and errors authoritative. Longest reason labels and their two-role footer
are checked against actual font/layout bounds in both themes; unknown reason
codes stay unavailable and disabled hits cannot emit a start.
Unopened and settling USB host streams share the same waiting text and stable
role-specific setup guidance. Alternating those owner blockers for 100 snapshots
with and without retained completed-job notices per theme must produce zero
flushes, allocations, reallocations or frees, with unchanged live memory. Actual
errors retain their card/footer priority. Readiness enables Start only when the
owner permits it; disconnect remains distinct and a held Start cannot retain
revoked admission.
Both cards stay visible regardless of the legacy audio-kind setting. Recording
and spoken-letter checks remain separate from primary Audio navigation. Storage
tests assert that its entry is absent from primary Audio, then exercise the
retained owner-supplied panel and every existing preparation/privacy assertion.

`ui_audio_level.inc` covers the read-only Mic input meter within the existing
card. Its label and bar distinguish unavailable, off, waiting, live RMS dBFS,
real silence and clipping; invalid live values fail closed. Actual held
Start/Stop/Mute/Menu and volume gestures survive telemetry-only snapshots.
Nine cases per theme prove identical, hidden and inactive updates produce no
allocation or display flushes. One hundred changing live snapshots per theme
bound the pinned LVGL transient text-layout callback to 64 bytes per update,
with no text-buffer reallocation, stable objects/live bytes after layout, and no
animation. Normal draw-task allocation during changed-frame rendering is separate.
The focused run writes `audio-level-resources.json` for each theme and asserts
zero live allocations at teardown. These synthetic PCM levels prove UI behavior,
not microphone sensitivity or host recording success.

`ui_media.inc` covers both themes and all retained media panels. The fixed core has 114
objects; one dynamic panel remains bounded by 160. Identical owner snapshots
must have zero idle allocation and flushes. This requirement also covers expired
confirmations, so rendering an invalid review cannot oscillate label contents.


## Native connection management

`ui_connections.inc` exercises the local USB commissioning window, confirmed
HTTPS address, complete paginated identity/permission review and exact digest
approval. Nonterminated or visually oversized review text disables approval.
`ui_trust_review_fixture.c` composes the unchanged production connectivity helper
with actual LVGL, substituting only public cached owner getters and submission
APIs. Pointer navigation visits all 3/10/46/53 pages of minimal, ordinary,
parser-accepted maximal-count enrollment and public-capacity fixtures in both
themes. Each page's exact text is visible; approval stays disabled until the
last page and submits the original digest. Missing fields and a held approval
whose authority becomes invalid emit no approval. The 46-page fixture is the
public metadata of a 7,735-byte synthetic package; parser/crypto validation is
separate trust-owner evidence. No private enrollment material is retained.
Saved Wi-Fi actions retain immutable profile IDs; Bluetooth target actions retain
the complete 32-byte bond identity and both owner revisions. Held presses reject
removed rows and refreshed catalogues. Unknown/uncertain state cannot silently
become an empty successful list. These pages share the same 44-pixel minimum
hit targets and bounded dynamic panel as the original UI.

Each saved Wi-Fi row exposes Join, First and Forget. Forget emits only the typed
review request with the captured profile ID and catalogue revision; the helper
owns its separate confirmation. Pending/uncertain state and replaced or removed
rows reject stale actions. The Bluetooth audio connection page has separate Connect and Disconnect
buttons. The owner can complete an existing service link's audio channel or
disconnect it after a failed request; the UI does not equate that service link
with live Mic/Speaker readiness.


`ui_presenter_integration` composes production UI, presenter and display observer
with actual LVGL FULL rendering. Only physical panel/IO, clock and task/ISR locks
are seams. It verifies idle heartbeat without redraw, exact transfer receipts,
overwrite revocation with and without an intervening status poll, first complete
proof retained across a forced empty refresh before the first status read,
callback generation replacement and hidden-screen revocation. This is software
composition evidence, not panel scanout or optical readability proof.

## Remote action review and temporary settings

`ui_remote_review.inc` checks complete restart, preference-group, connection reset,
LE pairing, all-bond removal and exact single-bond removal proposals, including
the full retained-manifest digest and consequences where applicable.
All five preference groups use the same immutable identity contract. Missing,
nonterminated or visually overflowing details disable confirmation. Held touches
cannot approve a replaced identity, revision, expiry or consequence, and expiry
is enforced even without another owner render. Unknown clocks, locked screens
and remotely owned input cannot approve. The coordinator must independently
validate the queued request identity before deciding its approval record.
Temporary settings and Bluetooth names are visibly distinguished from saved
values; the UI cannot make runtime-only changes durable.

The Navigation selection renders both themes and exercises the two independent
62 x 48 Home/Back targets inside the original 132 x 48 Mouse header slot,
AssistiveTouch setup, Input Reset grip, acknowledged preference changes,
header replacement under held touches, disabled/stale admission and the
unchanged keyboard draft/header. It also runs existing Motion grip, common-key
and settings checks. These are native LVGL observations, not host mapping or
physical navigation proof.
