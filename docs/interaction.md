# UI, buttons and state transitions

## Primary navigation

The latest user instruction revises the 480 x 480 baseline: **Mouse**, **Keyboard** and **Audio** each use the display for core functions. Remove persistent bottom tabs and the bulky target/telemetry header. One consistent Menu button (at least 44 x 44 px) and AI double-click open the same quick panel with mode choices, actual input-target/connection summary, Status, Settings and Close. The UI and physical buttons use one authoritative state. The [focused prototype and geometry plan](../design/ui-plan.md) define the authorized layout; production delivery requires native rendering, safety and resource validation.

Show the foreground mode and a compact selected-input/target cue in its context. Keep full target/link, battery and Wi-Fi details in the quick panel or Status. Reserve a small area outside touchpad/grid/slider targets for critical remote ownership, active capture/recording and input error/release state, with a real Stop/takeover/details action. Lock remains a full-screen safety state. Paired, HID-connected and audio-active must not collapse into one green indicator. Do not expose private credentials or unrelated management data in global status.

### Mouse

Provide a motion toggle, compact fine-gain chooser, much larger touchpad, separate scroll controls, clicks and explicit drag/release. No edge swipe or navigation gesture crosses the pad: pointer deltas must not be emitted before deciding to navigate. Any touch suppresses motion; release starts a short freeze. Menus and mode changes cancel drag. Motion follows wrist rotation, not absolute device tilt.

Input settings provides **Tap to click**, default on. The input recognizer owns single tap, double tap and double-tap-and-hold drag; the UI forwards real touch boundaries, displacement and bounded stationary-contact freshness. With the setting off, the pad moves only; explicit mouse buttons and scroll still work. Motion help reports the actual selected provider, readiness, calibration and sensor conditions. Selecting USB does not by itself establish that the sensor is calibrated or that USB input is ready.

### Keyboard

One compact chooser opens **Motion**, **Touch**, **Spoken letters** and **Shortcuts**; four persistent submode tabs are unnecessary. Motion and touch share a two-level 3 x 3 character grid, preview, case and symbol pages. The nine-cell layout is an explicit user revision of the original six-cell prototype. Motion moves local focus only; empty cells never receive focus or confirm input.

The chooser also opens **Common keys**, a separate live-host panel for Esc, Tab, Enter, Backspace, forward Delete, Home, End and arrows. It names the selected input host and states that keys send immediately while the preview stays unchanged. **Delete draft** removes a preview character only. Copy/Paste/Undo retain the existing Ctrl/Command modifier choice in Shortcuts; there are no sticky modifiers. The main nine-cell layout remains spacious.

Flow: choose group -> choose character -> confirm into preview -> send. Keep delete, space and send visible. Sending never includes Enter; expose stop and accepted-prefix progress. An Alpha sample yields an A candidate, which needs confirmation before entering preview. Prefer spelling words for easily confused letter sounds.

Spoken letters and arbitrary dictation are distinct features. The browser prototype labels its samples and never requests microphone access or pretends to run recognition.

### Microphone / speaker

Offer **Voice / meeting** and **Speaker**, a prominent mute control and volume. Speaker mode never uploads microphone audio. Independent audio mode produces no motion or character output. Background audio becomes available in mouse/keyboard mode only after its concurrency capability is qualified.

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

The user-authorized focused-workspace revision supersedes the original fixed-tab geometry; product workflows and requirements remain the acceptance baseline. Diagnostic buttons and sample text cannot replace the touchpad or preview-first keyboard flow. See [design conformance](design-conformance.md) for evidence levels and unresolved hardware acceptance.


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
waits for input cancellation, release recovery and disconnection; changing the
connection while confirmation is open invalidates that confirmation.

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

Bluetooth settings can edit this device's local name without renaming a host.
Name and Wi-Fi password entry share a local-only editor with separate bounded
buffers and purpose/session identity. Passwords are masked, never prefilled and
never sent into the HID preview, ordinary intents, labels or diagnostics. Only
explicit Apply/Join submits a private copy; acceptance is not manager completion.
Close, Back, AI double, lock, screen-off, session replacement and the five-minute
deadline clear local drafts. AI single cannot activate the hidden HID grid.
See the [native setup contract](../design/native-wifi-ui.md) for limits and states.

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
