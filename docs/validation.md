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

### Memory-repair source and build

The scoped repair at `41d6b42b65e432724c94af0f15caf2140c12bb99` passed
independent review of 21 changed files. Its approved-file inventory digest is
`530fe4621ed8b5529a369f32283b3b02d8ccb3d5bea6ba4bdaa9733c2c10bb97`.
The complete host suite passed, including 80 coordinator and 23 serial-parser
cases; independent BLE checks covered both 15/32-bond capacities and first-failure
retention. The final linked ESP32-S31 application is 2,423,824 bytes (`0x24fc10`),
SHA-256 `899841a8a4496d52bba8fb537003b8cc2871f78f8264d2083a8f782a8f741e91`.
All first-party firmware source hashes stayed unchanged during that build.
The later focused-UI edits are excluded from this image and source seal.

Generated configuration confirms the intended PSRAM/internal allocation policy.
The target ELF places the fixed allocation-failure record in internal BSS and
its hook in IRAM, using inline lock-free atomic operations without logging or
allocator calls. The original BSP TE/full-frame/internal-bounce path is retained.
These are source/build observations; this repair image has not yet passed
physical display/input or long-duration acceptance.

### Unified API planning review

The eight planning documents published at
`25760ebff776562768dd1221f86b4e261ff58535` passed independent consistency review.
The approved inventory digest is
`964525d29c526bbe84d1154ddc765f2937b30c47f414c2b1d21b34ea4f8d4fbc`.
All 48 current `app_action_t` values have one matrix row, with no omissions or
duplicates; the reviewer checked activation effects and cross-document names,
scopes, units and lifecycles. All eight recorded planning findings are closed.

Repository text/local-link checks and the pinned SDK's candidate partition-table
validation passed. These checks do not implement or qualify authenticated remote
control, recording, policy storage or OTA. Machine-readable schemas, generated
bindings, actual partition migration and device validation remain open gates.

### Focused UI source, native rendering and build

The final compact UI production source hash is
`6b2963cbd567dcf9f58cfbf37891f5580f89dc6230e7c4827384819b91bf098a`.
The stable native validation manifest digest is
`5c5741e38219615ab7eb76822d5e34eb59c2ab04eee838b06c6bda6d815b3468`.
Both visual and resource executables passed under ASan/UBSan; the source/test
inventory stayed unchanged throughout the run. A later interaction-document
correction clarifies the already tested three-fixed-pages/one-panel architecture;
it does not change executable behavior or invalidate that run.

The suite retained all 164 prior scenes and added ten, producing 174 native
480 x 480 PNGs. Representative Mouse, Keyboard, Menu, settings, confirmation,
long-text and error-state renders were visually inspected. All 50 FULL-render
resource cases recorded zero new flushes and zero allocator activity for idle or
hidden-only changes over 100 snapshots / 10 simulated seconds. A real visible
change produced a flush, so zero work is not a disabled-rendering result.
Both themes completed 1,000 panel open/close cycles across 17 variants, with 102
core objects, a peak of 142, and unchanged warmed live bytes/blocks. Teardown
returned all observed allocations. These are host measurements, not target heap
or elapsed powered-time evidence.

The complete host suite passed with 91 coordinator cases and 23 serial-parser
cases. The final pinned ESP32-S31 application is 2,423,488 bytes, SHA-256
`df5f803e04c5ae08ef8c834e4b8464cb140f96b5edf32565f78507ecf080c920`.
The UI/header/coordinator source hashes remained unchanged during the final
build. Earlier compact-UI images are superseded. Subsequent physical evidence is
recorded below; host interoperability remains a separate acceptance gate.

### Focused UI deployment and Motion investigation

The image above was flashed with verification, preserving NVS. The user confirmed
working Menu/Keyboard/Mouse touch navigation and AI double-click. A 90.47-second
capture had nine health samples, stable free internal memory of 72,735 bytes and
largest internal block of 31,744 bytes. No LCD allocation failure or BLE failure
was captured. However, repeated `esp_lv_adapter_lock` errors caused the smoke
result to fail. Source inspection traces the warnings to the coordinator's
zero-wait presentation lock; they do not prove a 100 ms blocking stall. The
planned correction transfers presentation consumption to the LVGL owner task,
without weakening the log parser or increasing lock waits.

A separate 35.03-second capture observed Bluetooth initialized, connected and
ready, with no BLE failure, but again failed on LVGL lock errors. The user still
could not enable Motion after selecting USB. Existing periodic logs omit IMU,
calibration and selected-route readiness, so they cannot establish that cause.
The user's factory-ROM accelerometer observation is separate evidence of prior
sensor operation. Retained startup/stream diagnostics and explicit Motion
availability reasons are required before claiming a fix. These captures are not
elapsed-time endurance or complete host-input qualification.

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
