# Product requirements

ESP32-INPUT provides standard USB or Bluetooth mouse and keyboard input to
one unlocked host. Basic input works without host software or Wi-Fi. The
target board is ESP-Mosaico. Detect the board revision at startup and expose
actual capabilities. iPhone, iPad, Windows and macOS are qualification targets.

This document describes required behavior. See [validation](validation.md)
for the results and [roadmap](roadmap.md) for priorities. The
[interaction contract](interaction.md) specifies the UI and input transitions.

## Input and safety

| ID | Feature | Acceptance |
| --- | --- | --- |
| M01 | Motion mouse | Turning with valid samples produces continuous directional relative HID movement using the calibrated aiming basis. Stopping holds the pointer and resuming uses fresh motion. |
| M02 | Touch fine control | Relative single-finger movement; quarter/eighth gain; touch priority and a short release freeze |
| M03 | Mouse buttons and wheel | Left/right click and explicit drag. Tap either scroll arrow for one wheel step or slide one finger vertically anywhere in the right scroll column. Interruptions release drag. |
| M04 | Touchpad gestures | Configurable tap-to-click: single tap, double tap and double-tap-and-hold drag; movement-only when disabled |
| M05 | Calibrated aiming reference | Set the aiming direction during sensor calibration, preserve it across Motion toggles and touch pauses, and provide separate manual Recenter |
| K01 | Composite HID | One stable HID service with separate mouse/keyboard report IDs; no re-pairing when changing modes |
| K02 | Motion character selection | Group -> character -> explicit confirmation into preview; local focus only |
| K03 | Character entry | A-Z/a-z, digits, space, delete, common ASCII punctuation and clear-preview action |
| K04 | Explicit send | Visible target, preview and progress; no appended Enter; remaining queue can be cancelled |
| K05 | Shortcuts | Host-specific Ctrl/Command configuration rather than one hardcoded mapping |
| K06 | Common keys | One-tap Keys from every Keyboard submode: Copy/Paste/Undo/Select all, Esc, Tab, Enter, Backspace, forward Delete, arrows, Home and End in a symmetric 5 x 3 grid without modifying the preview |
| S01 | Lock | Release HID, cancel queued input, suspend remote control and mute; long press again to unlock |
| S02 | Reconnect | No replay of old clicks, keys or text; motion stays off and remote tasks stay stopped |

### Mouse behavior

Calibrate while holding the device in the desired aiming direction. Motion
produces relative HID output and remains off at cold boot. Touch, menus,
physical controls and haptics suppress it as defined in the interaction
contract. Ordinary pauses preserve the valid reference and discard pending
movement. Recenter changes the direction without recalibrating sensor bias.
See [Mouse aiming](motion-estimation.md).

Tap-to-click defaults on. A contact originating on the pad keeps its pointer
action until lift. Crossing the pad boundary cancels tap eligibility. Expose
two-finger gestures only when the provider reports independent contacts.
[Optional Navigation](interaction.md#optional-navigation-layout) uses explicit
Home and Back controls while preserving the touchpad and scroll column.

### Keyboard behavior

Entering Keyboard from another workspace selects Touch and preserves the
draft. Motion changes local focus. The grid uses group selection, character
confirmation and an explicit Send. Direct Keys actions leave the draft intact.
The target must use a supported US ANSI physical layout. Interrupted sends
leave the unsent suffix in the draft.

## Spoken letters and independent audio

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

Selecting an audio route does not start Mic or Speaker. Expose waiting,
active, stopping and fault states from the audio owner. Use a separate
acknowledgement for mute and codec shutdown. Mic level describes fresh
local samples and does not establish host capture. Start requires a usable
route and appropriate capture permission. Recording requires explicit
activation, visible indication and bounded storage. See the
[media interface](api-audio-recording.md).

Qualify each audio route independently before coexistence. Treat same-host
HID with HFP, same-host HID with A2DP, and different-host HID/audio as separate
cases. Audio defaults off and the microphone muted. A known route or saved
bond does not grant capture.

## Device controls and persistence

| ID | Feature | Acceptance |
| --- | --- | --- |
| D01 | Power and connection status | A compact context indicator and on-demand Status panel show actual gauge-reported battery percentage/charge state, Wi-Fi state/signal and the selected input transport. Bluetooth shows connection/readiness and the current peer, without signal bars. Unknown, stale and unavailable readings remain distinct. Never derive a host name or external power from a guess. |
| D02 | Multiple Bluetooth pairings | Retain multiple saved peers within the configured SDK capacity (15 in the initial build; list storage is bounded at 32), with current-peer marking only when verified. Refuse additional pairing at capacity without evicting existing peers. A visible, bounded pairing window admits additional hosts; existing bonds may reconnect outside it. Show remaining time, readiness and failures. Disconnect retains bonds; forgetting requires local confirmation and safe input cancellation/disconnection. Support one active HID connection initially, without promising peripheral-initiated connection or simultaneous host control. |
| D03 | Wi-Fi setup | The device scans existing Wi-Fi networks, provides local selection and masked router-password entry, and joins the existing LAN. The computer stays on its network and never needs a generated device-hotspot password for normal setup. Remember networks, reconnect with bounded backoff, show actual SSID/IP/signal, and support confirmed forgetting. A recovery hotspot is an explicit secondary service only. Provisioning alone does not enable remote HID or MCP. |
| D04 | Display timeout | Default five minutes; choices are one, five, ten, thirty minutes or Off. Blank only the screen, leaving radios and enabled mouse control running. Apply on all power states so unknown supply telemetry does not defeat the plugged-in use case. |
| D05 | Safe wake and preferences | The first complete touch/AI gesture wakes only. Pause hidden keyboard selection; preserve the draft and lock state. Brightness, theme, haptics, shortcut modifier and timeout persist through a versioned validated record, coalesced writes and truthful save errors. |
| D06 | Consistent UI and device information | Use shared layout metrics, aligned text/control grids and both themes on every page. Separate Connectivity, Display, Input and About pages. Show firmware/board/capabilities without secrets; show actual audio/speech availability, session and fault state, with reasons for unavailable routes. |
| D07 | Input transport selection | System settings offers Bluetooth and USB, independent of foreground mode. Standard USB composite mouse/keyboard retains diagnostics under one USB stack. Cancel queued input and complete old-host releases before switching; an already disconnected old host permits explicit switching without claiming release delivery. A connected, suspended or stalled host must recover or disconnect, otherwise the switch times out without changing selection. Require fresh zero reports on the new link. Never duplicate input to both hosts or automatically fall back after cable removal. Distinguish selection, physical connection, readiness, switching and unavailable capability. |
| D08 | Startup identity | Show a lightweight ESP32-INPUT startup logo consistent with the dark/pale-blue design, then hand off to the saved workspace (Mouse by default). No artificial startup delay, additional full-frame allocation, hidden fault or automatic microphone activation. |
| D10 | Local device reset | About offers a second local confirmation to clear saved user data and restart to defaults. Preserve firmware, OTA security state, factory assets and hardware identity; follow the reset boundary in [interaction.md](interaction.md). |
| D09 | Developer and source identity | About and startup diagnostics identify the developer, repository and build-time Git commit with Clean/Modified/Unknown state. Preserve the separate firmware version and ELF build fingerprint. Missing Git data is unknown, never a fabricated commit or clean claim. |

Restore the saved workspace at cold boot, with Mouse as the fallback and
Touch as the Keyboard entry mode. AI double-press opens Menu. A sustained AI
press locks or unlocks through the [physical control contract](interaction.md#physical-controls).
Coalesce validated preference writes. Preserve unknown state when capability,
power or storage evidence is missing. [Configuration](configuration.md) owns
types, units and apply/save semantics.

## Connectivity and updates

- Bluetooth stores multiple bonds and uses one active HID destination.
  Mouse & Keyboard and Audio have separate records and readiness. New pairing
  requires a bounded window. Disconnect retains the bond. Forget acts on the
  exact selected record where supported. [Peer behavior](bluetooth-peer-snapshots.md)
- Wi-Fi joins an existing station network. The setup hotspot is a bounded
  recovery option. Keep Wi-Fi credentials inside the local editor and network
  owner. Station connectivity alone grants no LAN access. [Wi-Fi setup](wifi-provisioning.md)
- Local UI, HTTPS/WSS and MCP share typed semantics and owner validation. LAN
  clients require scoped authorization. Local lock, takeover and release keep
  priority over scans, reconnects and transfers. [LAN API](remote-control.md)
- Firmware updates verify the signed artifact and matching layout, stage an
  inactive application slot and activate only after the release and maintenance
  gates. Preserve the confirmed image across interrupted staging and require
  health confirmation or rollback. Partition migration and recovery are
  separate operations. [Update procedure](wifi-management-ota.md)
- The sensor journal exposes actual sensor availability, ordered timestamps,
  units and explicit loss over USB or authenticated LAN. Diagnostic capture
  and export stay bounded and keep raw private data local. [Sensor API](sensor-api.md)

## Qualification

Validate source, host behavior, the pinned target build and actual hardware
at their respective boundaries. Use [validation procedures](test-coverage.md)
and the [delivery checklist](delivery-checklist.md). Extended powered use
requires measured timing, resources, thermal behavior and recovery.

Host screen capture, automatic unlock, full dictation, air handwriting and
multi-target HID broadcast are outside the initial release scope.
