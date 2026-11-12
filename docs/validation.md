# Validation and known issues

ESP32-INPUT is development firmware. Host regressions, target compilation and
physical compatibility are recorded separately. A send acknowledgement proves
submission at the documented boundary. Observed host behavior requires a
host-side or physical check.

## Verified checkpoints

| Scope | Result |
| --- | --- |
| Controller memory policy, 2026-11-12 | The unchanged pinned VHCI TX and OSAL allocation/free functions reproduced an LE ACL assertion under injected internal DMA exhaustion. Enabling caller-authorized controller PSRAM allocation forwarded and freed the packet once with PSRAM available. Unmarked allocation and real PSRAM failure behavior were preserved. Heap allocation and the MSYS pool were simulated; the fixture did not execute the closed controller archive. Focused Bluetooth regressions, production and instrumented S31 builds, and independent review passed. |
| Windows playback with BLE input, 2026-11-12 | On one ESP-Mosaico/Windows setup, the operator reported normal audible Bluetooth playback, touchpad input and Motion together, without a restart, in a brief test of an instrumented build carrying the production memory policy. Serial observations included active playback, enabled Motion and admitted movement. This is a short functional check, not sustained-load or general Windows compatibility qualification. The Windows version was not recorded. |
| ESP32-S31 source checkpoint `eb8c69c` | Repository hygiene, 209 coordinator cases, 57 media-control cases and 35 Classic scan cases passed. The pinned target built successfully and 695 declared task-stack paths passed the static budget. Peak interrupt and Bluetooth controller stack use, CPU load and device timing remain unqualified. |
| Board boot on `eb8c69c` | Booted and confirmed the expected application on ESP-Mosaico with the v3 OTA layout. The prior confirmed application remained available as fallback. This is evidence for one installation. |
| Earlier Windows input and audio | Prior board tests observed pointer movement and recovered BLE input. Fresh Classic pairing established HFP and A2DP links in an earlier run. Saved audio reconnection failed. A later working interval does not qualify every saved-host recovery path. |
| USB audio, iPhone, iPad and macOS | Source and host tests cover their shared transport contracts. No complete physical compatibility result is claimed for these combinations. |

The controller-policy test used additional local diagnostic instrumentation,
including a retained panic record. Those diagnostics are not part of the
production defaults. Before the policy change, two retained exceptions
independently identified the pinned SDK's LE ACL `assert(om)` after a
packet-header allocation returned NULL. The separately logged first-allocation
failures were retained observations, not proof of the exact failing packet,
request size or failure time. A separate power-on-class reset remained unexplained.
Complete captures and host-specific evidence remain local.

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

## Local reset checks

The local About reset in `16b4463` passed focused confirmation, cancellation,
storage-failure and interrupted-reset checks, including legacy NVS records and
normal startup on an older layout without the optional control partition.
An operator-confirmed device reset returned to the default UI; subsequent
observations showed cleared Wi-Fi connectivity and disconnected BLE input.
This does not establish a complete physical store audit or power-interruption
qualification. See the [user procedure](user-guide.md#reset-saved-user-data)
and [storage boundary](interaction.md#status-connectivity-and-display) for what is retained.

## Open issues

- **Bluetooth memory pressure.** The controller policy removes the internal
  DMA dependency for allocations explicitly eligible for PSRAM. Real PSRAM
  exhaustion or a full MSYS pool can still trigger the SDK assertion. Dynamic
  memory margins, sustained coexistence and controller ISR/cache-off behavior
  remain unqualified beyond the inspected paths and brief device test.
- **Fast Motion response.** On hardware running the horizontal sign fix,
  the tester confirmed correct horizontal and vertical directions. Fast
  movements in all directions still produced noticeable stutter and large
  pointer jumps. This run used BLE input with Classic audio disconnected.
  No active reference loss or active consumer interval over 50 ms was
  observed in that run. Earlier reference-loss observations remain valid.
  The cause of the fast-motion behavior remains open. The later brief
  coexistence check did not systematically repeat the fast-motion cases.
- **Saved Classic audio reconnection.** A fresh pairing from **Audio →
  Manage devices → Add device** has connected on Windows. In a later saved
  Speaker attempt, the ACL link connected and the device supplied a stored
  link key. The connection then entered Secure Simple Pairing. With Add
  device closed, local policy denied the confirmation and authentication
  failed. The reason pairing restarted remains unresolved.
  [Audio Forget](bluetooth-peer-snapshots.md#local-audio-forget) supports one
  selected inactive Classic record with single-block storage and no controller
  key store. Native checks cover retention of same-address BLE state. Physical
  removal, continued BLE input and Windows audio re-pairing remain unverified.
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
