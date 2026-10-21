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

## Next checkpoints

| Stage | Publication gate | Current state |
| --- | --- | --- |
| Input foundation and device basics | Relevant host suites, pinned target build, independent review | Passed; device gates remain |
| Nine-cell UI, saved peers and USB switching | Native UI inspection, transport interruption tests, target build, independent review | Passed; device gates remain |
| Device verification | Actual deployment identity, boot/health evidence, observed input and connection checks | Failed expanded baseline; memory/display repair and retest required |
| Audio and spoken letters | Working drivers, acoustic/recognition evaluation and host qualification | Planned; unavailable in current input firmware |
| Remote Wi-Fi control and MCP | Authentication, ownership, complete state and failure-path tests | Contract work; provisioning alone is not remote control |

Every subsequent checkpoint must state its exact verification scope and remaining
gaps. A short smoke run cannot establish unattended or long-duration reliability.
