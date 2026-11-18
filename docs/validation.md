# Validation and known issues

ESP32-INPUT is development firmware. Host regressions, target compilation and
physical compatibility are recorded separately. A send acknowledgement proves
submission at the documented boundary. Observed host behavior requires a
host-side or physical check.

## Passport ESP32-C3 checkpoint

The Passport firmware combines local three-button BLE HID,
Wi-Fi station and fallback AP, board-hosted Web and HTTP/WebSocket input,
connection management, display labels, and optional speaker cues. The pinned
ESP32-C3 build passes at the current integration checkpoint. Host regressions
exercise the shared input engine and HID report path, BLE and Wi-Fi owners,
HTTP ingress, management transitions, lock and release behavior. Actual
Chromium renders and interaction checks cover desktop, phone and tablet
viewports in Dark and Light themes; host LVGL renders cover the 240 × 320
device interface in both themes. The browser tests do not establish Safari
or physical iPhone/iPad behavior.

On one physical Passport, prior candidate builds paired with an iPad and
delivered local mouse movement and Enter; the saved Bluetooth host later
reconnected automatically. The current integrated USB-control build was flashed
as an application with NVS preserved and hash verification. In one USB session,
switching from station to Passport AP returned a terminal applied result;
same-boot status showed AP, DHCP and BLE synchronized. A later build with the
same Wi-Fi resource profile reported complete USB status through connection
phases and served LAN `/health` with HTTP 200 on an unattended boot while
marking all three physical button observations pending. Its bounded LAN checks
included eight sequential requests, two concurrent requests and one follow-up,
all complete. A later LAN test on `beb3c1b` reproduced a controller page
stall before the browser opened a control session. The client received the
HTTP 200 headers for the home page and timed out after 12 seconds with zero
of 8,670 body bytes. The stylesheet also closed before its body arrived.
In a separate capture, a 704-byte health response completed in 0.19 seconds.
During the subsequent home-page request the server reported send errno 11
and a handler failure while the client timed out waiting for headers.
In `a2f6178`, the health handler, HTTP interface check and BLE owner each
replaced a 1,424-byte scan/profile snapshot with 48 bytes of link state.
Matched C3 builds measured 4,128 bytes less static DRAM, but a cold browser
load still stalled on that image. During a browser stall on an instrumented
build, full-size TCP segments repeatedly returned `ESP_ERR_NO_MEM` from the
Wi-Fi transmit call. The driver's transmit-buffer heap-failure count rose
while its quota-failure count stayed at zero.

The subsequent C3 configuration moved selected task-context FreeRTOS
functions into flash, freeing 13,164 bytes of instruction SRAM on the measured
build. On the physical Passport running `b3694476`, a desktop Chromium
session loaded the home page, stylesheet and script byte-for-byte and opened
a control session. Two independent Chromium contexts also fetched all three
assets concurrently, and a desktop refresh returned all three again. In a
separate boot, USB reported the device locked and disconnected. USB unlock
and selection of the saved peer restored the encrypted keyboard and mouse
subscriptions. With BLE ready, the same browser checks passed, two opposing
pointer moves returned terminal HID transport receipts, and Stop completed
the neutral release. The saved connection was ready at the end of that boot.

On the same board, a Chromium touch gesture on `b3694476` returned the page
to idle 0.32 seconds after acquisition. The first move was rejected as
expired, and two subsequent device reads still showed the original lease.
On `68c79e0d`, Chromium loaded the controller script from the installed
firmware. After one acquisition, two short opposing swipes and a sustained
swipe produced 11 terminal movement sends and eight expired rejections.
Control remained active across the rejections and 11 successful renewals.
Both arrow key taps and both wheel directions reached the BLE transport.
Both mode changes applied, and Stop completed the neutral release. The saved
BLE target remained connected and input-ready afterward. Expired samples
still reduced delivered motion. Cursor travel and touch feel on the paired
host were unobserved.

On `0f725f53`, the Passport returned successful relative movements in the
original HTTP response once the HID transport had accepted the report. On a
360-point Chromium touch stroke lasting 8.31 seconds, the input travelled
1,080 horizontal units and ended 72 units to the right. The board returned
26 direct `sent` receipts totalling 234 units of travel and net +186.
Seventeen moves expired. The same lease survived five renewals, and Stop
completed the neutral release. A diagnostic image with STA modem sleep
disabled delivered 192 units and net +72 on one run of the same stroke.
Short right and left strokes delivered more movement on that image. These
mixed results did not justify changing the default Wi-Fi power policy.

With the normal image, a touch beginning inside the pad continued after
Chromium released pointer capture. The browser observed 30 moves targeting
elements outside the pad and a lift outside. Four moves submitted more than
one sample lifetime after crossing the edge reached the HID transport.

On `b1a1442f`, the installed page also delivered movement from half-pixel
touch increments. In a browser fixture, 48 increments in each direction
produced the full +24 and −24 units. A delayed reply to an earlier move did
not change how opposing fractions cancelled. On the physical Passport,
Chromium delivered all 48 pointer events per direction to the page. Eight
rightward moves reached the BLE transport for +8 units. Four leftward moves
reached it for −5 units and two expired. Stop completed the neutral release,
the browser session closed, and the saved BLE connection remained ready.
The remaining loss of travel under network timing and pointer feel on the
receiving device remain open.

These bounded observations do not establish sustained Web
reliability, physical display rendering, every BLE/USB input path, Safari
behavior, speaker output or long-run radio coexistence. The
community release gate and remaining hardware cases are listed in the
[Passport guide](../passport/README.md) and
[community release requirements](passport-community-release.md).

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

- **Mosaico iPhone Web pad expiry.** A physical iPhone session received
  `rejected: expired` for movement despite an authorized control session.
  The updated firmware at `8c8057e` was installed on the Mosaico and passed
  same-boot OTA health confirmation. The served HTTPS page and JavaScript
  matched the candidate. An authorized client read state and configuration
  and received an authenticated WebSocket heartbeat. The check sent no HID
  input. In a subsequent physical iPhone test, the operator reported that
  swiping moved the pointer but motion still felt jumpy. Individual movement
  receipts and the cause of the uneven motion were not established by that test.
  Follow-up image `4e80279` passed on-device OTA health confirmation, served
  the matching HTML, CSS and JavaScript over HTTPS, and negotiated the renewal
  terminal extension on an authenticated WebSocket. The iPhone operator
  subsequently reported loss of control after 5–10 seconds of rapid swiping.
  Movement still expired on `6322e6c`. On `6850c24` the operator reported
  better touch response and no disconnect during that observation. Pointer
  jumps remained a concern.

  A desktop Chromium test drove the board running `6850c24` with the same
  360-point stroke before and after loading the `b861a98` script in the
  browser. Of 1,080 units of horizontal travel, terminal HID receipts accounted
  for 492 with the installed script and 801 with the candidate. The input
  ended 72 units to the right. The reported net movements were +126 and +99.
  In a separate test the candidate tracked 29 moves outside the pad after
  losing pointer capture. The finger lifted outside, all 12 submitted moves
  reached the HID transport, and Stop completed the neutral release.

  After `b861a98` was installed and confirmed by OTA, desktop Chromium
  loaded the matching page from the board. It tracked 29 moves outside the
  pad after losing capture. All ten submitted movements reached the HID
  transport, including seven submitted after crossing the boundary. The
  outside lift ended the contact with no further movement commands. Stop
  closed that session with HTTP 200. On the separate 360-point stroke, 55
  moves reached the transport and nine expired. The delivered path was 477
  units with net +141. Terminal receipt intervals had a median of 103 ms,
  a 95th percentile of 458 ms and a maximum of 690 ms. The same lease
  survived three renewals, and Stop completed the neutral release. This run
  did not reproduce the preview's gain in delivered travel. Pointer feel on
  a physical iPhone running this revision remains unverified.
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
