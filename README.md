# ESP32-INPUT

A motion and touch input device based on **ESP-Mosaico / ESP32-S31**, targeting iPhone, iPad, Windows and macOS. Standard Bluetooth and USB HID are the input transports; target devices should not need a proprietary client for basic mouse and keyboard input. Bluetooth is the initial preference.

## Three primary modes

| Mode | Interaction | Initial boundary |
| --- | --- | --- |
| Mouse (boot default) | Toggle motion, touchpad, fine adjustment, clicks, drag and scroll | Wrist rotation moves the pointer; touch temporarily takes priority |
| Keyboard | Motion selection, touch selection, shortcuts and text preview | Motion moves the local selection focus, never the host pointer |
| Microphone / speaker | Microphone, duplex voice and speaker playback | Independent modes first; concurrency requires separate qualification |

Keyboard mode reserves **spoken letter commands**, such as `Alpha -> A`, feeding the same preview buffer. Full offline dictation and air handwriting are experiments, not first-release commitments.

## Current implementation

This is a development baseline, not ready-to-use device firmware.

- The official BSP is pinned as a Git submodule with upstream notices intact.
- Requirements, interaction rules, architecture and staged acceptance criteria are documented.
- `design/` contains an interactive browser prototype. All connection, voice and audio states are simulated.
- `components/input_core/` contains portable state gates, motion integration and US ANSI key encoding.
- `components/hid_engine/` implements composite reports, bounded queues, cancellation and release recovery.
- `components/interaction/` implements the approved two-stage nine-cell character grid, 64-character preview, button gestures, calibration and motion selection.
- `components/device_settings/` implements versioned preferences and whole-gesture display wake rules without hardware dependencies.
- `components/haptics/` implements bounded, nonblocking feedback policy.
- `firmware/` connects the input transports to the three-mode LVGL product layout through a Mosaico board adapter. It includes a relative touchpad, explicit drag, preview-first keyboard, real IMU sampling, contextual AI gestures, USB diagnostics and haptic settings. System connection choice is independent of functional mode.
- Settings provides Light/Dark appearance, haptic feedback and Ctrl/Command selection. Dark starts with a black background, near-white text and a light-blue accent; preferences are saved through a validated, coalesced settings store.
- Device basics include measured battery/Wi-Fi status, multiple saved Bluetooth peers and current-host display, local Wi-Fi setup, brightness and safe display timeout. Their physical behavior and coexistence still require device qualification.
- Spoken letters, audio and Wi-Fi remote control remain unavailable; their UI and network control contracts preserve the intended product structure without simulated operation. Physical motion tuning and host compatibility require device evidence.
- Host C11 tests have passed with strict warnings and address/undefined-behavior sanitizers. Browser interaction checks are recorded in [validation](docs/validation.md).
- The pinned ESP-IDF toolchain has compiled the minimal S31 firmware. Device and host-family qualification are tracked separately in [validation](docs/validation.md).

Repository: [from1to9/ESP32-INPUT](https://github.com/from1to9/ESP32-INPUT) (private).

## Standards and design

- [Working agreement](AGENTS.md) and [engineering standards](docs/engineering.md)
- [Contributing](CONTRIBUTING.md)
- [Requirements](docs/requirements.md)
- [Interaction and buttons](docs/interaction.md)
- [Architecture](docs/architecture.md) and [board porting](docs/porting.md)
- [Wi-Fi setup](docs/wifi-provisioning.md) and [Wi-Fi / MCP control design](docs/remote-control.md)
- [Test coverage](docs/test-coverage.md) and [remote test plan](docs/remote-control-test-plan.md)
- [Validation](docs/validation.md)
- [Independent code review](docs/review.md) and [original-design acceptance](docs/design-conformance.md)
- [Roadmap](docs/roadmap.md)
- [Delivery checkpoints](docs/progress.md)
- [Sources and licensing](docs/references.md)
- [Interactive prototype](design/index.html)

## Local development

```sh
git submodule update --init --recursive
python3 tools/check_repository.py
python3 -m http.server 8768 --bind 127.0.0.1
```

Open `http://localhost:8768/design/`. The prototype requires no npm packages or external assets.

The firmware uses the exact SDK revision in [toolchain.lock.json](toolchain.lock.json). Setup needs Python 3.11 or newer. Portable components can be tested independently; the aggregate host suite additionally uses the resolved, pinned TinyUSB headers. Run setup/build once before the aggregate checks in a fresh checkout. See [firmware instructions](firmware/README.md) for dependency pins, diagnostics and recovery.

```sh
python3 tools/idf.py setup --python python3.12
python3 tools/idf.py build
python3 tools/check.py
```

Build and device deployment are separate steps. Confirm board revision, data port and recovery procedure before any separately authorized flashing. Unknown board eFuse values must stop initialization rather than select guessed pins.

## Licensing

Original code and documentation use Apache-2.0. The BSP retains its own license and provenance. M5 projects are behavioral references only; no unlicensed implementation is copied. Speech models, binary libraries and experimental ASR require separate license review.
