# Compatibility and acceptance

## Current combined delivery boundary

The current combined source includes HID, motion/touch, spoken letters, audio,
recording, authenticated LAN control/MCP, native connection management and signed
OTA staging. The source checkpoint follows the
[combined checklist](delivery-checklist.md). Its separately authorized device
attempt is recorded below. Historical observations belong to their named images
and must not be used as evidence for a different combined image.

The latest reviewed source incorporates the repairs recorded below. Source
synchronization is authorized separately from device acceptance: the retained
startup IMU fault remains unresolved and the latest device gate remains failed.
This checkpoint is not a qualified release. Reusable findings are indexed in
the [engineering lessons](../.agent/shared/ENGINEERING_LESSONS.md).

### Combined source checkpoint: 2026-10-21

| Check | Current evidence |
| --- | --- |
| Complete host regression | `python3 tools/check.py` passed, including 146 actual coordinator cases, protocol/generated-artifact checks, threaded media composition, server/authentication, connection ownership, settings/storage and updater failure paths |
| Companion/MCP | Five tests passed using the actual companion, stdio MCP and signing code with named external boundaries; independent review also passed four offline real-TLS cases and 54 C/Python format comparisons |
| Production UI | All native suites passed on stable source: 748 renders, all 174 legacy scenario names retained, 100 settled-resource cases and repeated navigation across both themes; teardown retained zero tracked allocations |
| Independent review | Component and integration findings were repaired and independently retested. Publication requires the exact whole-tree review inventory to match the final files; detailed private reports remain local |
| Hygiene | First-party text/local-link checks passed for 484 files; diff whitespace passed. Publication excludes private recovery images, credentials, recordings, generated binaries and local evidence |
| Target build | Full ESP32-S31 firmware build and size analysis passed with the pinned SDK, BSP and resolved dependencies; this did not enumerate or access a device |

The built application is **4,655,312 bytes** (`0x4708d0`), leaving 26 percent of
its six-MiB slot free. Its SHA-256 is
`601827c21e672e5dcba965ee4e8ea7d65a895564f73e4dfa83d8441f18344b24`.
The generated configuration SHA-256 is
`c55cc206f4a9e1e86f91934b4d120faca948d042e849c1f79ac161fa47c6c80c`.
SDK commit `383dda6c182e3dabf6355160166fe007df775629`, BSP commit
`5f10831ddf8697680121e72ba0fd3dce7f7f1799`, the exact reviewed pairing patch and
`firmware/dependencies.lock` identify the dependencies. The generated SDK and
actual X.509 compilation enable certificate date validation. Rollback uses
application-controlled confirmation, with startup auto-confirm disabled.

Static DIRAM use is 301,378 of 503,744 bytes, leaving 202,366 bytes before runtime
allocations. This is linker evidence, not a measured free-heap or endurance
result. The bounded native review pages and presentation buffers use external
RAM; their successful host allocation checks do not qualify physical PSRAM.

The operation catalog has 147 entries and 77 configuration metadata fields;
16 native preference keys are currently writable through the shared path.
Catalog presence does not imply runtime support. Initial-v1 event subscriptions,
playback pause/resume/seek, recording pause/resume, destination export and target
screen observation remain explicitly unsupported. Finalized recording download
uses the authenticated binary HTTP resource. Spoken input is the confirmed
letter-candidate workflow, not general dictation. Actual recording uses explicitly
prepared encrypted NOR storage; unknown external NAND remains preserved.

No flashing, USB/serial enumeration, pairing, recording on hardware, radio test
or device test was performed for this checkpoint. Host interoperability, acoustic
quality, physical motion/touch behavior, partition migration, power-loss recovery
and long powered operation remain separate qualification gates. The v3 layout
needs a separately authorized migration on an earlier-layout device; ordinary
OTA cannot perform that migration.

### Cabled v3 migration and failed startup: 2026-10-22

The user authorized flashing and explicitly waived a new backup. The existing
recovery image was retained. The connected target identified as ESP32-S31 rev0
with 16 MiB NOR, secure boot disabled and flash encryption disabled. Its installed
table contained NVS at `0x9000`, PHY at `0xf000` and a four-MiB factory slot at
`0x10000`. The entire new 64-KiB control region was verified erased before boot.

The five frozen artifacts from source commit
`1b36bc0811cee117612952998f871427b2ac4cb6` were written and individually verified,
with the partition table last. Readback confirmed NVS/PHY and control unchanged
before reset. No whole-chip erase, external NAND access or eFuse write occurred.
The first bootstrap marks OTA_0 valid directly; this is not application health
probation or proof of a working fallback image.

On macOS 27.0.1 (26A434), the device enumerated as USB HID plus one-channel USB
audio input/output at 48 kHz. This proves enumeration only. The user observed
the application boot screen at `Loading preferences`, followed by repeated
restarts. The first serial capture failed
after 2.273 seconds with an I/O error, zero bytes and no health samples. A later
bounded reconnect attempt observed no runtime CDC interface. Input, acoustic
audio, connectivity, UI behavior and powered endurance have not passed this gate.

Exact target compilation identified an independent stack-budget defect:
`app_main` reserves 5,472 bytes and its `render_view` child reserves 2,720 bytes,
exhausting the configured 8,192-byte main stack before caller or deeper callee
frames. No panic trace was captured, so this evidence does not alone attribute
the observed reset. Preserve this failed image and evidence when validating the
repair; a later source or build pass cannot replace a successful device run.

### Startup and coordinator stack repair: 2026-10-22

The repair separates startup from the non-returning coordinator loop and moves
large copied observations and requests into bounded owner workspaces. BLE and
Classic bond readers provide direct copy-out APIs. Media and connection polling
keep incoming observations separate from retained state, and transient private
payloads are cleared after consumption. Remote device execution uses a
per-instance workspace with callback-reentry rejection; accepted jobs and staged
transactions retain independent copies. BLE target submission validates the
complete request before replacing the worker-owned candidate under its lock.

Independent review found a second real overflow in remote BLE target selection:
the previous known nested frames exceeded 8,192 bytes before SDK overhead.
The repair also removes the large target-request and candidate stack copies.
The stack-budget checker covers the named callback chains, compiler split bodies
and nested helper paths, with a separate 2-KiB SDK/library/interrupt reserve.
Its declared-path result is not an exhaustive whole-program or runtime bound.
The configured main stack remains 8,192 bytes; increasing it is not the repair.

The final candidate passed the complete host regression, including 147 coordinator
cases and the added request-lifetime, callback-reentry, copy-out, stale-state and
failure-preservation cases under sanitizers. The 748 native UI renders remained
pixel-identical to the approved reference. The expanded target budget passed
28 declared paths: remote BLE target submission uses 4,000 known bytes, and the
largest declared path uses 5,024 plus the 2,048-byte reserve, leaving 1,120 bytes
beyond that reserve. The startup frame is 320 bytes. These are compiler results,
not observed device watermarks.

The full pinned ESP32-S31 build and size check passed with no drift across the
417 captured firmware/component/test files. The application is 4,654,752 bytes,
SHA-256 `662df63b385ed62bdabab7224965724bd91e4f093b633663daa0ddba15856d99`.
The SDK, BSP, reviewed pairing patch and configuration match the checkpoint
above. Static DIRAM use is 302,722 of 503,744 bytes, leaving 201,022 before runtime
allocations; linked coordinator and UI observation workspaces reside in external
RAM. Repository text/link and diff-whitespace checks passed.

The app-only repair procedure verifies the frozen application and installed v3
table before writing OTA_0, then verifies the application and unchanged NVS/PHY
and table before reset. It does not perform a new backup. The separately retained
failed-image evidence above remains part of the device acceptance record.

The application-only candidate was written and verified against that hash;
NVS/PHY and the installed table remained unchanged. A completed 90.005-second
capture contained eight health samples with increasing uptime from 12 to 83
seconds and a minimum observed main-stack watermark of 4,172 bytes. No reset was
observed in that capture. The health gate nevertheless **failed**: display SPI
transfers reported `ESP_ERR_NO_MEM`, Wi-Fi worker startup failed, and I2S DMA
allocation and media initialization failed. Runtime internal free memory was
4,083 bytes; DMA-capable free memory was 167 bytes with a largest block of 40.
The first recorded allocation failure requested 8,190 internal DMA bytes.
The user saw the product interface with the boot logo still over it. USB audio
input/output enumeration at 48 kHz did not establish working acoustic audio.

An IMU `batch_time` timeout stopped the stream after 16 samples and prevented
calibration. Its cause is not established by the memory failure and requires
separate verification. This candidate is not device-qualified; publication is
held while the physical refresh and resource failures are investigated.

### Display and combined resource repair candidate: 2026-10-22

The resource repair bounds the board display queue without changing its full
viewport, TE synchronization or completion callback. Audited task-only UI,
coordinator, subscription and audio scratch buffers move to fixed external BSS.
Supported I2S payloads and TLS allocations use PSRAM, while selected flash-free
network/clock tasks use external stacks. Flash/NVS/OTA owners, locks, TCBs and
interrupt data retain internal memory. The [runtime memory plan](../design/runtime-memory.md)
records capacities, cleanup costs and the remaining coexistence qualifications.

A separate IMU correction discards only finite, monotonically timed batches
that exceed 20 ms while the prior accepted sample still meets the original
50 ms freshness limit. It never retimestamps or publishes the discarded batch.
Bus errors, clock rollback, expired samples and raced consumer faults still
stop the stream. Duplicate timestamps do not advance calibration or emit HID.
Rejected-batch counts remain observable.

Fresh configuration from tracked defaults and the full pinned build passed.
The 4,649,952-byte application SHA-256 is
`7cbe4c5a013dd82112a7b76a3caa9a929e57e27e0de2c43776e6f6bb407cc2b5`;
configuration SHA-256 is
`f927e2539734a1b8e969efb45bab19328cd5e63b6695792a42cbac40bfef51bc`.
The installed partition table and model image are unchanged. The full host
regression passed without drift across 459 captured firmware, component, test
and tool inputs. All 748 UI images match the approved baseline.

Final linked static DIRAM use is 236,324 of 503,744 bytes, down 66,398 from the
preceding stack-repair image. External BSS is 285,688 bytes. The optional Wi-Fi
speed sections are no longer in internal IRAM, and the audited buffer symbols
are in external BSS; queue control and locks remain internal. These figures do
not establish measured heap/DMA headroom. The initial target link correctly
failed because the wrapper was not extracted from its static archive; an
explicit symbol anchor fixed that integration issue before this successful
build.

The exact application was subsequently written and verified; the installed
table and NVS/PHY remained unchanged, and no new backup was performed. The
90.010-second capture contained nine health samples with uptime increasing from
23 to 103 seconds, and a minimum main-stack watermark of 4,176 bytes. Bluetooth
was initialized, connected and ready. IMU samples increased from 1,824 to 9,101
with completed 64-sample calibration and two retained late batches. No reset,
display allocation error or I2S error was observed in that captured window.
The user confirmed that the boot logo disappears completely, Menu and mode
switching and touch work, and no restart is observed. This establishes those
physical display/navigation observations, not host pointer or audio behavior.

The device gate nevertheless **failed**. Intermittent battery telemetry
`ESP_ERR_TIMEOUT` warnings remain, and the new runtime diagnostics expose invalid
audio state/error values (`0xa5a5a5a5`) and an invalid off-confirmed value (`0xa5`)
despite the snapshot reporting known audio state. Audio readiness cannot be
inferred from initialization or USB enumeration. Wi-Fi initialized in idle
station mode; no LAN join or server session was tested. Steady internal free
memory was 20,847 bytes with a largest block of 10,752, and DMA free memory was
11,415 bytes with the same largest block. This remains below the existing Wi-Fi
scan admission limits. The user also observed a failed Wi-Fi scan. The memory
gate is consistent with that result, although a scan-specific driver error was
not captured. These observations require further owning-layer repair
and coexistence validation; this image is not device-qualified and publication
remains held.

### Audio, battery and scan-resource follow-up: 2026-10-22

The audio repair checks initialization before publishing any engine snapshot.
Explicit alignment queries and aligned allocations correct a source-proven
contract mismatch: the pinned generic PSRAM allocator can return four-byte
alignment, whereas the linked audio engine requires eight. This explains a
possible rejected initialization, followed by the previously observed stack
poison publication; the original device allocation address was not captured.
Matching engine/store, remote adapter/bridge and network object allocations
are corrected at their owning boundaries. Weak-alignment regressions exercise
rejection, correct allocation, failure cleanup and retry. Late completion
notifications no longer borrow released audio state, and task-handle publication
uses the same owner lock as readers. Valid hardware wait/fault recovery remains.

Battery reads now follow the BQ27220 documented incremental-read and timing
requirements, with three bounded packets instead of six. The original 20-ms
whole-batch limit, 1-Hz polling and five-second freshness remain. Read, batch and
clock failures retain truthful diagnostics; no automatic retry or hidden stale
success is added. Native regressions cover legal clock stretching, partial
failure, overdue batches and recovery. Physical timeout recovery remains open.

The audited board audio and media control tasks use two external 6-KiB stacks
on the supported profile. The Wi-Fi static TX pool is reduced from eight to
four; existing scan admission limits and transport backpressure are unchanged.
The [memory plan](../design/runtime-memory.md) records the projected benefit and
the remaining combined-operation peaks. A projection is not device headroom.

Fresh configuration, full target build and host regression passed without drift
across 467 captured source/test/tool inputs. The regression includes 149
coordinator cases and 35 serial-parser tests, which independently reject the
observed poisoned runtime fields while retaining earlier error detection.
The target stack budget passed all 31 declared paths with a maximum of 5,024
known bytes plus the existing 2,048-byte reserve against an 8,192-byte stack.
Linked static DIRAM use is 236,332 of 503,744 bytes; this excludes runtime costs.

The application is 4,650,416 bytes, SHA-256
`bda4e94e0203b44f7d5e006a30a445b01ff37c4ccae9fc57d56ae33431d24641`;
configuration SHA-256 is
`39b6b39f809b5ad47714a5e159086f1c405481d847b02c94ac09642793d4377c`.
The partition table and model image remain byte-identical. Independent review
approved the exact 139-file source inventory and application-only device test.
The application was written and verified, preserving NVS/PHY and the installed
table without a new backup.

The completed 90.002-second capture contained nine health samples with uptime
increasing from 23 to 103 seconds and a main-stack watermark of 4,176 bytes.
All nine media observations were valid: audio OFF/OK with hardware-off confirmed,
and speech IDLE/OK. USB audio input/output enumerated at 48 kHz. These results
resolve the invalid snapshot observation; they do not establish acoustic audio
or recognition. The user confirmed that Wi-Fi scanning lists networks and that
the device successfully joins an existing LAN. The final two Wi-Fi observations
also reported CONNECTED with LAN ready. The control service remained stopped
with no provisioned trust; LAN connectivity does not imply API readiness.

The device gate nevertheless **failed**. All nine Motion observations retained
only 20 accepted samples, with no new samples throughout the capture and no
calibration progress. The repeated first-fault record identifies one earlier
`batch_time` timeout at uptime 3,148,860 microseconds, not nine separate faults.
Source inspection shows that a late batch with an expired cache permanently
stops and cleans up the sampler. The user independently reported Motion
unavailable. Two battery warnings were whole-batch timeouts of 38,553 and
23,694 microseconds after successful packets, not packet-read timeout errors.
The source of those scheduling or bus delays remains under investigation.

Observed free internal memory ranged from 37,407 to 43,423 bytes, with a largest
block of 25,600; DMA free memory ranged from 11,719 to 17,735, with largest blocks
of 8,192 to 16,384. Because the IMU worker had stopped, these measurements cannot
qualify simultaneous healthy sensor, radio and audio operation. Publication is
held for sampler recovery and timing repair. Physical pointer behavior, active
acoustic paths, authenticated services and powered endurance remain unverified.

### Sensor recovery and unified keyboard candidate: 2026-10-22

The next candidate replaces permanent sampler shutdown on transient acquisition
or freshness failure with bounded recovery. IMU and battery reads share one
worker, with IMU first and missed periods skipped. A single Bosch frame supplies
readiness and both sensor vectors. Clock rollback, invalid configuration and
other structural faults remain terminal. The 20-ms batch and 50-ms freshness
limits are unchanged; stale or rejected frames never advance accepted samples.
Recovery requires fresh data, resets the motion reference and does not silently
re-enable the user's Motion switch.

Keyboard now exposes a persistent Keys entry in every submode. Its aligned panel
provides Copy, Paste, Undo, Backspace, forward Delete, Enter, Esc, Tab, Home, End
and four arrows. Copy/Paste/Undo use the selected Ctrl or Command host convention
and send complete key-down/release pairs without changing the local draft.
Submode, Keys and Menu captions share the same 16-pixel font and baseline in both themes.

The full host logic/protocol run passed, including 151 coordinator cases and
54 health-parser cases. The native LVGL run passed 768 renders,
retained all 174 original scenes, and checked 100 resource cases. Each theme
completed 1,000 panel cycles without retained growth; core/peak object counts
were 106/148 under the unchanged 160-object ceiling. Unchanged frames caused no
extra refreshes, and final teardown retained no tracked allocations.

The target application is 4,651,040 bytes with SHA-256
`04a8ec591cb24b4373047ed2beb93ae445736095b34d3e3497700e97a35a2007`.
The complete build, size and coordinator stack-budget checks passed with 148
unchanged target inputs. The sensor retains a 4-KiB internal stack; its measured
944-byte call chain plus engineering reserve is a source budget, not a complete
SDK bound or measured sensor-stack watermark. See the
[runtime memory plan](../design/runtime-memory.md#persistent-sensor-worker).

Independent review approved the exact 149-file inventory and application for
app-only device testing. The application was flashed and its digest verified;
NVS/PHY and the installed partition table were preserved, with no new backup.
The runtime USB device re-enumerated.

The completed 90.001-second capture still **failed**, retaining one startup IMU
fault at uptime 3,103,083 microseconds. The maximum acquisition batch was
69,358 microseconds. The capture began later, so it cannot identify which startup
operation caused that delay. The sticky fault must not be cleared or treated as
a passing gate merely because sampling recovered.

Recovery was observed: all nine Motion records reported an active, live worker,
available IMU and `ESP_OK`; accepted samples increased from 2,070 to 10,072 with
no stall or regression. Sample ages were 1,950..9,469 microseconds and calibration
had completed 64 samples. The recovery count remained one and late-batch count
two throughout the window. Current batches were 1,162..1,223 microseconds, with
zero reported schedule lateness. No new battery warning was captured; this does
not establish that battery reads never fail.

All nine Wi-Fi records reported CONNECTED/LAN ready. Bluetooth remained connected
and ready; audio remained OFF/OK and speech IDLE/OK. Internal free memory was
38,791 bytes with a 25,600-byte largest block while the sampler was alive; DMA
free memory was 12,971 with a 9,216-byte largest block. The coordinator stack
watermark was 4,112 bytes; sensor stack watermark remains unmeasured. Main uptime
increased from 23 to 103 seconds. These observations do not qualify connected
scanning, physical pointer/host keys or active audio.

Device acceptance remains failed because of the retained startup fault, with
physical checks still outstanding. Source publication does not change that
result. Qualification explicitly requires Motion records, a live worker,
increasing samples and sample ages within 50 ms. Legacy logs without Motion
records remain readable but cannot satisfy this candidate's device gate.
Previous failed captures remain failed.

### Repeat device test and physical failures: 2026-10-23

The same `04a8ec59...` application was reflashed after all 148 target inputs,
467 host inputs and five frozen artifacts were verified against the reviewed
package. The application digest, partition table and NVS/PHY preservation were
verified again, without a new backup. macOS 27.0.1 registered the USB keyboard
and mouse drivers and one input/one output audio interface at 48 kHz. This is
interface evidence, not host-key delivery or acoustic qualification.

The immediate 90.005-second capture contained eight health observations and
still **failed** the unchanged error-log gate. Its two error lines came from
GPIO ISR service installation after the TE display path had already installed
that service. The pinned touch and button callers explicitly accept
`ESP_ERR_INVALID_STATE` and continue registering their handlers. Those lines
alone do not establish failed touch hardware, but the original failed capture
is retained without filtering or changing the acceptance criteria.

All eight Motion records were ACTIVE, available and calibrated, with a live
worker and accepted samples increasing from 1,069 to 8,073. Sample ages were
511..9,138 microseconds. There was no latched first fault or recovery in this
boot, but one IMU batch exceeded 20 ms (maximum 24,589 microseconds) and was
discarded. The previous accepted cache remained within its 50-ms budget, so
continued ACTIVE state is consistent with this rejected batch. Wi-Fi and
Bluetooth remained connected. The earlier boot's 69,358-microsecond delay and
its cause remain unresolved.

Physical feedback nevertheless **failed** the product behavior: Motion initially
moved abnormally and stopped after approximately two seconds while its switch
remained on. The touchpad could still move the same host's pointer. Wi-Fi Scan
reported Failed on one attempt and displayed a refreshed network list on a
later attempt with unchanged firmware. This establishes intermittent failure,
not a permanent parameter rejection or a repaired scan path. The user also
found the Keys panel's explanatory caption confusing; host-key reception was
not confirmed.

A subsequent capture was interrupted by a user-confirmed unplug or manual
restart after 15.562 seconds; it is incomplete, not evidence of a spontaneous
reset. After reconnection, a 45.001-second capture passed its bounded serial
checks with five fresh Motion observations. Neither that pass nor sensor
availability clears the physical Motion and scan failures. Investigation must
distinguish acquisition, consumed-sample timing, reference validity, pause
conditions, generated motion and HID admission/delivery. Exact scan failure
phase and error were not available in these logs.

## Historical evidence

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

### Historical expanded integration

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

### IMU startup diagnosis and configuration reproducibility

Diagnostic source `db0d24fbf9d4dbf97a52fe6ae58483c8b4ee2f3d` was built in an
isolated checkout with freshly generated configuration and flashed with hash
verification, preserving NVS. The 2,425,376-byte application SHA-256 is
`ae7adf59c0b58bb64a829224718bcfdd428c02545398ed1e705044d30aa09baa`.
A 36.001-second capture contained three complete health samples and failed
on retained errors plus an incomplete final health line; it is not a passing
runtime smoke test.

The first IMU fault was `first_sample / ESP_ERR_TIMEOUT`, with zero accepted
samples and status `0x90`: accelerometer and command ready, gyro not ready.
The selected input was connected and ready. This establishes a sensor startup
readiness failure rather than a Bluetooth admission failure; it does not prove
a defective sensor. The startup correction permits 100 ms for the first valid
sample, with deadline checks before and after acquisition. Runtime freshness
remains 50 ms and the acquisition batch limit remains 20 ms. Regressions cover
delayed gyro readiness, permanent absence and late batch completion. Physical
recovery remains unverified until the corrected image is deployed.

The same diagnostic capture retained an 8,190-byte internal DMA allocation
failure. Its freshly generated configuration used 16 Wi-Fi static RX buffers
and a receive Block Ack window of 16, whereas the earlier local configuration
used two each. Those limits had not been retained in tracked defaults. The
profile now records 2/2 explicitly and bounds TCP out-of-order retention to
four pbufs per connection. Fresh configuration generation and a new device
capture are required; earlier memory observations do not qualify this image.
The diagnostic image also includes the 50-percent fallback brightness change,
but physical brightness and preserved settings have not been measured.

### Corrected IMU deployment and remaining pointer qualification

The startup/memory correction was published at
`892805a4c37777ee22518de4517bf6974ed22222`. Its freshly configured, reviewed
2,425,632-byte application SHA-256 is
`4996587fad68e21ddc5075010d921c5e9ea8d896b45ef19035cee828c354b1f3`.
Flash verification passed and NVS was preserved. The 46.005-second capture
observed IMU samples increasing from 928 to 3,675, `ESP_OK`, and completed
64-sample calibration by uptime 22 seconds. No retained IMU, allocation or
Bluetooth fault was captured. Complete health samples reported 66,591 bytes
free internal RAM and a largest DMA block of 25,600 bytes.

That capture remains failed: its final health line is incomplete
(`malformed_health`, `unterminated_line`). A subsequent attempted capture found
no USB serial interface and collected no samples. Neither result is a passing
health or endurance gate.

The user confirmed that Motion can enable and produce pointer movement, but
movement is too slow and directions are unintuitive; touchpad movement works
normally on the same connection. Sensor startup recovery is established, while
air-mouse behavior is not accepted. The requested next refinement is automatic
adaptation between screen-up and tilted screen-facing grips, with configurable
speed and directions. The fixed-axis mapping in this image has not passed that
requirement.

### Native station setup and input controls candidate

The integrated candidate provides on-device Wi-Fi scan/select/password/join,
saved-network reconnect, explicit common host keys, touchpad tap/double-tap/drag,
a persistent tap-to-click preference and verified asynchronous Bluetooth-name
updates. Normal Wi-Fi setup joins the existing LAN; a recovery hotspot is an
explicit secondary action. A LAN address does not imply an implemented web/API,
MCP or OTA service. Those services remain unavailable/planned.

The native UI run passed 276 rendered scenes in both themes, preserving all
174 earlier scenarios. Sixty unchanged/hidden-state cases produced no redraw
or allocation; 1,000 panel cycles per theme retained no growth. Core/peak object
counts were 102/144 under the unchanged 160-object limit. Final teardown retained
zero live blocks. The run checked private draft wiping, consumed sessions,
generation-bound network rows, common keys and touch contact heartbeats using
the production LVGL UI. These are host observations, not device memory proof.

The expanded coordinator suite passed 108 cases, including actual HID down/up
sequences, queued swipe rejection, drag cancellation, local editor FIFO and
secret cleanup, acknowledged name persistence and the two added navigation
routes. The presenter module passed eight bounded-publication tests. Actual
LAN join/IP acquisition, on-air name changes and physical tap/drag/common-key
interoperability remain device gates for this candidate.

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
