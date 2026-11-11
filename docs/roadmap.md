# Roadmap

| Priority | Goal | Evidence needed |
| --- | --- | --- |
| Input | Preserve the calibrated aiming reference across ordinary pauses and recover from genuine sample loss. Verify smooth touch input, drag release and handoff on real hosts. | Repeatable physical trajectories, input continuity across UI boundaries, and host-observed release after disconnect and congestion. |
| Bluetooth | Reconnect a saved Classic audio peer without a new pairing while preserving its independent BLE input record. | Repeat cold boot and saved-peer reconnection with both services paired. Verify explicit Forget changes only the selected record. |
| Audio | Qualify independent Mic and Speaker on USB and Bluetooth before making coexistence claims. | Host capture and audible playback, restart and mute behavior, and sustained playback with drop and underrun accounting. |
| Host compatibility | Qualify the composite HID reports on iPhone, iPad, Windows and macOS. | Host OS versions, layout and accessibility configuration, pairing, sleep, reconnect and observed application input. |
| Management | Qualify browser/native authorization, sensor export and signed updates under interrupted connections and power loss. | Device receipts and rollback for the supported partition layout, sustained export at a measured rate, and explicit authorization expiry and revocation. |
| Endurance | Measure resource headroom during extended powered operation. | Timing, memory, thermal and storage behavior during combined use and recovery. |

See [validation](validation.md) for current evidence. Initial release goals
cover one selected HID target, local input and independently qualified audio.
Full dictation, screen capture from a paired host and multi-host HID broadcast
are outside that scope.
