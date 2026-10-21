# Product requirements v0.1

Baseline: 2026-10-21. Product: ESP32-INPUT. Current user decisions take precedence over proposed defaults.

## Goal and scope

Use one Mosaico to control one unlocked iPhone, iPad or computer. Basic mouse and keyboard operation must work without a Mac or network. A Mac may later provide remote commands, screen recognition and heavier transcription, but is optional for local input.

The initial tablet is an iPad Pro 2018. The development Mosaico is confirmed as v1.2 by its hardware eFuse; supported revisions must still be detected at runtime. Long-term external power is expected. The first release does not capture iOS screens, unlock devices or implement business approval, attendance or account-switching workflows.

## Confirmed decisions

1. Three primary modes: mouse, keyboard and microphone/speaker. Cold boot enters mouse mode with motion disabled.
2. A click toggles mouse motion. Once enabled, wrist rotation continuously controls relative movement without holding a button.
3. Motion character selection, touch selection and spoken English letters belong to keyboard mode.
4. Prioritize iPhone, iPad, Windows and macOS. No proprietary target client is required for basic HID, but pairing, input layout and accessibility settings may still be necessary.
5. Prefer concurrent mouse and microphone operation. Deliver independent modes first if concurrency cannot be made stable.
6. Qualify three different connection cases separately: HID plus HFP/A2DP on one host; HID and audio on different hosts; multiple HID targets. The first case has priority. Remembering multiple bonds does not imply simultaneous control.
7. Keep multiple Bluetooth pairings and show the actual connected peer. Bluetooth is a connection state, not a signal-strength meter in the product UI. Wi-Fi retains measured signal display.
8. Functional mode and connection transport are independent. Every primary mode uses the System connection selector: Bluetooth or USB. The initial preference is Bluetooth and is saved after a successful switch. Implement and qualify USB/Bluetooth mouse and keyboard first; audio requires separate working drivers and host tests before its transport can be shown as available.
9. Wi-Fi is a separate management/control channel, not an alternative HID target.
   It may remain connected while Bluetooth or USB carries input. Its services are
   authenticated remote control, state reporting, MCP bridge access and firmware
   download/update. Provisioning alone does not enable these services.

## P0: complete the input loop

| ID | Feature | Acceptance |
| --- | --- | --- |
| M01 | Motion mouse | No continuous stationary drift after calibration; rotation gives relative movement; no stale movement burst |
| M02 | Touch fine control | Relative single-finger movement; quarter/eighth gain; touch priority and a short release freeze |
| M03 | Mouse buttons | Left/right click, explicit drag and separate scroll; interruptions release drag |
| K01 | Composite HID | One stable HID service with separate mouse/keyboard report IDs; no re-pairing when changing modes |
| K02 | Motion character selection | Group -> character -> explicit confirmation into preview; local focus only |
| K03 | Character entry | A-Z/a-z, digits, space, delete, common ASCII punctuation and clear-preview action |
| K04 | Explicit send | Visible target, preview and progress; no appended Enter; remaining queue can be cancelled |
| K05 | Shortcuts | Host-specific Ctrl/Command configuration rather than one hardcoded mapping |
| S01 | Lock | Release HID, cancel queued input, suspend remote control and mute; long press again to unlock |
| S02 | Reconnect | No replay of old clicks, keys or text; motion stays off and remote tasks stay stopped |

### Mouse behavior

Convert calibrated angular velocity to relative HID movement using actual monotonic sample intervals and fractional carry. Axis orientation, gain, deadzone, filtering and click freeze are tunable. Initial proposals are a 150 ms freeze and quarter-speed fine control; actual values need hardware tuning. HID units are not screen pixels.

Touch start, physical button down, menus, calibration and keyboard selection suppress host pointer output. Entering keyboard/audio clears the motion intent; returning to mouse requires explicit re-enabling. Leaving fine control may restore the previous motion intent. Haptic feedback must also suppress IMU output during vibration.

### Keyboard behavior

- The device displays a two-level, three-column by three-row character grid; the host needs no custom input method. This user-requested nine-cell update supersedes the original six-cell layout while retaining the original preview and explicit-send workflow. Empty character cells cannot receive motion focus or confirm a character.
- Motion changes local focus; AI single-click confirms. Touch uses the same grid and buffer.
- Integrate relative angular movement, reset the center after confirmation, apply boundary hysteresis and never auto-confirm from dwell or jitter.
- Characters enter preview first. Send emits key-down/key-up pairs without Enter. Track the accepted prefix and preserve only unsent text after interruption; never automatically replay a partial send.
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
| A03 | Audio controls | Local microphone mute, playback volume, link and resource ownership status |
| A04 | Echo handling | Duplex playback/capture requires AEC and acoustic tests; otherwise limit experiments to separated output or half duplex |

On-device letter recognition consumes local microphone frames; it does not require HFP transport. One audio manager owns capture/codec resources. Recognition and HFP must not independently reopen the codec.

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
| D03 | Wi-Fi setup | A local action opens a temporary password-protected setup hotspot and a bounded browser form. Remember one network, reconnect with bounded backoff, show actual SSID/IP/signal, and support confirmed forgetting. No remote HID or MCP endpoint is enabled by provisioning. |
| D04 | Display timeout | Default five minutes; choices are one, five, ten, thirty minutes or Off. Blank only the screen, leaving radios and enabled mouse control running. Apply on all power states so unknown supply telemetry does not defeat the plugged-in use case. |
| D05 | Safe wake and preferences | The first complete touch/AI gesture wakes only. Pause hidden keyboard selection; preserve the draft and lock state. Brightness, theme, haptics, shortcut modifier and timeout persist through a versioned validated record, coalesced writes and truthful save errors. |
| D06 | Consistent UI and device information | Use shared layout metrics, aligned text/control grids and both themes on every page. Separate Connectivity, Display, Input and About pages. Show firmware/board/capabilities without secrets; retain explicit unavailable audio and speech. |
| D07 | Input transport selection | System settings offers Bluetooth and USB, independent of foreground mode. Standard USB composite mouse/keyboard retains diagnostics under one USB stack. Cancel queued input and complete old-host releases before switching; an already disconnected old host permits explicit switching without claiming release delivery. A connected, suspended or stalled host must recover or disconnect, otherwise the switch times out without changing selection. Require fresh zero reports on the new link. Never duplicate input to both hosts or automatically fall back after cable removal. Distinguish selection, physical connection, readiness, switching and unavailable capability. |

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
