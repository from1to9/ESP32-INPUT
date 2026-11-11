# Roadmap

The firmware already provides local USB/BLE HID, independent audio controls
and authenticated LAN management. Priorities below are driven by the open
results in [validation](validation.md).

| Next milestone | Acceptance |
| --- | --- |
| Mouse aiming and touch | Verify correct horizontal and vertical direction at different grips, controllable slow and fast movement, no catch-up movement after a pause, and clean drag release. |
| Saved Classic audio | Reconnect an existing HFP/A2DP peer outside a new pairing window, with its separate BLE HID record still usable. |
| Audio quality | Verify audible playback, actual microphone capture, mute and restart for each route. Test sustained playback and coexistence separately. |

After these checks, expand host coverage across iPhone, iPad, Windows and
macOS. Qualify authorization expiry, interrupted sensor exports, signed update
rollback and extended powered operation on representative hardware.

The initial release targets one HID destination and independently qualified
audio. Full dictation, host screen capture and multi-host HID broadcast are
outside that scope.
