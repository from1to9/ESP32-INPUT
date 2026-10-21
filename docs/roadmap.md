# Roadmap and open decisions

## Milestones

| Stage | Deliverable | Acceptance | Status |
| --- | --- | --- | --- |
| D0 | Requirements, standards, prototype, pinned BSP, portable core | Consistent design, interactive prototype, passing host tests | Baseline established |
| D1 | Board bring-up and minimal BLE composite HID | Target build plus separate mouse/text qualification on four host families | Minimal implementation and target build complete; device/host qualification in progress |
| D2 | Motion mouse, touch fine control and LVGL character selection | Mode gates, releases and reconnection behavior validated | Product implementation and host tests complete; final review/device qualification in progress |
| D3 | Local spoken letters | Spelling word -> candidate -> confirm -> HID | Pending implementation and evaluation |
| D4 | Independent HFP/A2DP | Capture, playback, mute and echo handling qualified | Pending |
| D5 | Same-host HID plus audio | Per-OS combination and stress checks pass | Capability experiment pending |
| D6 | Wi-Fi control panel and Mac MCP bridge | Authenticated lease, complete state, failure tests | Design complete; implementation pending |
| D7 | Multiple saved peers and selectable Bluetooth/USB input | No bond eviction, explicit target routing, release barriers and USB host tests | Current input implementation; USB audio and simultaneous hosts remain later |
| D8 | Connection management and Wi-Fi firmware updates | Truthful scan/select UI, saved-network/peer ordering, authenticated updates, inactive-slot validation and rollback | Newly requested design; no OTA path in current factory-only image |

Audio coexistence research may accompany SDK validation without blocking independent HID. If a combination is unsupported or unstable, release independent modes with explicit capabilities.

## Decisions already made

- Project name ESP32-INPUT; private GitHub repository by default.
- First-party repository content is English. General working rules are separate from product requirements.
- Cold boot selects mouse with motion off. Keyboard owns motion selection and spoken letters.
- Audio shares managed resources but starts as an independent mode. Expose concurrency only after qualification.
- Begin with one active HID target and multiple remembered Bluetooth bonds. System settings selects Bluetooth or USB independently of the primary mode; input is never broadcast to both hosts.
- Use the existing AI button and touch surface; no extra hardware is required for the initial scope.
- Prefer Alpha-Zulu commands. Full dictation is outside D3.
- M5 is behavioral reference material; do not copy unlicensed implementation.
- The development board is Mosaico v1.2 with 16 MB flash; native USB ROM download and full original-flash backup have been verified. Runtime USB and HID still require separate tests.
- ESP-IDF and BSP revisions are pinned in `toolchain.lock.json`; the resolved managed components are fixed in `firmware/dependencies.lock` after a successful S31 build.

## Decisions requiring evidence

1. iPhone AssistiveTouch behavior with the composite descriptor.
2. IMU orientation, drift, deadzone, gain and selection hysteresis.
3. Runtime BOOT accessibility; no product function is assigned until verified.
4. Same-host HFP/BLE HID coexistence and pairing behavior.
5. Memory/CPU budget with ESP-SR, Bluedroid and LVGL together.
6. Single-letter pronunciation confusion rates and whether only spelling words should be enabled.
7. Extended power, idle display behavior, temperature and storage-wear qualification.

## Outside the initial release

iPad video relay through Mosaico; blind business approvals/attendance; automatic account switching; mandatory full-sentence offline ASR; handwriting as default input; keyboard broadcast to multiple targets; LE Audio as the primary compatibility path.

The current authorized implementation includes toolchain setup, Bluetooth/USB composite HID, the original product flows with the approved nine-cell keyboard revision and Light/Dark themes, haptics, board isolation, persistent settings, battery status, Wi-Fi provisioning, host/build tests, separately managed role conversations, full first-party code review, device tests and staged GitHub synchronization. The user expanded the earlier no-flash boundary to include device testing. Remote Wi-Fi control and MCP remain design deliverables; provisioning does not enable remote input. Each completed stage is published promptly with its checks and remaining gaps in [delivery progress](progress.md). Long-term external-power operation requires duration and fault testing, not only a successful boot.
