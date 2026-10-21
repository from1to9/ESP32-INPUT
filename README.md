# ESP32-INPUT

A motion and touch input device based on **ESP-Mosaico / ESP32-S31**, targeting iPhone, iPad, Windows and macOS. Standard Bluetooth HID is the planned baseline; target devices should not need a proprietary client for basic mouse and keyboard input.

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
- `firmware/` is a BSP bring-up scaffold. BLE HID, the product LVGL UI and audio are not connected yet.
- Host C11 tests have passed with strict warnings and address/undefined-behavior sanitizers. Browser interaction checks are recorded in [validation](docs/validation.md).
- Target compilation and device/host compatibility are not yet verified. SDK setup and minimal composite HID are the next implementation stage.

Repository: [from1to9/ESP32-INPUT](https://github.com/from1to9/ESP32-INPUT) (private).

## Standards and design

- [Working agreement](AGENTS.md) and [engineering standards](docs/engineering.md)
- [Contributing](CONTRIBUTING.md)
- [Requirements](docs/requirements.md)
- [Interaction and buttons](docs/interaction.md)
- [Architecture](docs/architecture.md)
- [Validation](docs/validation.md)
- [Roadmap](docs/roadmap.md)
- [Sources and licensing](docs/references.md)
- [Interactive prototype](design/index.html)

## Local development

```sh
git submodule update --init --recursive
python3 tools/check_repository.py
python3 tools/check.py
python3 -m http.server 8768 --bind 127.0.0.1
```

Open `http://localhost:8768/design/`. The prototype requires no npm packages or external assets.

The firmware requires ESP-IDF 6.2 or later with ESP32-S31 support. Portable tests do not require that SDK. SDK setup is not yet a completed build qualification.

```sh
cd firmware
idf.py --preview set-target esp32s31
idf.py build
```

Build and device deployment are separate steps. Confirm board revision, data port and recovery procedure before any separately authorized flashing. Unknown board eFuse values must stop initialization rather than select guessed pins.

## Licensing

Original code and documentation use Apache-2.0. The BSP retains its own license and provenance. M5 projects are behavioral references only; no unlicensed implementation is copied. Speech models, binary libraries and experimental ASR require separate license review.
