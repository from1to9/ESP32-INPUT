# Roadmap and open decisions

## Milestones

| Stage | Deliverable | Acceptance | Status |
| --- | --- | --- | --- |
| D0 | Requirements, standards, prototype, pinned BSP, portable core | Consistent design, interactive prototype, passing host tests | Baseline established |
| D1 | Board bring-up and minimal BLE composite HID | Target build plus separate mouse/text qualification on four host families | SDK preparation started; implementation and device qualification pending |
| D2 | Motion mouse, touch fine control and LVGL character selection | Mode gates, releases and reconnection behavior validated | Pending hardware integration |
| D3 | Local spoken letters | Spelling word -> candidate -> confirm -> HID | Pending implementation and evaluation |
| D4 | Independent HFP/A2DP | Capture, playback, mute and echo handling qualified | Pending |
| D5 | Same-host HID plus audio | Per-OS combination and stress checks pass | Capability experiment pending |
| D6 | Multi-host, Mac controller and USB composition | Independent routing/protocol qualification | Later |

Audio coexistence research may accompany SDK validation without blocking independent HID. If a combination is unsupported or unstable, release independent modes with explicit capabilities.

## Decisions already made

- Project name ESP32-INPUT; private GitHub repository by default.
- First-party repository content is English. General working rules are separate from product requirements.
- Cold boot selects mouse with motion off. Keyboard owns motion selection and spoken letters.
- Audio shares managed resources but starts as an independent mode. Expose concurrency only after qualification.
- Begin with one HID target. Multiple remembered bonds do not mean simultaneous output to multiple hosts.
- Use the existing AI button and touch surface; no extra hardware is required for the initial scope.
- Prefer Alpha-Zulu commands. Full dictation is outside D3.
- M5 is behavioral reference material; do not copy unlicensed implementation.

## Decisions requiring evidence

1. Actual board revision, USB data port and recovery procedure.
2. Exact ESP-IDF 6.2+ commit and resolved component lockfile after a successful target build.
3. iPhone AssistiveTouch behavior with the composite descriptor.
4. IMU orientation, drift, deadzone, gain and selection hysteresis.
5. Runtime BOOT accessibility; no product function is assigned until verified.
6. Same-host HFP/BLE HID coexistence and pairing behavior.
7. Memory/CPU budget with ESP-SR, Bluedroid and LVGL together.
8. Single-letter pronunciation confusion rates and whether only spelling words should be enabled.

## Outside the initial release

iPad video relay through Mosaico; blind business approvals/attendance; automatic account switching; mandatory full-sentence offline ASR; handwriting as default input; keyboard broadcast to multiple targets; LE Audio as the primary compatibility path.

The current authorized implementation work is SDK setup and minimum composite HID with build/host tests and GitHub synchronization. Device flashing is explicitly excluded. Project standards are defined first, as requested.
