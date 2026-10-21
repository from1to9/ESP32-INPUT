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

## Next checkpoints

| Stage | Publication gate | Current state |
| --- | --- | --- |
| Input foundation and device basics | Relevant host suites, pinned target build, independent review | Integration and regression fixes |
| Nine-cell UI, saved peers and USB switching | Native UI inspection, transport interruption tests, target build, independent review | Implementation |
| Device verification | Actual deployment identity, boot/health evidence, observed input and connection checks | Pending the reviewed candidate |
| Audio and spoken letters | Working drivers, acoustic/recognition evaluation and host qualification | Planned; unavailable in current input firmware |
| Remote Wi-Fi control and MCP | Authentication, ownership, complete state and failure-path tests | Contract work; provisioning alone is not remote control |

Every subsequent checkpoint must state its exact verification scope and remaining
gaps. A short smoke run cannot establish unattended or long-duration reliability.
