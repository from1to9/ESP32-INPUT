# Product requirements v0.1

Baseline: 2026-10-21. Product: ESP32-INPUT. Current user decisions take precedence over proposed defaults.

## Goal and scope

Use one Mosaico to control one unlocked iPhone, iPad or computer. Basic mouse and keyboard operation must work without a Mac or network. A Mac may later provide remote commands, screen recognition and heavier transcription, but is optional for local input.

The initial tablet is an iPad Pro 2018. The development Mosaico is confirmed as v1.2 by its hardware eFuse; supported revisions must still be detected at runtime. Long-term external power is expected. The first release does not capture iOS screens, unlock devices or implement business approval, attendance or account-switching workflows.

## Confirmed decisions

1. Three primary modes: mouse, keyboard and microphone/speaker. Cold boot enters mouse mode with motion disabled.
2. A click toggles mouse motion. Once enabled, handheld waving controls relative movement without holding a button. Use inertial sensor data and holding orientation to estimate natural direction and speed; direct wrist-rotation pointer control is superseded by the 2026-10-23 user revision.
3. Motion character selection, touch selection and spoken English letters belong to keyboard mode.
4. Prioritize iPhone, iPad, Windows and macOS. No proprietary target client is required for basic HID, but pairing, input layout and accessibility settings may still be necessary.
5. Prefer concurrent mouse and microphone operation. Deliver independent modes first if concurrency cannot be made stable.
6. Qualify three different connection cases separately: HID plus HFP/A2DP on one host; HID and audio on different hosts; multiple HID targets. The first case has priority. Remembering multiple bonds does not imply simultaneous control.
7. Keep multiple Bluetooth pairings and show the actual connected peer. Bluetooth is a connection state, not a signal-strength meter in the product UI. Wi-Fi retains measured signal display.
   Add host must lead to a visible pairing page showing the effective device
   name, preparation, acknowledged advertising and remaining window, with Stop
   and an actionable failure reason. If already connected, explain the required
   disconnect before confirmation. Saved-host details support exact single-host
   forgetting. Forget all waits for both bond removal and durable target-policy
   cleanup; it does not implicitly open a new pairing window. Selection policy
   changes, disconnection and new trust are distinct acknowledged operations.
8. Functional mode and connection transport are independent. Every primary mode
   offers Bluetooth or USB selection. System input settings select the shared
   mouse/keyboard link; the Audio connection entry selects the audio link under
   the Mic/Speaker workspace revision below. Their acknowledged connection states
   remain separate, allowing the same-host and different-host cases above.
   The initial input preference is Bluetooth and is saved after a successful
   switch. Implement and qualify USB/Bluetooth mouse and keyboard first; audio
   requires separate working drivers and host tests before its transport can be
   shown as available.
9. Wi-Fi is a separate management/control channel, not an alternative HID target.
   It may remain connected while Bluetooth or USB carries input. Its services are
   authenticated remote control, state reporting, MCP bridge access and firmware
   download/update. Provisioning alone does not enable these services.

## P0: complete the input loop

| ID | Feature | Acceptance |
| --- | --- | --- |
| M01 | Motion mouse | Handheld waving gives intuitive relative movement; stopping holds the pointer; no braking reversal, unexplained timeout or stale burst |
| M02 | Touch fine control | Relative single-finger movement; quarter/eighth gain; touch priority and a short release freeze |
| M03 | Mouse buttons and wheel | Left/right click and explicit drag. Tap either scroll arrow for one wheel step or slide one finger vertically anywhere in the right scroll column. Interruptions release drag. |
| M04 | Touchpad gestures | Configurable tap-to-click: single tap, double tap and double-tap-and-hold drag; movement-only when disabled |
| M05 | Automatic grip reference | Establish a stable grip when Motion starts; recover after reference loss without jumps; expose waiting/ready states and a separate Recenter action that preserves sensor bias |
| K01 | Composite HID | One stable HID service with separate mouse/keyboard report IDs; no re-pairing when changing modes |
| K02 | Motion character selection | Group -> character -> explicit confirmation into preview; local focus only |
| K03 | Character entry | A-Z/a-z, digits, space, delete, common ASCII punctuation and clear-preview action |
| K04 | Explicit send | Visible target, preview and progress; no appended Enter; remaining queue can be cancelled |
| K05 | Shortcuts | Host-specific Ctrl/Command configuration rather than one hardcoded mapping |
| K06 | Common keys | One-tap Keys from every Keyboard submode: Copy/Paste/Undo/Select all, Esc, Tab, Enter, Backspace, forward Delete, arrows, Home and End in a symmetric 5 x 3 grid without modifying the preview |
| S01 | Lock | Release HID, cancel queued input, suspend remote control and mute; long press again to unlock |
| S02 | Reconnect | No replay of old clicks, keys or text; motion stays off and remote tasks stay stopped |

### Mouse behavior

The requested behavior uses the accelerometer/gravity and gyro observations to
estimate handheld movement. Holding orientation compensates gravity and maps
left/right and up/down. The user specifies the intended interaction, not a raw
acceleration-to-speed formula or exact absolute displacement. Gyro data may
contribute to orientation; rotation itself must not directly drive the Mouse
pointer. Keyboard angular selection is unchanged.

Movement should respond continuously to intentional waving, brake without an
immediate direction reversal, and remain still after the hand stops. Do not
introduce an arbitrary short movement timeout to conceal estimator drift. Keep
independent horizontal/vertical sensitivity, inversion, filtering and touch
freeze configurable with explicit units. A new estimator must migrate old
HID/degree settings rather than reinterpret them. HID units are not screen pixels.

The [algorithm investigation](motion-estimation.md) records candidate open-source
libraries, sampling constraints and the validation boundary. The installed
angular candidate has not passed user acceptance. A replacement remains a local
prototype until mathematical, causal replay and physical hand tests establish
usable behavior. Enabling Motion and Recenter retain their acknowledged grip
acquisition flow; changes in reference must never produce pointer jumps.

Expose manual **Recenter** separately from **Calibrate sensor**. Recenter preserves
the established gyro bias and settings, clears pending motion carry, and waits
for valid stable grip evidence before resuming the existing Motion intent.
It never enables a disabled mode or treats unavailable hardware as calibrated.
Show waiting while grip acquisition/recovery is pending, and distinguish that
state from link, sensor, calibration and ownership failures. Ordinary touch
pauses and zero movement do not indicate failed grip acquisition. The same
operation must be available through the authenticated Wi-Fi input API with the
existing ownership, lease, readiness and stale-request checks.

Touch start, physical button down, menus, calibration and keyboard selection suppress host pointer output. Entering keyboard/audio clears the motion intent; returning to mouse requires explicit re-enabling. Leaving fine control may restore the previous motion intent. Haptic feedback must also suppress IMU output during vibration.

Tap-to-click defaults on and is a durable preference. A short stationary contact
produces one left click; a second short contact produces the second click. On a
second contact held or moved, dragging holds the left button until release.
Movement outside the tap slop suppresses ordinary click recognition. With the
preference off, the touchpad only moves the pointer; explicit click/drag controls
remain available. Menus, lock, wake capture, owner/transport changes, stale
gestures and cancellation must not generate clicks or leave a held button.
Gesture time and distance bounds belong to the portable interaction profile.

Motion and touchpad gestures coexist. Enabling Motion does not disable the
default single/double tap or double-tap-and-hold drag behavior. An explicitly
saved movement-only preference remains respected. Touch takes pointer priority;
release applies the configured settling freeze before restoring the existing
motion intent. Single-finger swipes move the pointer without a hidden navigation
action. Additional scroll/right-click gestures require actual multi-touch
provider evidence; unavailable multi-touch must not be simulated from ambiguous
single contacts. Expose concise gesture help, visible drag state and a clear
release/cancel action. Gesture interruption must never leave a host button held.

#### Optional Navigation controls

Input settings provides a persisted **Navigation** layout, disabled by default.
It replaces only the Mouse header Recenter rectangle with independent Home and
Back buttons; pointer, wheel, critical controls, Menu, ordinary layout and keyboard
draft behavior remain intact. Input settings retains **Reset grip**, distinct
from sensor calibration and unable to enable Motion by itself.

Home uses the existing third mouse button and requires an explicit AssistiveTouch
Button 3 to Home mapping. The setup instructions must permit returning to Mouse
to emit the button during registration. Back is a Command+[ tap for supporting
apps, not a system-wide return or a simulated screen swipe. Both use existing
local ownership and USB/Bluetooth send/release recovery, and must not act across
drag, lock, sleep, modal, maintenance or target/transport boundaries. See the
[Navigation interaction contract](interaction.md#optional-navigation-layout).

### Keyboard behavior

- First entry and re-entry from Mouse or Audio default to Touch. Menu, Settings, Keys and repeated selection of the current Keyboard mode preserve the current submode and draft; Motion remains explicitly selectable.
- The device displays a two-level, three-column by three-row character grid; the host needs no custom input method. This user-requested nine-cell update supersedes the original six-cell layout while retaining the original preview and explicit-send workflow. Empty character cells cannot receive motion focus or confirm a character.
- Motion changes local focus; AI single-click confirms. Touch uses the same grid and buffer.
- Integrate relative angular movement, reset the center after confirmation, apply boundary hysteresis and never auto-confirm from dwell or jitter.
- Characters enter preview first. Send emits key-down/key-up pairs without Enter. Track the accepted prefix and preserve only unsent text after interruption; never automatically replay a partial send.
- A persistent Keyboard header Keys button opens a separate live-host panel from every submode. It sends explicit key-down/key-up pairs to the selected host. Copy/Paste/Undo/Select all follow the saved Ctrl/Command setting; no clipboard contents are read. Enter is deliberate and never appended by Send. Preview deletion, host Backspace and host forward Delete are distinct operations; key-panel actions leave the preview unchanged.
- Initial encoding is US ANSI. The host must use a matching physical layout and English input mode. Other layouts require explicit support.
- Transport acceptance does not prove text reached the intended field. Focus is user-controlled until a separate visual feedback loop exists.

## P1: spoken letters and independent audio

| ID | Feature | Design |
| --- | --- | --- |
| V01 | English letter commands | Prefer Alpha-Zulu; optional zero-nine; direct single-letter pronunciations remain experimental aliases |
| V02 | Recognition flow | Explicit short listening window -> candidate -> confirm into preview; no automatic sending |
| V03 | Local recognition | Prefer ESP-SR mn7_en; choose confidence/rejection thresholds from measurements |
| A01 | Voice microphone | HFP HF with ES8311; first validate Windows/macOS capture, playback and mute |
| A02 | Speaker | A2DP Sink with SBC; appropriate mono mix, no stereo-speaker claim |
| A03 | Audio controls | Two primary controls, Mic and Speaker, with independent observed start/stop state, microphone mute and speaker volume; USB/Bluetooth is the connection choice |
| A04 | Echo handling | Duplex playback/capture requires AEC and acoustic tests; otherwise limit experiments to separated output or half duplex |
| A05 | Local recording | Explicit start/stop, visible capture state, bounded storage and duration, recoverable partial files, playback/list/delete/export and truthful completion; no automatic restart or upload |

On-device letter recognition consumes local microphone frames; it does not require HFP transport. One audio manager owns capture/codec resources. Recognition and HFP must not independently reopen the codec.

The 2026-10-23 user revision simplifies the primary Audio workspace to Mic and
Speaker. Do not expose Routes, Roles, Voice, recording setup or internal profile
names as competing primary modes. The audio owner chooses a supported Bluetooth
profile for the requested microphone/speaker combination; the UI does not guess
from the HID connection. Connection transitions must stop and acknowledge old
consumers before binding a new peer/profile. USB enumeration alone proves neither
microphone delivery nor speaker playback. Show waiting, active, stopping and
failure from actual stream/codec observations. Retain recording and spoken-letter
backend contracts; spoken-letter controls remain in Keyboard.
Each unavailable Start control must show the owning admission reason, such as
missing host audio, unsupported microphone capability or an operation still
stopping. Derive the explanation from the same decision that disables the
control; a generic Off label or a previously successful command is insufficient.

The Mic card includes a live digital input level, derived from real local PCM
already acquired with capture permission. Show a compact RMS dBFS value and a
logarithmic bar, with separate off, waiting, unavailable, quiet and clipping
states. Missing or stale samples must never look like silence or retain an old
live value. This is local microphone activity, not proof that a host has saved
or received a recording. Do not add hardware reads or retain audio for the meter;
bound observation updates to 10 Hz and keep them independent of command
revisions, audio scheduling and existing Start, Stop, Mute and volume controls.

## P2: concurrency and remote extensions

- Qualify same-host HID + HFP first and HID + A2DP separately. Enable background audio in mouse/keyboard mode only after qualification.
- Different-host HID/audio requires independent peers, link states and failure rules. It is not a first-release default.
- USB keyboard/mouse plus diagnostic CDC is part of the current input work. UAC audio composition still needs separate resource and OS validation.
- Mac/Codex actions must pass through one coordinator. Local activity takes ownership immediately, consuming the first takeover action rather than forwarding it.
- Full offline English transcription, air handwriting, multiple simultaneous HID targets and LE Audio do not block P0.

## Resources and failures

Prioritize key release and HID scheduling over animation, recognition and audio UI. On overload, reduce UI refresh or stop experimental recognition; never silently discard key-up. Disconnect cancels the queue. Recognition results from an old session must not reach a new target.

Audio defaults off and the microphone muted. Keep samples in short-lived memory, with no recording persistence or upload by default. Diagnostic capture requires explicit activation and indication. Standalone operation does not require Wi-Fi.

## Added foundation and deployment requirements

- Provide a global haptic feedback switch. Confirmations, mode changes, locking and errors may use short bounded pulses; continuous motion and per-report transmission stay silent. Missing or failed motors expose an unavailable capability. Runtime disable cancels pending pulses; the preference is saved with other device settings.
- Keep input semantics, encoding, queues and remote contracts independent of board pins and BSP types. Porting to another ESP32 requires a board adapter and explicit feature/transport capabilities, plus independent hardware qualification.
- Support extended external-power operation with bounded memory/queues, watchdog coverage, rate-limited diagnostics, no-host USB logging behavior, fault recovery and long-duration evidence. A brief successful run is insufficient for this claim.
- Before unattended-use qualification, define an idle display dim/off policy that does not suspend Bluetooth control, verify available power/temperature telemetry, and test supply interruption and thermal behavior under realistic enclosure conditions. Do not invent battery, charging or temperature capabilities on boards that do not expose them. Coalesce persistent settings and avoid per-frame or per-heartbeat flash writes.
- Design Wi-Fi remote control and state display for all implemented capabilities, with authenticated ownership, local override and explicit unavailable features. The [remote-control contract](remote-control.md) defines the API and MCP bridge boundary.
- Review all first-party code in an independent conversation and retain actionable findings, fixes and relevant retest evidence before delivery.

## Device basics added to the current delivery

These additions preserve the original Mouse / Keyboard / Audio workflows.

| ID | Feature | Acceptance |
| --- | --- | --- |
| D01 | Power and connection status | A compact context indicator and on-demand Status panel show actual gauge-reported battery percentage/charge state, Wi-Fi state/signal and the selected input transport. Bluetooth shows connection/readiness and the current peer, without signal bars. Unknown, stale and unavailable readings remain distinct. Never derive a host name or external power from a guess. |
| D02 | Multiple Bluetooth pairings | Retain multiple saved peers within the configured SDK capacity (15 in the initial build; list storage is bounded at 32), with current-peer marking only when verified. Refuse additional pairing at capacity without evicting existing peers. A visible, bounded pairing window admits additional hosts; existing bonds may reconnect outside it. Show remaining time, readiness and failures. Disconnect retains bonds; forgetting requires local confirmation and safe input cancellation/disconnection. Support one active HID connection initially, without promising peripheral-initiated connection or simultaneous host control. |
| D03 | Wi-Fi setup | The device scans existing Wi-Fi networks, provides local selection and masked router-password entry, and joins the existing LAN. The computer stays on its network and never needs a generated device-hotspot password for normal setup. Remember networks, reconnect with bounded backoff, show actual SSID/IP/signal, and support confirmed forgetting. A recovery hotspot is an explicit secondary service only. Provisioning alone does not enable remote HID or MCP. |
| D04 | Display timeout | Default five minutes; choices are one, five, ten, thirty minutes or Off. Blank only the screen, leaving radios and enabled mouse control running. Apply on all power states so unknown supply telemetry does not defeat the plugged-in use case. |
| D05 | Safe wake and preferences | The first complete touch/AI gesture wakes only. Pause hidden keyboard selection; preserve the draft and lock state. Brightness, theme, haptics, shortcut modifier and timeout persist through a versioned validated record, coalesced writes and truthful save errors. |
| D06 | Consistent UI and device information | Use shared layout metrics, aligned text/control grids and both themes on every page. Separate Connectivity, Display, Input and About pages. Show firmware/board/capabilities without secrets; show actual audio/speech availability, session and fault state, with reasons for unavailable routes. |
| D07 | Input transport selection | System settings offers Bluetooth and USB, independent of foreground mode. Standard USB composite mouse/keyboard retains diagnostics under one USB stack. Cancel queued input and complete old-host releases before switching; an already disconnected old host permits explicit switching without claiming release delivery. A connected, suspended or stalled host must recover or disconnect, otherwise the switch times out without changing selection. Require fresh zero reports on the new link. Never duplicate input to both hosts or automatically fall back after cable removal. Distinguish selection, physical connection, readiness, switching and unavailable capability. |
| D08 | Startup identity | Show a lightweight ESP32-INPUT startup logo consistent with the dark/pale-blue design, then hand off to the normal Mouse workspace. No artificial startup delay, additional full-frame allocation, hidden fault or automatic microphone activation. |
| D09 | Developer and source identity | About and startup diagnostics identify the developer, repository and build-time Git commit with Clean/Modified/Unknown state. Preserve the separate firmware version and ELF build fingerprint. Missing Git data is unknown, never a fabricated commit or clean claim. |

Project attribution and source identity follow the [build identity contract](build-identity.md).

### Focused mode workspace revision

The user subsequently requested that each mode show only its core controls,
opening mode selection, device status and settings through a screen button or
physical AI double-click. This explicitly supersedes the prototype's persistent
bottom tabs and detailed header, while preserving its product workflows. Keep
the menu entry discoverable and at least 44 px, the current mode and input
context understandable, and lock/remote/recording/error state visible when
relevant. Do not turn touchpad movement into hidden navigation gestures.

Mouse prioritizes a large relative pad, scroll, motion and click/drag/release.
Keyboard prioritizes the nine-cell grid, draft and explicit Delete/Space/Send;
submode and infrequent editing controls are on demand. Audio prioritizes mute,
volume and true capability/session state. POWER retains hardware power control;
AI single-click remains contextual, double-click opens the shared menu, and
long press locks/unlocks. Runtime BOOT access is not assumed.

Reduce hidden-page work, redundant invalidation and eager object allocation
without weakening whole-gesture wake, local takeover, stale-event rejection or
credential clearing. Compare actual LVGL object counts and idle drawing before
and after the change, and measure target internal/DMA memory separately. A
sparser layout alone does not prove the existing display failure is repaired.

Display brightness defaults to 50 percent, with ten-percent steps from 10 to 100.
Physical touch/button activity resets inactivity; routine sensor, radio and
status updates do not keep the screen awake. Screen Off is not a lock, system
sleep or HID cancellation. An AI hold used to wake cannot also unlock. Changing
the timeout starts a fresh interval; selecting Off wakes the display.

The BQ27220 percentage remains the gauge's reported estimate, not a separately
validated battery-capacity measurement. The pinned board has no independent
external-power signal. A lack of USB enumeration must not be presented as proof
that external power is absent.

The setup hotspot is explicit, expires after ten minutes and uses a fresh local
password/session. HTTP provisioning is restricted to its local interface with
bounded input, session validation and escaped output. Saved Wi-Fi passwords are
never shown or logged. Development flash is not encrypted; do not claim physical
credential protection. Provisioning security does not qualify the later remote
control API. Wi-Fi setup must not erase bonds or unrelated settings on failure.

This hotspot is a recovery-only capability, not the normal Wi-Fi setup flow.
The user's existing router credentials stay inside the private local editor
and network owner; they never enter the HID preview or require typing a newly
generated device password on another computer.

## Complete delivery boundary

The latest user request requires all previously accepted modules, including real
audio/recording, spoken-letter input, configuration, authenticated LAN control,
MCP, firmware updates, motion refinement and the startup logo, to be completed
before the next combined submission. Earlier
published checkpoints remain historical evidence. Module self-tests and
independent review continue during development; no placeholder or successful
build counts as a completed physical capability. Preserve explicit hardware and
host qualification limits, and report any requirement that cannot be implemented
on the actual board rather than silently dropping it.

The latest delivery instruction defers all device testing and flashing. Complete
local host/native tests, the pinned target build and independent review, then
commit and push the combined source. Do not access serial/USB, pair a host,
install firmware or perform physical tests for this delivery. Physical
qualification remains pending and is not a source-publication gate. Installation,
partition migration, acoustic quality, host compatibility and endurance must not
be reported as verified by source tests. The [delivery checklist](delivery-checklist.md)
tracks implementation separately from that deferred evidence.

## Connection management and firmware updates

These requirements extend the reviewed single-network provisioning checkpoint;
they are not claims that the current firmware implements the following services.

All implemented device functions must have typed Wi-Fi API operations or an
explicitly documented physical-presence exception. Plan the complete operation
inventory and shared configuration schema before implementing new interfaces.
Include input, settings, connections, volume/mute, microphone/speaker, recording
and data access, updates, status and events. Unsupported capabilities must reject
operations truthfully. API admission is not completion or observed host success.

Use one validated configuration model across local UI, API and MCP: defaults,
types, units, bounds, capability dependencies, apply/restart semantics, version
and persistence policy. Make interaction, haptics, display, reconnect/work policy
and audio/recording parameters configurable where the owning driver permits it.
Keep board profiles/pins separate from runtime preferences. Immutable resource
and safety limits remain explicit; configuration cannot invent missing hardware.
Apply changes transactionally, preserve previous values on rejected validation,
coalesce actual durable changes and report uncertain storage without fake rollback.
Do not persist per-frame state or write unchanged defaults on every boot.

- Provide a bounded Wi-Fi scan list with current, saved, secured and unsupported
  states. Selecting a supported network opens Join or Connect. Keep credentials
  masked and local, support saved-network ordering, and bound background retries.
- Provide saved Bluetooth HID targets with explicit selection and priority. A
  BLE peripheral cannot infer connectable HID hosts from arbitrary advertisements.
  A nearby scan, if supported, must distinguish discovery from host eligibility;
  never offer a fake connection action. New pairing still needs the host's Bluetooth
  settings. Selected-target reconnect must respect the SDK's identity/privacy
  capabilities and preserve input cancellation across changes.
- Keep reconnect ordering separate from runtime work priority. Input-first is
  the default, with balanced and management-first policies where supported.
  Background scanning, reconnect and transfer work must be bounded; key release,
  local lock and local override retain priority under every setting.
- Provide an independently authorized firmware-update API over the management
  channel. A request identifies an approved HTTPS firmware source and verified
  artifact identity. Download to an inactive application slot, validate target,
  board compatibility, size and integrity, and expose progress/error state.
- Preserve the running image until validation succeeds. Stop remote work and
  release held input before installation/restart. Interrupted downloads must not
  replace the bootable image; a failed new-image health check must roll back.
  Reject concurrent updates and define cancellation, retry and reboot boundaries.
- Keep credentials, tokens, private download URLs and update logs out of Git and
  public status. Firmware authenticity needs an explicit trust/provisioning model;
  a checksum alone is not authorization. Do not silently change secure-boot eFuses.
- Qualify partition migration and power-loss recovery before exposing installation.
  Keep the original factory backup and preserve bonds, profiles and preferences.
- Prefer the existing station LAN connection for subsequent development updates,
  status inspection and diagnostic capture downloads after authenticated
  commissioning and device qualification. Ordinary iteration should not require
  entering USB download mode. USB remains the recovery path when boot or network
  services are unavailable; the requested no-new-backup procedure remains a
  separate deployment choice.
- Provide bounded, authenticated diagnostic exports with explicit capture
  identity, firmware/configuration identity, sequence/loss accounting and
  completion/integrity evidence. Network transfer must not block sensor acquisition
  or input handling, grant input ownership, or silently start audio recording.
  Interrupted or incomplete exports remain visibly incomplete. Keep private
  addresses, credentials, input text and audio outside ordinary diagnostic logs.
- Provide a [live sensor read API](sensor-api.md) over the existing USB and
  authenticated station LAN transports, independent of Mouse Motion and screen
  blanking. Expose actual available sensors and observed input events with units,
  timestamps, source identity and explicit loss/unknown state. Support independent
  developer readers and bounded host recording without additional sensor owners.

### Shared Bluetooth manager revision (2026-11-04)

One Bluetooth devices page serves system settings and Audio Manage devices, with entry-selected Mouse & Keyboard / Audio purposes. Each purpose lists its actual saved records, search name and pairing state and exposes only owner-supported operations. Names or private addresses do not merge pairing authority. The original mode, keyboard draft and input transport survive management. Audio remains off until separately started. Unsupported Classic single deletion must remain explicit; no address-only optimistic deletion is acceptable. The [native contract](../design/native-bluetooth-devices.md) and [prototype](../design/bluetooth-devices.html) define the 480px flow.
