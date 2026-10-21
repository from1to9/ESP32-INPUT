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
