# Compatibility and acceptance

## Current evidence

Evidence date: 2026-10-21. The pinned S31 toolchain has built the minimal composite HID firmware. Portable behavior, report layout and production BLE callback logic have executable host coverage. The physical development board is identified as Mosaico v1.2 (eFuse `0x0102`) with 16 MB flash, and its original flash contents have been backed up locally. Runtime deployment and host compatibility evidence are recorded separately below; a build does not establish a working HID device or long-term reliability.

| Host | Mouse path | Keyboard checks | Voice/audio | Status |
| --- | --- | --- | --- | --- |
| iPhone | AssistiveTouch pointer devices | BLE keyboard; English layout/input mode | HFP/A2DP in actual apps | Pending |
| iPad Pro 2018 | Native pointer and optional AssistiveTouch tested separately | BLE keyboard; software keyboard visibility | HFP/A2DP and HID coexistence | Pending |
| Windows 11 | System Bluetooth mouse | US ANSI and Ctrl shortcuts | HFP input/output and A2DP playback | Pending |
| Windows 10 | Add if an actual test host is available | Same layout checks | Hands-Free/Stereo endpoint behavior | Pending |
| macOS | System Bluetooth mouse | US ANSI and Command shortcuts | HFP input/output and A2DP playback | Pending |

For iPhone, check Settings -> Accessibility -> Touch -> AssistiveTouch -> Devices -> Bluetooth Devices, and enable AssistiveTouch. This is system configuration, not a proprietary client installation. See [Apple's pointer-device instructions](https://support.apple.com/en-us/111775).

## Host checks

`python3 tools/check.py` runs portable state, HID queue and haptic policy tests under strict C11 diagnostics and address/undefined-behavior sanitizers. It also validates the composite report map independently, exercises production BLE callbacks with substitutes at SDK boundaries, and checks the serial-health capture tool using synthetic streams. Coverage includes interruption, stale epochs/generations, release recovery, congestion deadlines, bounded lifetime cycles, motor cancellation races and numerical limits. These checks do not emulate a radio or prove host interoperability. See the [coverage matrix](test-coverage.md) for remaining integration and device cases.

### Historical design baseline: 2026-10-21

- C11 tests passed under Clang with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
- Prototype JavaScript syntax passed. Browser checks covered default mouse mode, motion toggle, motion selection of G, the Alpha -> A candidate/confirmation flow, explicit send interrupted by lock, independent audio exit, concurrency simulation and local takeover.
- Browser connections, audio, speech and sending are simulated. No Bluetooth or microphone permission was requested.
- At this earlier snapshot, only the portable core provided epochs and release gates; transport, queue and device UI had not yet been implemented.
- This historical record contains no target-build, flashing, pairing, latency or acoustic result. Current implementation evidence supersedes that scope below.

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
4. **Keyboard:** two-level nine-cell selection, deletion, space, case, mode changes, full 64-character drafts, repeated sends totalling over 100 characters and cancellation/disconnect/target changes. Old results must not reach a new target.
5. **Spoken letters:** repeated 26-letter/digit/spelling-word trials, confusion matrix and rejection rate, with separate accents/distances/noise conditions. Set thresholds from evidence.
6. **Independent audio:** capture, playback, meeting echo, mute, gain and HFP codec negotiation. Pairing alone is not an audio result.
7. **Concurrency:** same-host HID + HFP and HID + A2DP separately, proposed 30-minute interaction runs, then 8-hour powered soak. Record latency, underruns, disconnects and memory.

These durations are acceptance plans, not completed runs. IMU sample rate or report frequency is not end-to-end latency.

## Input firmware engineering record

- Target: `esp32s31`, experimental ESP-IDF 6.2.0 at `383dda6c182e3dabf6355160166fe007df775629`; BSP at `5f10831ddf8697680121e72ba0fd3dce7f7f1799`. Managed dependencies are fixed in `firmware/dependencies.lock`.
- Tooling is project-local: Python 3.12, official RISC-V compiler, CMake 4.0.3 and Ninja 1.12.1. `tools/idf.py` separates setup/build from deployment.
- The BLE development build uses one connection, encrypted composite mouse/keyboard reports, bounded input queues and release barriers. The coordinator watchdog has panic enabled with a five-second timeout.
- The product LVGL UI follows the original top-target/bottom-mode layout, large touchpad, preview-first keyboard and separate unavailable Audio page. Light and Dark themes cover all pages and overlays. Dark is the initial default; Ctrl/Command and haptic settings are explicit and persistent. Only a complete physical AI hold can unlock.
- Live BMI270 sampling, stationary calibration, wrist motion, touch fine control and local motion selection are implemented. The approved nine-cell grid and 64-character draft use bounded transport batches with accepted-prefix retention. Physical orientation, stationary drift and host outcomes still require device evidence. Audio, speech and remote Wi-Fi input remain unavailable; provisioning does not enable remote control.
- USB CDC provides read-only diagnostics; composite USB HID and transport switching are being integrated under the same USB owner. Health records include uptime, free/minimum heap, largest internal block, coordinator stack watermark, queue occupancy and lock state. Logs may drop when the host does not read; logging must not block HID.
- Bluetooth Device Information/Battery services, cross-host compatibility and long-duration qualification remain release gaps. Local battery/About screens are separate from GATT services. The current profile is a development HID subset, not a Bluetooth qualification claim.

### Current expanded integration

The original product implementation passed a frozen independent v3 review and
target build before the user added device basics, multiple saved peers, a
nine-cell keyboard and USB selection. Those earlier results do not qualify the
expanded tree. Standalone reviewed components are published as completed stages
in [delivery progress](progress.md). The expanded linked candidate, deployment
and actual Bluetooth/USB host tests require fresh results below before delivery.

The expanded baseline passed independent review of 128 first-party code,
configuration and test files. The sorted SHA-256 inventory digest is
`11c200a3d1237b05b3c1a4021a5344dd4b5b0d49e2ce10d431308f26d0a1fe26`.
All ten review findings are closed for that snapshot. The complete host suite
passed with 77 coordinator and 16 settings-store cases, both BLE capacity
configurations, USB lifecycle/identity checks and the existing portable,
board, provisioning and serial suites. Native UI inspection covered 164 cases.

The exact candidate built for ESP32-S31 has application size `0x24da80` bytes
and SHA-256 `522fa60eab1bdcca0e0706da25f81bc29d3e2b51677dd9be12304816285e6ea7`.
Its four-MiB app partition retains 42 percent free. The candidate supports one
shared CDC/HID USB stack with chip-derived serial identity. These are compiled
capabilities; source/build success did not establish a working device. The
expanded deployment failed the device gate described below. Input, pairing,
wake, persistence and endurance still need successful device evidence.
New connection-priority/scanning and OTA requirements are
excluded from this source/build seal and remain separate extension work.

### Runtime evidence

Initial deployment completed on the confirmed Mosaico v1.2. Bootloader,
partition table and application writes passed esptool data verification. Native
runtime USB CDC enumerated after reset. The original 16 MiB backup was verified
again before writing and remains private local evidence.

The first 90.035-second capture observed eight increasing health samples
(uptime 22 through 92 seconds), queue occupancy zero, internal free memory
167,683..167,819 bytes, largest internal block 94,208 bytes and coordinator
stack watermark 2,000 bytes. No reset or runtime error appeared in that captured
window. This was an idle bring-up observation, not a load or endurance result.
The capture **failed** because its deadline split the final health line; the
capture helper was corrected with a fixed one-second grace for an already-started line. Its 21 offline tests preserve malformed/fault/freshness failures.

The host did not discover BLE on this initial image. Investigation found that
the pinned structured advertising API rejects a two-byte UUID input: it requires
full 128-bit UUID representations and compresses Bluetooth-base UUIDs on air.
The correction uses the complete HID service UUID and adds an initialization
boundary regression. Periodic health diagnostics now retain Bluetooth startup
failure visibility after late USB attachment. Corrected advertising is included in the product UI replacement; deployment and actual host-input qualification are recorded separately when performed.

Original-firmware backup, build logs, serial captures and identifying details
remain ignored local evidence. Never commit the factory backup because it can
contain credentials or pairing data.

### Expanded baseline: failed device gate

Commit `f5e724b9b0a5b7cfd36e636d1f1ee8ab4f09a3c2`, application hash
`522fa60eab1bdcca0e0706da25f81bc29d3e2b51677dd9be12304816285e6ea7`,
was written and verified successfully. Runtime USB enumerated. The user observed
the expected dark layout and nine-cell keyboard, but touches produced vibration
without visible button transitions, with residual content near the top.

The requested 90-second serial capture ended after 46.918 seconds with a serial
I/O error. Five health samples covered uptime 22 through 62 seconds. Internal
free memory ranged from 2,991 to 12,435 bytes; the largest internal block ranged
from 864 to 8,192 bytes, despite roughly 14 MB of total free heap. Repeated LCD
SPI transfer and LVGL draw failures reported `ESP_ERR_NO_MEM`, followed by
display-lock and battery telemetry timeouts. Bluetooth initialization had
returned success, but the later transport failure flag remained set. The
initial failure event was outside the captured window. A reset was not proven.

This is a **failed physical acceptance gate**. Failed redraws are a supported
explanation for the unchanged screen and residual pixels, not proof that every
touch path is correct. The repair must preserve internal/DMA memory, capture
startup allocation stages, and retest real display transitions and Bluetooth
readiness before attributing success to a layout change. Native render tests
cannot establish target memory sufficiency or display transfer completion.

### Long-term power gate

The test plan requires 24 hours of powered operation followed by 72 hours after failure cases pass. Cover host sleep/reconnect storms, no USB reader, queue pressure, motor cancellation, storage wear, power interruptions, watchdog recovery and comparable memory checkpoints. Record temperature and power conditions. A short smoke run and 100,000 simulated host cycles are useful evidence but do not satisfy this gate; periodic reboot is not an acceptable substitute for fixing resource growth.

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
