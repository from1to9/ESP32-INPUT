# UI, buttons and state transitions

## Primary navigation

Use a 480 x 480 logical display with three bottom entries: **Mouse**, **Keyboard**, **Audio**. Target and settings controls belong in the top bar. The UI and physical buttons use one authoritative state.

Show foreground mode, HID target and link status separately. Once concurrency is qualified, add independent audio target/mute status. Paired, HID-connected and audio-active must not collapse into one green indicator.

### Mouse

Provide a motion toggle, fine gain, large touchpad, separate scroll controls, clicks and explicit drag/release. Any touch suppresses motion; release starts a short freeze. Menus and mode changes cancel drag. Motion follows wrist rotation, not absolute device tilt.

### Keyboard

Submodes are **Motion**, **Touch**, **Spoken letters** and **Shortcuts**. Motion and touch share a two-level character grid, preview, case and symbol pages. Motion moves local focus only.

Flow: choose group -> choose character -> confirm into preview -> send. Keep delete, space and send visible. Sending never includes Enter; expose stop and accepted-prefix progress. An Alpha sample yields an A candidate, which needs confirmation before entering preview. Prefer spelling words for easily confused letter sounds.

Spoken letters and arbitrary dictation are distinct features. The browser prototype labels its samples and never requests microphone access or pretends to run recognition.

### Microphone / speaker

Offer **Voice / meeting** and **Speaker**, a prominent mute control and volume. Speaker mode never uploads microphone audio. Independent audio mode produces no motion or character output. Background audio becomes available in mouse/keyboard mode only after its concurrency capability is qualified.

## Physical controls

The official button factory currently exposes AI only. Its header also declares BOOT GPIO61, but runtime BOOT access is not a first-release dependency. POWER retains hardware power control.

| Action | Mouse | Keyboard | Audio |
| --- | --- | --- | --- |
| AI single-click | Toggle motion | Confirm selected group/character; on voice page start/stop listening or confirm candidate | Toggle microphone mute; speaker playback awaits AVRCP |
| AI double-click | Open mode chooser | Open mode chooser | Open mode chooser |
| AI long press, at least 0.9 s | Stop and lock globally | Stop and lock globally | Stop and lock globally |
| Long press while locked | Unlock with motion off and microphone muted; never resume tasks | Same | Same |

Single and double clicks are mutually exclusive, with an initial 280 ms decision window subject to hardware tuning. Do not emit a single-click first and undo it later. Button down freezes motion. Menus, dialogs and lock overlays own focus; AI input must not activate hidden controls.

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

Settings are not a fourth primary mode; closing them returns to the previous mode. Hidden pages cannot redefine the AI key. Defaults are mouse, motion off, audio off and remote control unauthorized. Show the selected target at all times. Plan high-contrast focus, light/dark appearance, left-handed scroll placement and roughly 44 px touch targets, then verify readability on the actual 2.16-inch display.
