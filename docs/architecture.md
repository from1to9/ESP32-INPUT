# Architecture and interfaces

## Technical direction

- ESP-IDF 6.2+ targeting ESP32-S31, with a pinned SDK commit and official [Mosaico BSP](https://github.com/esp-mosaico/esp-mosaico-bsp).
- BSP-managed LVGL 9.x display and touch. Product UI uses high-level BSP interfaces rather than guessed GPIOs.
- Prefer Bluedroid to retain a path to BLE HID and Classic HFP/A2DP. Qualify coexistence separately rather than infer it from independent examples.
- Keep one stable composite HID descriptor across mouse and keyboard modes. Switching producers does not change the device name or require re-pairing.
- Relative X/Y, wheel and buttons for mouse; standard six-key rollover plus modifiers for keyboard. Separate encoding from transport.

## Ownership

```text
IMU / touch / AI key / spoken letters / remote actions
                         |
                input coordinator
          mode + owner + epoch + output gates
                  /             \
               mouse           keyboard
          filter/integrate   select -> preview -> queue
                  \             /
                    HID transport selector
                     /               \
                 BLE host          USB host

ES8311 <-> audio manager <-> HFP / A2DP / optional UAC
                    \-> AFE / spoken-letter recognizer
```

UI sends intents to one coordinator. Bluetooth and recognition callbacks post events; they must not directly mutate LVGL or send characters.

## Modes and concurrency

`foreground_mode = mouse | keyboard | audio`.

`audio_state = off | voice_muted | voice_active | speaker` is separate. Audio may remain active behind mouse/keyboard only when `coexistence_verified` is enabled for that target/firmware combination. Compilation alone cannot set that capability.

Queued events carry an epoch and target identity. Mode changes, disconnects, target replacement, locking and ownership changes invalidate the epoch and cancel residual work. Recheck epoch, owner, link and mode immediately before transmission.

Initially support one HID target. Audio defaults to the same physical host, although BLE and Classic may expose separate pairing entries. Different-host concurrency needs separate peer identities and link states, never a single ambiguous connected flag.

The foreground mode is separate from the selected output transport. System
settings selects Bluetooth (initial default) or USB. A switch cancels queued
work and drains releases on the old transport before selecting the new one.
Disconnect, suspension and endpoint backpressure invalidate stale generations;
cable removal never redirects pending work to a paired Bluetooth host. Both
adapters use the same canonical HID report descriptor. USB diagnostics and HID
share one TinyUSB owner. USB audio is separately capability-gated until its
driver and host behavior are qualified.

The BLE adapter respects the configured SDK bond capacity (15 initially) and
publishes a cached identity list bounded at 32 entries.
Names are optional evidence, never guesses from an address or selected host OS.
Saved bonds and simultaneous links are different capabilities: the initial
adapter supports multiple saved peers and one active input link.

## Motion and selection

Begin with 100 Hz IMU sampling and monotonic intervals. Reject long gaps and invalid values, clearing residual movement. Estimate gyro bias from stationary samples. Process deadzone, filtering, time integration, gain, fractional carry and bounded HID movement.

Producer changes clear backlog. Character selection reuses integration but targets local grid focus. Reset the reference on confirmation, add boundary hysteresis and never dwell-submit. Validate axis signs against actual grip. Do not rely on a magnetometer for default heading around magnetic attachments.

## Audio resources

BLE HID and Classic SCO/A2DP share radio scheduling. Prioritize key-up/link handling, then audio buffers, then UI animation and experimental recognition. Bluetooth/audio callbacks must not write NAND, wait synchronously on LVGL or run full-sentence ASR.

One audio manager owns codec initialization, formats, capture/playback and mute. Recognizers subscribe to frames. Prefer ESP-SR mn7_en for letter commands; full transcription belongs in an isolated experiment. No empty success-returning audio stubs.

Qualify independent HFP and A2DP before HID + HFP stress testing. One USB manager must own UAC/CDC resources to avoid conflicting stack instances.

## Future Mac interface

Wi-Fi is an independent LAN management channel. It may run alongside either
selected HID transport; choosing Bluetooth or USB does not choose the Wi-Fi
network or disable management. Remote actions enter the coordinator, while
status queries and firmware downloads belong to separate bounded services.
Firmware installation must acquire a maintenance gate, cancel remote work and
release held input before reboot. No update path may directly emit HID or bypass
local locking. Network/peer reconnect ordering is separate from runtime work
priority; key release and local override always take precedence.

Proposed actions: `move_relative`, `mouse_button`, `scroll`, `key`, `text_ascii`, `release_all`, `get_state`. Include protocol version, session ID, request ID, epoch, target ID and deadline. Distinguish accepted, sent and rejected; sent does not mean target UI success.

Remote control uses an authenticated short lease. Expiration cancels queues and releases keys. Local takeover wins and never automatically resumes a script. Screen recognition and outcome checks run on the Mac; Mosaico does not relay iPad video.

## Input implementation

`input_core` owns portable state gates and encoding. `hid_engine` owns the descriptor, bounded semantic actions, transport-backpressure expiry and release recovery. `interaction` owns the original grid and draft, gesture classification, stationary calibration, motion selection and touch fractional carry. `haptics` owns deterministic pulse/cooldown/fault policy. The application task is the only engine mutator; UI intent callbacks carry a captured epoch and cannot emit HID directly. Physical lock/release signals bypass the ordinary intent queue. BLE callbacks publish a synchronized snapshot and a generation; the sender rechecks that generation.

`board_port.h` is the board contract; `board_mosaico.c` is the only consumer of BSP APIs. Hardware capabilities are measured during initialization. Board failure prevents Bluetooth startup. USB diagnostics initialize separately so board errors can be investigated. The original product UI is an LVGL backend; headless boards will need a different producer/status backend. See [porting](porting.md).

The haptic worker does not delay the input task. Disabling feedback cancels pending work; continuous movement never generates vibration. The IMU producer uses the board haptic-active gate to avoid self-induced movement. Cold boot and cancellation keep gyro output disabled until explicitly re-enabled after calibration.

`board_mosaico.c` samples the official BMI270 interfaces at 100 Hz and publishes bounded-age snapshots; the coordinator performs no I2C access. Raw AI edges carry monotonic timestamps into a bounded queue. Global touch/physical state suppresses motion independently of normal UI queue admission. UI intents capture both input epoch and visible-context revision, so delayed controls cannot act on a different grid or modal.

The preview holds 64 printable US ANSI characters. One explicit send owns a token and may use successive 32-character HID batches. Accepted key-down prefixes are removed from the preview, even if their key-up later fails; only the unsent suffix remains after cancellation. Enter is never appended. The original design conformance checklist distinguishes source, host, LVGL, physical and target evidence.

Remote control will enter through the same coordinator and use the [versioned Wi-Fi/MCP contract](remote-control.md). No remote-input endpoint is enabled by this build; the temporary Wi-Fi provisioning form has a separate local-only scope. A state view must distinguish hardware capability, implemented feature, link readiness and observed host outcome.
