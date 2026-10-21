# Delivery progress

Updated: 2026-10-21. Each completed stage is published promptly after its
applicable checks and independent code review. Work in progress is not a tested
firmware release. Source review, host tests, target builds, device checks and
host compatibility are separate evidence levels.

## Checkpoint 1: revised product contract

Status: design decisions recorded; implementation and qualification in progress.

- Use a two-stage 3 x 3 keyboard grid for motion and touch selection. Keep the
  preview, explicit confirmation and explicit Send; never append Enter.
- Retain multiple Bluetooth pairings, display the current peer and a saved-host
  list, and provide bounded new-pairing admission. Initially connect one HID
  host at a time. Do not display Bluetooth signal bars or invent peer names.
- Keep Mouse, Keyboard and Audio as the three functional modes. Offer a separate
  Bluetooth / USB selector in System settings, initially defaulting to Bluetooth.
  Complete and qualify both keyboard/mouse transports first. Audio availability
  requires its own driver and host tests; selection is not proof of support.
- Cancel pending input and release the old host's pressed keys/buttons before a
  transport switch. Never duplicate input or automatically redirect it after a
  cable disconnect. Preserve Bluetooth bonds when using USB.
- Apply one aligned visual system to every page, including Light and Dark themes,
  battery status, Wi-Fi setup/reconnection, input readiness and pairing controls.
- Default the display timeout to five minutes, with 1 / 5 / 10 / 30 minutes and
  Off options. The first complete touch or AI-button gesture wakes only.
- Retain the global haptic switch, portable board interfaces, bounded resources,
  persistent preferences and independent review of all first-party code.

This checkpoint changes documentation only. It does not claim that the new
grid, USB input, audio or multiple-host interoperability has passed device tests.

## Checkpoint 2: portable input and haptic foundations

Status: independently reviewed source and focused host checks passed.

- Added the transport-independent composite HID descriptor, bounded action queue,
  drag state, accepted-prefix text tracking, cancellation and release recovery.
- Added portable haptic admission, timed patterns, cancellation generations,
  cooldown and conservative motor-failure handling.
- Re-ran strict compiler checks with AddressSanitizer and UndefinedBehaviorSanitizer.
  HID checks include 100,000 deterministic lifecycle cycles. These are simulated
  transitions, not an elapsed-time device endurance claim.
- Verified published code/test hashes against the independent review's frozen
  inventory; the focused descriptor decoder validates report sizes and semantics.

This checkpoint publishes portable components and their tests. It does not yet
replace the firmware entry point or qualify physical Bluetooth/USB input. The
component READMEs contain standalone reproduction commands.

## Checkpoint 3: nine-cell interaction and persistent preferences

Status: independently reviewed portable implementation and host checks passed.

- Added the 3 x 3 letter/digit and symbol groups, valid-cell gyro selection and
  hysteresis. Touch and physical confirmation share the preview-first path.
- Added display idle/wake policy and a versioned preference codec, including
  Bluetooth/USB preference. Schema 2 reads valid schema-1 records as Bluetooth;
  unknown/corrupt records are rejected without changing the caller's value.
- Strict compiler and sanitizer suites passed. Independent review also exercised
  400 generated legacy-record migration cases and 16 settings-store cases.
- Published portable source, headers, registration, documentation and tests match
  the frozen ten-file review inventory.

This checkpoint does not yet qualify the full UI, flash adapter, USB/Bluetooth
switching or physical motion. Those integration and device checks remain below.

## Checkpoint 4: Wi-Fi provisioning service

Status: independent service review and focused host/target checks passed.

- Added explicit protected-hotspot provisioning, one remembered station network,
  bounded reconnection, signal/status snapshots and local forgetting.
- Verified that expired or closed setup revokes admission immediately and retries
  incomplete HTTP/AP shutdown at a bounded one-second interval.
- Kept runtime configuration separate from durable storage evidence. Failed writes
  may already have taken effect; uncertainty remains visible until an explicit
  successful save, forget or read verification.
- Closed review findings IR-07 and IR-08 with independent failure probes. Supported
  and unavailable-hardware host tests and pinned S31 object compilation passed.

The service is published separately from firmware entry-point integration. Actual
hotspot/browser provisioning, hardware persistence and radio coexistence remain
device gates. The accompanying remote-control/MCP documents are future contracts;
this service does not execute network HID commands.

## Checkpoint 5: native nine-cell UI and connection settings

Status: the exact eight-file UI scope passed independent review and native tests.

- Rebuilt the original product layout around aligned nine-cell selection, explicit
  preview/Send and separate Mouse, Keyboard and Audio modes.
- Added Light/Dark themes, display and haptic preferences, Bluetooth saved-host
  pagination/capacity, Wi-Fi setup/status and an independent Bluetooth/USB selector.
- The production LVGL UI passed sanitizer, geometry and actual pointer-hit checks.
  Independent review inspected 164 native render cases, including unavailable,
  disconnected, storage-uncertain and saturated-capacity states.
- Setup credentials are cleared when the view exits, locks, expires or sleeps.
  Unsupported audio and recognition capabilities remain visibly unavailable.

This checkpoint publishes the UI and its visual contract, not device-screen or
transport interoperability evidence. Coordinator and adapter review continue.

## Checkpoint 6: Bluetooth composite HID and saved peers

Status: the exact 21-file adapter scope passed independent review and checks.

- Added encrypted composite input, verified per-bond subscriptions, bounded
  pairing admission and a truthful cached saved-host list with capacity checks.
- Preserved durable bond refresh across authentication/disconnect races. A new
  pairing requires a fresh capacity check, preventing implicit oldest-bond eviction.
- Independent regression probes closed IR-09. Strict sanitizer suites passed at
  capacities 15 and 32, each with 100,000 simulated lifecycle transitions.
- Pinned S31 object compilation and the expanded linked build passed. These do
  not establish successful over-the-air pairing or actual host input.

Coordinator release ordering and physical host/device checks remain separate
gates. Nearby scanning and prioritized saved-target selection are later additions.

## Checkpoint 7: USB composite input and diagnostics

Status: the exact nineteen-file USB scope passed independent review and checks.

- One TinyUSB owner provides CDC diagnostics plus keyboard/mouse reports, with
  separate admission, submission and completion state and bounded deadlines.
- Reset, suspend, cancellation and late completion cannot replay obsolete input.
  HID failure preserves the diagnostic stack; actual reset is the recovery gate.
- Real pinned TinyUSB descriptor macros and SDK serial generation were tested
  under strict compiler diagnostics and sanitizers, including 100,000 lifecycle
  cycles. Both S31 adapter objects and the expanded linked build passed.

This checkpoint publishes the adapter and its tests. The final integration
commit supplies its build/configuration wiring. Actual USB enumeration, host
input, cable/suspend behavior and electrical/thermal qualification remain gates.

## Checkpoint 8: integrated input and device settings baseline

Status: all 128 first-party code/configuration/test files passed independent
source review; no blocking findings remain in the sealed baseline.

- Integrated the product UI, Mosaico board/IMU/haptics adapter, measured device
  status, persistent settings, display wake policy, Wi-Fi setup and BLE/USB input.
- Switching waits for final release completion, rejects replacement connections,
  and preserves explicit selection without automatic fallback. Pairing operations
  distinguish their own asynchronous disconnect from an unrelated replacement.
- The full host suite passed, including 77 coordinator cases, 16 settings-store
  cases, BLE/USB/board checks and serial capture regressions. Ten independent
  review findings were fixed and retested. The native UI has 164 inspected cases.
- The pinned S31 linked build passed, with a 0x24da80-byte application and 42 percent
  of the four-MiB application partition free. Source/build evidence is recorded
  in validation; physical deployment and host input remain separate gates.

Wi-Fi scan/profile management, reconnect priorities and firmware update APIs
are newly requested extensions. The current factory-only image has no OTA path;
the next design must provide inactive-slot validation and post-boot rollback.

Deployment of this baseline passed write verification but **failed physical
acceptance**: touches caused vibration without visible transitions, LCD transfers
reported memory exhaustion, and Bluetooth latched a later transport failure.
The [device evidence](validation.md#expanded-baseline-failed-device-gate)
records the exact image and observed memory limits. Repair and fresh device
verification are required; source/build approval remains a separate result.

The next UI revision follows the user's explicit request to maximize mode work
areas. Persistent mode tabs and detailed status give way to an accessible menu;
core mouse, keyboard and audio workflows remain. The redesign must measure
object/update overhead and preserve gesture, lock and target-state safeguards.

## Checkpoint 9: internal-memory repair and retained diagnostics

Status: independently reviewed and published at
`41d6b42b65e432724c94af0f15caf2140c12bb99`; physical recovery remains unverified.

- General/UI allocations, eligible Wi-Fi data and dynamic Bluetooth host data
  prefer PSRAM, preserving internal memory for stacks and LCD DMA transfers.
  The pinned BSP's TE synchronization and DMA path remain intact.
- Fixed startup memory samples, runtime DMA headroom and first allocation/BLE
  failure records remain observable after late USB diagnostic attachment.
- The exact 21-file repair passed independent review, the complete host suite,
  80 coordinator cases, 23 serial-parser cases and the pinned S31 linked build.
  The compiled application hash is recorded in [validation](validation.md).
- The 64 KiB internal reserve is shared and does not guarantee display queue
  capacity. Successful real redraws, pairing and input must still be observed.

The focused UI and its navigation/performance tests form a separate checkpoint.
This repair is not proof that the user's reported touch/display problem has
been resolved on the device.

## Checkpoint 10: unified remote-control planning

Status: independently reviewed and published at
`25760ebff776562768dd1221f86b4e261ff58535`; implementation has not started.

The eight-document plan maps all 48 existing UI actions and future capabilities
to shared input, configuration, connection, audio and update owners. Device UI,
Wi-Fi clients and MCP use the same validation and command semantics. Wi-Fi
management remains independent from the selected Bluetooth/USB input route.
The plan defines bounded authorization, cancellation, reconnect, recording and
update lifecycles, including explicit unsupported operations.

All eight cross-document review findings are closed. Machine-readable schemas,
generated bindings and executable failure-path tests remain required before
new interfaces are exposed. The 90 remote acceptance cases and candidate A/B
partition layout are plans, not evidence of working control or OTA services.

## Checkpoint 11: focused workspaces and bounded UI updates

Status: published at `955877104733af6d756ec358e2c328db3b85a35b`; flashed and
physical navigation confirmed. The bounded runtime smoke still fails.

- Each mode keeps its core controls. Menu and physical AI double-click open one
  shared panel for mode switching, Status, Settings and Lock. The mouse surface
  is 93% larger by area; the keyboard retains its nine-cell preview-first flow.
- Three primary pages remain fixed and at most one secondary panel exists.
  Native observations fell from 315 baseline objects to 102 core objects, with
  a measured panel peak of 142. Unchanged and covered content no longer causes
  periodic text allocation or full-screen invalidation.
- The stable native run passed 174 rendered scenes and 50 resource cases in both
  themes. Each theme completed 1,000 panel open/close cycles without retained
  growth. New checks cover held-control expiry, stale gestures, confirmation
  cancellation, simultaneous connection errors and setup-secret cache cleanup.
- The complete host suite passed, including 91 coordinator cases. The final
  pinned target build and image identity are recorded in [validation](validation.md).

The user confirmed touch-based Menu/Keyboard/Mouse navigation and AI double-click
after flashing the recorded image. The 90-second capture retained stable internal
memory with no captured LCD allocation failure, but repeated zero-wait LVGL lock
errors make its result failed. A subsequent capture observed Bluetooth connected
and ready; host input and physical Motion remain unverified. Motion could not be
enabled even after the user selected USB. Factory firmware reportedly displays
accelerometer data, so this does not establish hardware failure. Audio and remote
control remain unavailable/planned; no long-duration qualification is claimed.

## Checkpoint 12: default brightness

Status: independently reviewed, host-tested and published at
`550f1a31b45d279e303a8b527dd806c2f2eb8c42`; included in the later diagnostic
deployment, with physical brightness still unverified.

The fallback brightness is 50 percent. Existing schema-1/schema-2 values,
including explicitly stored 70 percent, remain unchanged. Initialization never
writes defaults or migrates storage implicitly. Portable settings, 16 production
store cases and 91 coordinator cases passed with sanitizers. The twelve-file
review inventory has no open findings. This source change alone does not prove
physical brightness or a new device image.

## Checkpoint 13: Motion startup diagnosis

The diagnostic stage was published at
`db0d24fbf9d4dbf97a52fe6ae58483c8b4ee2f3d` and deployed with verification.
The capture identifies the initial IMU readiness timeout: accelerometer data
was ready, gyro data was not, and no sample had been accepted. Input transport
readiness was already true. The startup fix extends only the initial acceptance
window to 100 ms; runtime freshness and batch limits remain unchanged.

Fresh configuration generation also exposed omitted Wi-Fi memory limits,
causing a retained DMA allocation failure. Tracked defaults now preserve the
two-buffer/two-window profile and bound TCP out-of-order retention. The failed
capture remains failed; corrected-image device evidence is still pending.
See [validation](validation.md#imu-startup-diagnosis-and-configuration-reproducibility)
for the exact image and evidence boundaries.

## Next checkpoints

| Stage | Publication gate | Current state |
| --- | --- | --- |
| Input foundation and device basics | Relevant host suites, pinned target build, independent review | Passed; device gates remain |
| Nine-cell UI, saved peers and USB switching | Native UI inspection, transport interruption tests, target build, independent review | Passed; device gates remain |
| Device verification | Actual deployment identity, boot/health evidence, observed input and connection checks | Compact UI navigation confirmed; runtime lock warnings and Motion diagnosis remain open |
| Audio and spoken letters | Working drivers, acoustic/recognition evaluation and host qualification | Planned; unavailable in current input firmware |
| Remote Wi-Fi control and MCP | Authentication, ownership, complete state and failure-path tests | Contract work; provisioning alone is not remote control |

Every subsequent checkpoint must state its exact verification scope and remaining
gaps. A short smoke run cannot establish unattended or long-duration reliability.
