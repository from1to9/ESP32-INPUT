# Independent code review

## Current expanded baseline

The independent review sealed 128 first-party code/configuration/test files
after device basics, the nine-cell keyboard, multiple saved peers and USB input
were integrated. Inventory SHA-256:
`11c200a3d1237b05b3c1a4021a5344dd4b5b0d49e2ce10d431308f26d0a1fe26`.
No blocking finding remains in that exact source/host-tested snapshot. The
coordinator verified the sealed file hashes before final integration delivery.

The expanded review closed IR-07 (incomplete setup shutdown), IR-08 (uncertain
Wi-Fi storage), IR-09 (bond refresh after fast disconnect) and IR-10 (confirmed
operations following the wrong peer or mishandling their own disconnect).
Earlier IR-01 through IR-06 remain covered by retained regressions. USB review
cross-checked real pinned TinyUSB reset/defer behavior and SDK serial identity.

The full host suite passed, including 77 coordinator and 16 settings-store cases.
UI inspection covered 164 native Light/Dark cases. Target build and device
evidence are separate in [validation](validation.md). This seal excludes the
new connection scanning/priority and firmware-update design work, physical host
qualification, audio/recognition and long-duration power testing.

## Product UI replacement

The original product layout, live input model and Light/Dark themes supersede the diagnostic Foundation interface. The replacement passed independent review as a frozen v3 baseline covering 87 code/configuration/test files, with code inventory SHA-256 `6c5a0b8e3d44e3adbbec964ccc1825fc89bbe09ebb3c342fafd044eadf394f0b`. Six findings were fixed and retested, including pending-click focus stability and long-text layout. The user then expanded the scope to device settings, battery/radio status, pairing, Wi-Fi provisioning and a full UI alignment pass. That expanded implementation is under review; the v3 approval does not cover it. The original-design acceptance checklist remains in [design conformance](design-conformance.md).

## Historical foundation snapshot

Date: 2026-10-21. The independent **ESP32 Full Code Review** conversation reviewed
all 69 first-party code, configuration, build, test and prototype files, including
unchanged and newly added source. Its complete inventory also included 24
documentation/license context files. No unresolved code blocker remained in the
frozen snapshot. Vendor sources were checked for pins, provenance and relevant
integration contracts, not subjected to a complete upstream audit.

The reviewed code inventory SHA-256 is
`691649327724584452ad7450af8dce456a4fc1c185d79b2f7844648d99a64a7e`.
It hashes lexically sorted UTF-8 lines of each file's SHA-256, two spaces,
repository-relative path and LF. Per-file hashes, the complete manifest and
independent verification output remain in ignored `.agent/local/`. The
integration task verified every code hash again before deployment. Later
documentation records device evidence separately; they do not change this code
inventory.

## Findings addressed during integration and device bring-up

| Finding | Resolution and evidence |
| --- | --- |
| Touch could unlock; competing safety requests had unclear priority | Physical-only unlock, touch-lock priority and full-queue coordinator regressions |
| Congestion could replay obsolete work | Bounded deadline cancels once per episode, including when congestion clears; engine boundary and lifetime tests |
| Haptic cancellation could race the actual motor write | Shared dispatch lock covers validation and driver write, including the second error pulse; deterministic interleavings |
| Failed motor-off could appear inactive | Unconfirmed stop remains active and capability fails truthfully; motor policy/dispatch tests |
| Early boot faults disappeared before USB attachment | Slow repeated fault diagnostics with HID disabled |
| Early heartbeats could hide later serial silence | Host-monotonic initial/intermediate/final freshness checks and independent reproduction |
| Bonded reconnect lost notification subscriptions | Bounded SC bond fingerprint records, restore after verified encryption and commit before successful ATT reply; reboot/reconnect and storage-failure tests |
| Full queue silently discarded no-response disable | Admission closes and the connection faults; exact production-callback regression |
| Old lookup/commit failure poisoned a replacement connection | Connection identity and authentication revision checked atomically before failure mutation; independent replacement probes |
| Structured advertising used an invalid two-byte UUID input | Real device did not advertise; the pinned SDK requires full 128-bit input UUIDs. Corrected Bluetooth-base representation and an initialization regression reject the original argument |
| Bluetooth failure preceded USB attachment | Periodic health includes explicit BLE initialization/failure state and repeats a bounded error diagnostic |
| Capture deadline split a health record | A bounded final-line grace period completes an already-started frame; unfinished or faulty records still fail |

## Verification and limits

The independent full host suite passed strict compiler checks and sanitizers,
descriptor checks, coordinator safety tests, persistence regressions and the
21 serial-capture tests. Engine and BLE adapter each passed their separate
100,000-cycle workload. JavaScript syntax, text/link hygiene and whitespace
checks passed. These are not physical elapsed-time or radio qualification.

Final target builds, actual device observations and host compatibility are
recorded in [validation](validation.md). Detailed remaining coverage is in the
[test matrix](test-coverage.md). Audio, remote Wi-Fi input, physical motion tuning,
full HOGP service composition and 24/72-hour powered qualification remain
separate delivery stages.
