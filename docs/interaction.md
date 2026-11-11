# UI, buttons and state transitions

See the [native UI renders](ui/README.md) for the workspace layouts.

## Primary navigation

On the first boot of an OTA image awaiting confirmation, the header shows
**Boot check / Browse only**. Menu, Status, mode selection, Settings pages
and Back respond immediately. Host input, media, radio changes and persistent
edits stay paused. Confirmation restores normal controls for a fresh gesture.
An already confirmed image boots normally. Active flash work and restart
maintenance retain priority over browsing. Lock and release remain available.

**Mouse**, **Keyboard** and **Audio** each use the 480 × 480 display for core functions. One consistent Menu button (at least 44 x 44 px) and AI double-click open the same quick panel with mode choices, actual input-target/connection summary, Status, Settings and Close. The UI and physical buttons use one authoritative state. The [native UI suite](../tests/ui/README.md) checks layout and hit targets.

Show the foreground mode and a compact selected-input/target cue in its context. Keep full target/link, battery and Wi-Fi details in the quick panel or Status. Reserve a small area outside touchpad/grid/slider targets for critical remote ownership, active capture/recording and input error/release state, with a real Stop/takeover/details action. Lock remains a full-screen safety state. Paired, HID-connected and audio-active must not collapse into one green indicator. Do not expose private credentials or unrelated management data in global status.

### Mouse

Provide a motion toggle, compact fine-gain chooser, much larger touchpad, separate scroll controls, clicks and explicit drag/release. No edge swipe or navigation gesture crosses the pad: pointer deltas must not be emitted before deciding to navigate. Any touch suppresses motion; release starts a short freeze. Menus and mode changes cancel drag. Mouse Motion uses rotational aiming through relative HID over the selected USB or Bluetooth link.

The right scroll column accepts a one-finger slide starting on either arrow or in the gap between them. Slide up for wheel up and down for wheel down. Distance determines the number of steps. Holding still or lifting after a slide adds no step. A tap on an arrow sends its usual single step, and a tap in the gap has no effect. Scrolling works over either input transport with either tap-to-click setting. The adjacent pointer pad keeps its own gestures.

Hold the device still in your intended aiming direction during **Calibrate
sensor**. This measures gyro bias and sets the aiming basis. Motion toggles and
touch pauses retain that basis and keep tracking valid samples. Resuming starts
with fresh relative increments and clears filter and movement fractions from
the paused interval. Temporary acceleration can disable gravity correction
while gyro tracking continues. **Motion / waiting** indicates that aiming needs
valid sensor evidence after a genuine sample loss or invalid sample.

The header **Recenter** captures the current aiming direction with Motion on
and a fresh, valid attitude. It preserves bias, settings and the Motion request
and clears pending model movement. The authenticated Wi-Fi input API uses the
same action with its existing authorization and input gates. In **Input**
settings, **Recenter aim** captures the same basis while Motion is off. It
leaves Motion off. Use **Calibrate sensor** to measure bias again. The
[orientation design](motion-estimation.md) describes recovery after missing
samples and the limits of gyro heading.

Input settings provides **Tap to click**, default on. The input recognizer owns single tap, double tap and double-tap-and-hold drag; the UI forwards real touch boundaries, displacement and bounded stationary-contact freshness. With the setting off, the pad moves only; explicit mouse buttons and scroll still work. The pad hint and Status report the actual selected provider, readiness, calibration and sensor conditions. Selecting USB does not by itself establish that the sensor is calibrated or that USB input is ready.

A single-finger press that starts on the pad keeps its relative pointer
movement across the entire screen until lift, including over the scroll column
and buttons. It retains an already held drag, and crossing the pad boundary
disqualifies a tap. Controls along the path receive no activation. A press
that starts elsewhere keeps its original target when it moves into the pad.
Multi-contact recognition and existing input cancellation rules still apply.

Preserve relative touch displacement and precision fractions across ordinary USB/BLE backpressure. Split large movement into the existing signed 8-bit reports, and merge only adjacent local movement without crossing click, scroll or button transitions. A contact beginning while busy may move once output resumes, but cannot grant a delayed tap or drag. Retained movement uses the existing 32-slot queue, a 4096-unit per-axis action bound and a 250 ms deadline from the oldest source sample; overflow or expiry cancels the pending epoch and prioritizes neutral releases. Cancellation, lock, mode/owner changes and target replacement discard residual movement. Remote touch uses the same displacement conversion and splitting, with explicit busy/capacity rejection before consuming a retryable request; lease revocation takes priority over retained movement.

#### Optional Navigation layout

**Input > Navigation** selects a persisted layout, off by default. On Mouse,
the original 132 x 48 Recenter header region becomes two independent controls:
Home at x 140/y 8/w 62/h 48 and Back at x 210/y 8/w 62/h 48. The touchpad,
wheel, click/drag row, Motion/gain row, Menu and critical controls keep their
geometry. Turning Navigation off restores the original Recenter header.

**Home** sends the existing third mouse button (bit 2, value 4), then its neutral
release. It requires host configuration; it does not claim that Home occurred.
Enable AssistiveTouch under Settings > Accessibility > Touch, then open
AssistiveTouch > Devices, select the pointer device and map Button 3 to Home.
The user can close the setup panel and press Home on Mouse while registering
the additional button. See [Apple's button-assignment instructions](https://support.apple.com/en-au/111775).

**Back** sends one Command+[ keyboard tap through the existing composite HID
connection while remaining in Mouse mode. It is independent of the draft's
Ctrl/Command preference, applies only to apps that support that shortcut, and
is neither system-wide Back nor a touchscreen swipe. Navigation never types
into or sends the local keyboard draft.

Both controls use local ownership, epoch/revision and selected USB/Bluetooth
admission. Active or queued drag, lock, release recovery, unavailable/busy input,
covered or sleeping UI, firmware maintenance and target/transport changes reject
or cancel the action. Failures use the existing neutral-release recovery without
replaying an accepted down report. Changing the header layout under a held
contact cancels that contact; a fresh press is required.

**Input > Recenter aim** provides the manual reference action when Navigation
occupies the header.

### Keyboard

First entry and every transition from Mouse or Audio into Keyboard select
**Touch** on the nine-cell grid, preserving the draft. Opening or closing Menu,
Settings, Keys or the entry chooser, and selecting the current Keyboard mode
again, preserve the current submode and draft. Motion remains an explicit entry
choice. Local and authorized remote mode selection use the same transition.

One compact chooser opens **Motion**, **Touch**, **Spoken letters** and **Shortcuts**; four persistent submode tabs are unnecessary. Motion and touch share a two-level 3 x 3 character grid, preview, case and symbol pages. Motion moves local focus only; empty cells never receive focus or confirm input.

A persistent **Keys** button in the Keyboard header opens the live-host panel in one tap from Motion, Touch, Spoken and Shortcuts. Its fifteen buttons form a symmetric five-row, three-column grid: Copy, Paste, Undo; Backspace, forward Delete, Enter; Esc, Tab, Select all; Home, Up, End; Left, Down, Right. The panel names the selected input host and shows the acknowledged Ctrl/Command shortcut setting. Its keys act on the host without changing the draft. Copy/Paste/Undo/Select all use Ctrl+C/V/Z/A for Windows or Command+C/V/Z/A for Apple hosts; Paste asks the host to paste its own clipboard and never reads clipboard contents. **Delete draft** removes a local preview character only; Send remains an explicit, separate action with no appended Enter. The existing chooser and six-button Shortcuts entry remain available, with no sticky modifiers. The nine-cell grid and preview controls keep their original geometry.

Flow: choose group -> choose character -> confirm into preview -> send. Keep delete, space and send visible. Sending never includes Enter; expose stop and accepted-prefix progress. An Alpha sample yields an A candidate, which needs confirmation before entering preview. Prefer spelling words for easily confused letter sounds.

Spoken letters and arbitrary dictation are distinct features.

### Microphone / speaker

Offer **Mic** and **Speaker** together, with independent start/stop and observed
state, a prominent microphone mute control and speaker volume. USB/Bluetooth is
the connection choice, not another functional mode. Keep Routes/Roles/Voice and
recording configuration out of the primary workspace. Bluetooth profile choice
belongs to the audio owner and must reflect actual capabilities; HID connection
does not prove an audio link. Speaker activation never enables the microphone.
Audio → Bluetooth offers **Pair audio** and **Close pairing** with local review,
the actual Classic device name (or unavailable), and bounded pairing status and
countdown. HID pairing remains separate; pairing completion does not start audio.
Independent audio mode produces no motion or character output. Background audio
becomes available in mouse/keyboard mode only after its concurrency capability is
qualified.

USB connection selection does not require an open host audio stream. Local Mic
or Speaker start becomes actionable only after the corresponding valid USB
stream identity, format and role readiness remain stable for 1,000 ms. An idle
role awaiting a host stream or stable negotiation shows **Off / Waiting** and
**Waiting for host audio** in the existing card. Use stable, role-specific host
instructions in the footer instead of alternating Unavailable/Preparing labels.
**Ready** and actionable Start require the owner's actual permission. Do not
flash an actionable Start button between transient stream openings. Loss or
replacement revokes readiness immediately. Keep Stop and Mute available under
their existing owner rules, retain terminal-job evidence, and never remember
or automatically replay a rejected Start request. The settling interval is a
named build-time policy; it does not delay transport invalidation or bypass
consent, privacy, cleanup or resource checks.

Audio operations show progress through their existing button and job states.
Accepted requests and successful completion do not produce a footer notice;
only failures show a reason. Success must remain silent after page navigation,
including a result received after leaving and returning. A new accepted request
clears the previous audio notice without erasing a newer status owned by another
subsystem. Errors remain available until superseded. Live volume and microphone
gain changes must finish between PCM transfers without starving behind a
continuous stream or silently restarting capture.

Keep the real-time Mic level inside the existing card, beside Mute, without
moving Start/Stop or shortening the two-line state explanation. Show RMS dBFS
and a logarithmic bar (-60 to 0 dBFS maps to 0 to 100 percent); a valid all-zero
sample window shows **Quiet**, while a full-scale peak shows **Too loud**.
Freshness is bounded to 300 ms and observation publication to 10 Hz. Show
**Waiting for signal** for an active capture with no fresh valid window,
**Level unavailable** for unknown/invalid/error observations, and **Mic off**
when capture is confirmed off or muted. Those states clear the bar and ignore
any retained value. Actual digital silence is distinct from missing evidence.
No raw audio is retained and this display does not certify host recording.
Meter changes must not alter control revisions, reject an already held control
or animate synthetic activity. Reuse persistent label/bar objects, update only
changed visible values without animation, and do no meter work on hidden pages.

## Physical controls

The official button factory currently exposes AI only. Its header also declares BOOT GPIO61, but runtime BOOT access is not a first-release dependency. POWER retains hardware power control.

| Action | Mouse | Keyboard | Audio |
| --- | --- | --- | --- |
| AI single-click | Toggle motion | Confirm selected group/character; on voice page start/stop listening or confirm candidate | Toggle microphone mute; speaker playback awaits AVRCP |
| AI double-click | Open shared quick panel | Open shared quick panel | Open shared quick panel |
| AI long press, at least 0.9 s | Stop and lock globally | Stop and lock globally | Stop and lock globally |
| Long press while locked | Unlock with motion off and microphone muted; never resume tasks | Same | Same |

Single and double clicks are mutually exclusive, with an initial 280 ms decision window subject to hardware tuning. Do not emit a single-click first and undo it later. Button down freezes motion. Menus, dialogs and lock overlays own focus; AI input must not activate hidden controls. Double-click can return a noncritical detail page to the quick panel, but cannot confirm an operation or unlock. Opening the quick panel cancels drag/queued input and suppresses motion through the coordinator; closing it preserves the mode and unsent draft with motion off.

During remote control, AI down immediately takes local ownership; consume that press. Continued holding may still lock. The first local touch similarly takes ownership without activating its underlying control.

Use short haptic feedback for mode changes, confirmations, locking and errors. Freeze IMU output during vibration; never vibrate every frame.

## Boundaries

| Event | Required behavior | Forbidden behavior |
| --- | --- | --- |
| Mouse -> keyboard | Release drag, reset motion remainder and local focus | Forward selection motion to the host |
| Primary mode change | Release HID, cancel queued text, increment epoch | Resend interrupted text automatically |
| Disconnect/target change | Cancel queue, mute, stop input, retain unsent draft | Replay old key-down after reconnect |
| Local takeover | Stop remote queue and release its keys | Resume the script automatically |
| Global lock | Pause producers, stop recognition and mute | Unlock through touch or a short click |
| Unlock | Preserve foreground mode, keep motion off and microphone muted | Restore old capture/motion/tasks |

## Usability

Settings are not a fourth primary mode; closing them returns to the previous mode. Hidden pages cannot redefine the AI key. Defaults are mouse, motion off, audio off and remote control unauthorized. Keep a bounded selected transport/target cue in mode context and full details one Menu action away, including before Send. Use high-contrast focus, light/dark appearance and at least 44 px touch targets, then verify readability on the actual 2.16-inch display. Any future handedness option must use a qualified setting rather than a hidden swipe.

## Appearance and product acceptance

Settings includes Light and Dark appearance. Dark uses a black background, near-white text and a light-blue accent for clearer selection and focus, with amber warnings and coral errors; Light retains the original green accent. Every page, focus indicator, disabled control and overlay follows the same theme. Appearance does not change input mode, target, draft or lock state. Dark is the default when no saved preference exists. Theme, brightness, idle timeout, haptics and the host shortcut modifier use validated persistent settings, with coalesced writes and visible save failures.

Keep the full touchpad and preview-first keyboard flows in their workspaces. See [validation](validation.md) for evidence and unresolved hardware issues.


## Status, connectivity and display

The shared quick panel and Status keep input-target/readiness separate from
actual battery, Wi-Fi signal and Bluetooth connection indicators. Primary pages
retain only core controls, compact target context and critical safety state.
Bluetooth shows the current peer and saved pairings rather than signal bars.
Detailed Wi-Fi values and network information belong in Connectivity; unknown
measurements do not become zero or empty battery indicators. Settings has Home,
Connectivity, Display, Input and About pages, entered from the shared panel.
Back returns to its parent; Close returns to the prior foreground mode.

Display provides Light/Dark, brightness in ten-percent steps, and Screen timeout:
Off, 1, 5, 10 or 30 minutes. Defaults are Dark, 50 percent and 5 minutes. Blanking
leaves radios and enabled mouse control running, while hidden keyboard focus
pauses. The first complete touch/AI gesture wakes only; the AI wake sequence also
consumes a rapid second click. A later fresh hold may unlock normally. Changes
are stored only after explicit edits, not on render/status polling.

Bluetooth actions open a local confirmation before changing pairing admission or
forgetting saved hosts. Pairing is time-limited and displays a countdown. Forget
waits for input cancellation, current Bluetooth host release recovery and
disconnection; changing the admitted host while confirmation is open invalidates
that confirmation. A physical link awaiting authentication and identity checks
has no input or user-host authority. Its retries do not invalidate local gestures
or an exact saved-host confirmation. After confirmed link-down, the next-host neutral-report
barrier remains armed but does not block pairing or saved-host management.
Pending releases on a separately selected USB transport do not gate Bluetooth
management. Connected Bluetooth congestion/release and transport-switch guards
still apply. These rules are the same in Mouse, Keyboard and Audio modes.

Manual **Disconnect** retains the pairing and pauses automatic return of the
observed former peer. **Resume** restores normal reconnect eligibility for the
most recently paused remaining host while no admitted host is connected. Other
hosts' pauses remain in place. If a different admitted host is connected, the
same button disconnects that host. An unexpected radio loss keeps the normal
reconnect policy. Reboot clears the temporary pauses.
If the host leaves before its pause is accepted, report that reconnect policy
is unchanged; do not promise a pause or ask the user to Resume.

**Select target** releases the old host's input and applies the saved target
policy before accepting its connection. Selecting the current, verified full
pairing preserves its connection after input release and saves the target
preference if needed. An already durable selection requires no new write;
selection alone never creates input readiness. For a private saved host that cannot be
identified at connection time, **Allow reconnect** gives that host 30 seconds to
connect from its system Bluetooth settings. It must authenticate and match the
saved pairing before receiving input. When the saved identity cannot be
verified, the action stays disabled and the current connection is preserved.
Select/Allow reconnect resumes only that saved host. Forget removes only that
host's pause after verified deletion. **Add host** opens a separate pairing
window for a new host and preserves other pauses. The previous host cannot claim
that window by reconnecting. The open window displays **Pairing open** and its
countdown through provisional retries; visibility and authentication remain
separate observations and never imply input readiness.

Connectivity selects Bluetooth or USB for all primary modes and opens separate
Bluetooth-host and Wi-Fi detail pages. Selection cancels pending input, waits
for old-host release completion, and starts a fresh release barrier on the new
link. An already disconnected old host permits explicit switching, without
claiming its releases arrived. A connected but suspended/stalled host must
recover or disconnect; the switch otherwise stops after five seconds and keeps
the old selection. USB removal never redirects input automatically to Bluetooth.
Preserve the preview, mode and lock; motion stays off after a switch.

Wi-Fi normally scans nearby networks on the device, shows four records per page,
and accepts a local password before Join once or Remember and join. The computer
stays on its existing Wi-Fi. LAN association/address acquisition, saved-record
status and actual Web/API availability remain separate; a LAN IP is not evidence
of a running backend. The temporary protected hotspot remains an explicit
Recovery hotspot option. Its passcode and local browser URL appear only on the
setup page; leaving, Stop, expiry or locking closes it. Forget still requires
local confirmation. Failed setup never enables remote-control/MCP authority.

Settings > Device name edits the shared base and previews the full mouse/keyboard and audio search names. The Bluetooth device options link opens the same settings page.
Name and Wi-Fi password entry share a local-only editor with separate bounded
buffers and purpose/session identity. Passwords are masked, never prefilled and
never sent into the HID preview, ordinary intents, labels or diagnostics. Only
explicit Apply/Join submits a private copy; acceptance is not manager completion.
Close, Back, AI double, lock, screen-off, session replacement and the five-minute
deadline clear local drafts. AI single cannot activate the hidden HID grid.
See [Wi-Fi provisioning](wifi-provisioning.md) for service limits and states.

### Browser controller permissions

In Settings → Wi-Fi → Web/API, open the displayed station HTTPS address on
the controller and request pairing. On the device, open **Wi-Fi controllers**,
choose the request and compare its full credential fingerprint with the browser.
A new approval starts with Keyboard & mouse for six hours. Management and
Updates are independent choices. The term can also be 24 hours or until revoked.
Review the device and the selected permissions before approving or saving.
Changing an imported USB identity replaces its previous scopes, including any
capture grants. Revoke removes that controller's access.

The list identifies the controller separately from the HID target and shows
requests, saved permissions, current input control, expiry and revocation.
Timed approvals require trusted time. Saving permission does not take control
or resume an old session. The Legacy API input switch applies to unconverted
USB-imported identities. It does not grant or revoke a browser permission.
The page reports the result of the matching request
after the access service acknowledges it. Local takeover and USB trust import
remain available.

## Runtime and revision safety

Keep the three primary mode pages and safety/navigation shell fixed, with at most
one dynamically created panel or Settings category after a safe, coordinator-accepted
transition. Retain a stable wake/lock surface. Do not delete
a pressed object or binding inside its event callback. Preserve whole-gesture
wake consumption, captured epoch/revision, local takeover and delayed-action
rejection during any page replacement. Exact lifecycle changes require review
and regression evidence before implementation.

Stable snapshots and hidden-page changes must not allocate or invalidate the
foreground. Use shared styles and value/revision-driven visible updates; rendering
never writes preferences. The current TE_SYNC path uses FULL rendering, so
validation must count actual flushes in FULL mode: after settling, unchanged
snapshots and hidden-only updates produce zero additional flush callbacks.
Measure object/allocation counts before and after; do not claim an arbitrary FPS
improvement or partial refresh. The current display-memory fault remains a
separate board/configuration repair, not justification for changing touch policy.

## Shared Bluetooth devices

System Bluetooth settings and Audio Manage devices enter the same purpose-scoped manager. Mouse & Keyboard and Audio retain separate saved records and search names. Add device enters the selected owner's local pairing flow; it never starts audio, changes input transport or sends a draft. Exact record actions preserve owner revisions and host generations. Audio connection requests report actual service state; Audio Forget reviews the exact inactive Classic record, including the last one, and reports removal after the audio list refreshes. Input pairing remains independent. Controller key stores and multi-block Bluetooth storage are unavailable for this operation. See [Bluetooth peer observations](bluetooth-peer-snapshots.md) for the service contract.
