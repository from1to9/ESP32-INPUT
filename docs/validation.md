# Validation and known issues

ESP32-INPUT is development firmware. Host regressions, target compilation and
physical compatibility are recorded separately. A send acknowledgement proves
submission at the documented boundary. Observed host behavior requires a
host-side or physical check.

## Verified checkpoints

| Scope | Result |
| --- | --- |
| ESP32-S31 source checkpoint `eb8c69c` | Repository hygiene, 209 coordinator cases, 57 media-control cases and 35 Classic scan cases passed. The pinned target built successfully and 695 declared task-stack paths passed the static budget. Peak interrupt and Bluetooth controller stack use, CPU load and device timing remain unqualified. |
| Board boot on `eb8c69c` | Booted and confirmed the expected application on ESP-Mosaico with the v3 OTA layout. The prior confirmed application remained available as fallback. This is evidence for one installation. |
| Windows input and audio | Prior board tests observed pointer movement and recovered BLE input. Fresh Classic pairing established HFP and A2DP links in an earlier run. Saved audio reconnection failed. Link state does not establish audio quality. |
| USB audio, iPhone, iPad and macOS | Source and host tests cover their shared transport contracts. No complete physical compatibility result is claimed for these combinations. |

## Motion candidate

Native checks for `959aabd` passed 213 coordinator cases, interaction and
board-adapter regressions, the pinned ESP32-S31 build and 700 declared
task-stack paths. In one controlled trajectory, a rightward input produced
13 horizontal HID units and 9 unintended vertical units on the baseline.
The vertical sign depended on the preceding turn direction. The candidate
produced 7 horizontal and zero vertical units in both runs. A pure-roll trajectory changed from 1,336
accumulated absolute HID units to zero. These are report units from host
fixtures.

In a separate fixture with a 100 Hz producer and a 160 ms coordinator delay,
the baseline lost its reference. The candidate tracked four degrees of
attitude change from retained samples, emitted no catch-up movement and
resumed on the next live sample. A real sample gap still invalidates the
reference. Device feel, host delivery, runtime CPU cost and interrupt
safety remain unverified for this candidate.

## Open issues

- **Motion aiming.** Earlier runs showed reference loss during active aiming
  and gaps at the consumer while the sensor worker remained active. A later
  check on the same firmware saw no reference loss over approximately 20 seconds
  without Classic audio. The tester still reported obvious vertical movement
  during horizontal aiming and excessive sensitivity. Motion acceptance remains
  open. The test did not establish the cause of the earlier gaps.
- **Saved Classic audio reconnection.** Connecting a saved Mic or Speaker peer
  can fail while the pairing window is closed. A fresh pairing from **Audio →
  Manage devices → Add device** has connected on Windows. Repeating a saved
  connection after fresh pairing still failed in the recorded run. Leave other
  saved pairings in place. Removing a Classic record from the audio UI remains
  unavailable.
- **Audio quality.** Intermittent Bluetooth playback stutter has been observed.
  USB Speaker start, Mic restart under concurrent Speaker playback, and
  sustained duplex operation need physical qualification. An active link or a
  passing codec test does not establish audible output.
- **Sensor export.** The transport reports data lost to retention gaps.
  Sustained 100 Hz capture without loss has yet to be demonstrated.
- **Host and power coverage.** Input timing, Bluetooth coexistence, extended
  power and resource use, update interruption and recovery still need repeatable
  qualification on representative hardware.

## Hardware qualification stages

Follow [test coverage](test-coverage.md) for reproducible source checks and
device cases. Record the source and build identity, board and host versions,
transport, relevant settings and observed result for each case. Verify input
with a disposable application, Mic with a host recording, Speaker by listening
to actual output, and update/recovery with the expected running image and
fallback. Keep private evidence and complete captures in an ignored workspace.
