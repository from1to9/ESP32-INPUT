# Portable input core

This is original C11 logic with no Bluetooth, LVGL or BSP dependency. A single coordinator owns `input_state_t`; this is not a thread-safe transport implementation.

Adapters must drain all old queued events after an epoch change, reset `input_motion_t`, mute audio/stop speech where required, then send all-zero mouse and keyboard reports before acknowledging the release for that **same epoch**. If disconnected, discard output locally; a new connection starts with the same release barrier. Old acknowledgements cannot unlock a new session.

Every queued action carries the epoch and owner from creation. Check `input_can_emit` immediately before sending, not only when enqueueing. Touch/menu/haptic freezes additionally gate gyro output; `input_set_gyro(false)` alone does not clear the motion accumulator. Always reset it on a producer change. Remote ownership must be granted by an authenticated, expiring lease in the future adapter; the enum does not provide authentication.

The ASCII encoder emits a key-down payload only. The future sender must wait for transport readiness, send a matching zero key-up, and track the accepted prefix. It must never auto-append Enter. No queue, full HID descriptor, gyro calibration/filter, gesture timing, selector hysteresis or audio manager is implemented here yet.

`input_set_gyro_owned` checks an explicit producer against actual ownership when enabling motion. The original API remains the local-source wrapper; disabling is a safety operation. The coordinator must validate remote lease/producer authority before calling the owned entry point.
