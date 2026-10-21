# Compatibility and acceptance

## Current evidence

Baseline date: 2026-10-21. Evidence currently covers source research, a browser prototype and host-side logic. SDK source preparation has begun, but no S31 target build, device flash or real pairing is recorded. All hardware acceptance items remain pending.

| Host | Mouse path | Keyboard checks | Voice/audio | Status |
| --- | --- | --- | --- | --- |
| iPhone | AssistiveTouch pointer devices | BLE keyboard; English layout/input mode | HFP/A2DP in actual apps | Pending |
| iPad Pro 2018 | Native pointer and optional AssistiveTouch tested separately | BLE keyboard; software keyboard visibility | HFP/A2DP and HID coexistence | Pending |
| Windows 11 | System Bluetooth mouse | US ANSI and Ctrl shortcuts | HFP input/output and A2DP playback | Pending |
| Windows 10 | Add if an actual test host is available | Same layout checks | Hands-Free/Stereo endpoint behavior | Pending |
| macOS | System Bluetooth mouse | US ANSI and Command shortcuts | HFP input/output and A2DP playback | Pending |

For iPhone, check Settings -> Accessibility -> Touch -> AssistiveTouch -> Devices -> Bluetooth Devices, and enable AssistiveTouch. This is system configuration, not a proprietary client installation. See [Apple's pointer-device instructions](https://support.apple.com/en-us/111775).

## Host checks

`python3 tools/check.py` compiles and runs portable C tests for default state, mode/disconnect/lock gates, stale epochs, release acknowledgement, sampling-rate-independent motion, invalid samples and US ANSI encoding. Keyboard mode rejects mouse output. These tests do not validate a BLE descriptor or radio stack.

### Baseline record: 2026-10-21

- C11 tests passed under Clang with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
- Prototype JavaScript syntax passed. Browser checks covered default mouse mode, motion toggle, motion selection of G, the Alpha -> A candidate/confirmation flow, explicit send interrupted by lock, independent audio exit, concurrency simulation and local takeover.
- Browser connections, audio, speech and sending are simulated. No Bluetooth or microphone permission was requested.
- The portable core provides epochs and release gates. The real transport, queue, button timing and product LVGL UI remain unimplemented.
- No S31 build, flashing, pairing, latency or acoustic result is claimed. Next: pin the SDK and build minimum composite HID.

### Standards and English-language migration: 2026-10-21

- General working rules, engineering standards, knowledge boundaries and contribution guidance are defined separately from product requirements.
- First-party documentation and prototype copy are English. Upstream BSP contents and licenses are unchanged.
- First-party text hygiene, local Markdown links, diff whitespace and prototype JavaScript syntax checks passed.
- The English keyboard and spoken-letter screens were visually reviewed in the browser; group/character confirmation and the sample candidate flow were exercised.
- No portable C behavior changed in this documentation/copy update. Previous host results remain historical evidence, not a new hardware result.

## Hardware qualification stages

1. **Board identity:** record revision, BSP/SDK commits, power and configuration. Abort unsupported eFuse values. Check display, touch, AI, IMU, USB power and temperature.
2. **Minimum HID:** pair each host family; check report IDs, encryption/bonding, reconnect and sleep/wake. Type `aAzZ09 !?@` and confirm modifier release.
3. **Mouse:** 60 s stationary, different grips, fast/slow rotation, touch release, fine control, click vibration and interruption during drag. Retain raw/filtered evidence without concealing drift.
4. **Keyboard:** two-level selection, deletion, space, case, mode changes, 100-character sending and cancellation/disconnect/target changes. Old results must not reach a new target.
5. **Spoken letters:** repeated 26-letter/digit/spelling-word trials, confusion matrix and rejection rate, with separate accents/distances/noise conditions. Set thresholds from evidence.
6. **Independent audio:** capture, playback, meeting echo, mute, gain and HFP codec negotiation. Pairing alone is not an audio result.
7. **Concurrency:** same-host HID + HFP and HID + A2DP separately, proposed 30-minute interaction runs, then 8-hour powered soak. Record latency, underruns, disconnects and memory.

These durations are acceptance plans, not completed runs. IMU sample rate or report frequency is not end-to-end latency.

## Device evidence template

```text
Date / firmware commit / BSP commit / IDF commit:
Board revision / power / Wi-Fi state:
Host model / full OS version / Bluetooth adapter:
Mode / active profiles / layout / accessibility settings:
Steps / expected / observed / local evidence location:
Passes / failures / next action:
```

Keep raw logs and private host details local. Promote only reviewed, portable conclusions under the knowledge contract.
