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
                    HID transport
                       BLE host

ES8311 <-> audio manager <-> HFP / A2DP / optional UAC
                    \-> AFE / spoken-letter recognizer
```

UI sends intents to one coordinator. Bluetooth and recognition callbacks post events; they must not directly mutate LVGL or send characters.

## Modes and concurrency

`foreground_mode = mouse | keyboard | audio`.

`audio_state = off | voice_muted | voice_active | speaker` is separate. Audio may remain active behind mouse/keyboard only when `coexistence_verified` is enabled for that target/firmware combination. Compilation alone cannot set that capability.

Queued events carry an epoch and target identity. Mode changes, disconnects, target replacement, locking and ownership changes invalidate the epoch and cancel residual work. Recheck epoch, owner, link and mode immediately before transmission.

Initially support one HID target. Audio defaults to the same physical host, although BLE and Classic may expose separate pairing entries. Different-host concurrency needs separate peer identities and link states, never a single ambiguous connected flag.

## Motion and selection

Begin with 100 Hz IMU sampling and monotonic intervals. Reject long gaps and invalid values, clearing residual movement. Estimate gyro bias from stationary samples. Process deadzone, filtering, time integration, gain, fractional carry and bounded HID movement.

Producer changes clear backlog. Character selection reuses integration but targets local grid focus. Reset the reference on confirmation, add boundary hysteresis and never dwell-submit. Validate axis signs against actual grip. Do not rely on a magnetometer for default heading around magnetic attachments.

## Audio resources

BLE HID and Classic SCO/A2DP share radio scheduling. Prioritize key-up/link handling, then audio buffers, then UI animation and experimental recognition. Bluetooth/audio callbacks must not write NAND, wait synchronously on LVGL or run full-sentence ASR.

One audio manager owns codec initialization, formats, capture/playback and mute. Recognizers subscribe to frames. Prefer ESP-SR mn7_en for letter commands; full transcription belongs in an isolated experiment. No empty success-returning audio stubs.

Qualify independent HFP and A2DP before HID + HFP stress testing. One USB manager must own UAC/CDC resources to avoid conflicting stack instances.

## Future Mac interface

Proposed actions: `move_relative`, `mouse_button`, `scroll`, `key`, `text_ascii`, `release_all`, `get_state`. Include protocol version, session ID, request ID, epoch, target ID and deadline. Distinguish accepted, sent and rejected; sent does not mean target UI success.

Remote control uses an authenticated short lease. Expiration cancels queues and releases keys. Local takeover wins and never automatically resumes a script. Screen recognition and outcome checks run on the Mac; Mosaico does not relay iPad video.
