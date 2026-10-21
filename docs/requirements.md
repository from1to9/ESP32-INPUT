# Product requirements v0.1

Baseline: 2026-10-21. Product: ESP32-INPUT. Current user decisions take precedence over proposed defaults.

## Goal and scope

Use one Mosaico to control one unlocked iPhone, iPad or computer. Basic mouse and keyboard operation must work without a Mac or network. A Mac may later provide remote commands, screen recognition and heavier transcription, but is optional for local input.

The initial tablet is an iPad Pro 2018. The Mosaico board revision is unconfirmed, possibly v1.2. Long-term external power is expected. The first release does not capture iOS screens, unlock devices or implement business approval, attendance or account-switching workflows.

## Confirmed decisions

1. Three primary modes: mouse, keyboard and microphone/speaker. Cold boot enters mouse mode with motion disabled.
2. A click toggles mouse motion. Once enabled, wrist rotation continuously controls relative movement without holding a button.
3. Motion character selection, touch selection and spoken English letters belong to keyboard mode.
4. Prioritize iPhone, iPad, Windows and macOS. No proprietary target client is required for basic HID, but pairing, input layout and accessibility settings may still be necessary.
5. Prefer concurrent mouse and microphone operation. Deliver independent modes first if concurrency cannot be made stable.
6. Qualify three different connection cases separately: HID plus HFP/A2DP on one host; HID and audio on different hosts; multiple HID targets. The first case has priority. Remembering multiple bonds does not imply simultaneous control.

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

- The device displays a two-level character grid; the host needs no custom input method.
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
- USB UAC/HID/control-serial composition needs separate resource and OS validation.
- Mac/Codex actions must pass through one coordinator. Local activity takes ownership immediately, consuming the first takeover action rather than forwarding it.
- Full offline English transcription, air handwriting, multiple simultaneous HID targets and LE Audio do not block P0.

## Resources and failures

Prioritize key release and HID scheduling over animation, recognition and audio UI. On overload, reduce UI refresh or stop experimental recognition; never silently discard key-up. Disconnect cancels the queue. Recognition results from an old session must not reach a new target.

Audio defaults off and the microphone muted. Keep samples in short-lived memory, with no recording persistence or upload by default. Diagnostic capture requires explicit activation and indication. Standalone operation does not require Wi-Fi.
