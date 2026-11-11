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

## Motion checks

The `959aabd` implementation passed 213 coordinator cases, native interaction
and board-adapter regressions, the pinned ESP32-S31 build and 700 declared
task-stack paths. Native fixtures verified cross-axis isolation and no
pointer output from pure roll. The later `d8e0701` change corrected the
default horizontal sign and passed focused direction regressions.

In a native fixture with a 100 Hz producer and a 160 ms coordinator delay,
the older implementation lost its reference. The sample-history implementation
tracked four degrees of attitude from retained frames, emitted no catch-up
movement and resumed on the next live sample. A true sample gap still
invalidates the reference. Interrupt safety and device CPU cost remain
unqualified.

## Open issues

- **Fast Motion response.** On hardware running the horizontal sign fix,
  the tester confirmed correct horizontal and vertical directions. Fast
  movements in all directions still produced noticeable stutter and large
  pointer jumps. This run used BLE input with Classic audio disconnected.
  No active reference loss or active consumer interval over 50 ms was
  observed in that run. Earlier reference-loss observations remain valid.
  The cause of the fast-motion behavior remains open.
- **Saved Classic audio reconnection.** A fresh pairing from **Audio →
  Manage devices → Add device** has connected on Windows. In a later saved
  Speaker attempt, the ACL link connected and the device supplied a stored
  link key. The connection then entered Secure Simple Pairing. With Add
  device closed, local policy denied the confirmation and authentication
  failed. The trace does not explain why pairing restarted. No repair has
  been verified. Removing a Classic record from the audio UI remains
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
