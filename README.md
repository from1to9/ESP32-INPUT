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

This is an integrated development firmware. Source implementation, local tests,
independent review and target compilation are separate from physical qualification.
The combined delivery does not flash or test a device; see the
[delivery checklist](docs/delivery-checklist.md) and [validation record](docs/validation.md).

- One coordinator serves BLE/USB composite HID, wrist motion, a relative touchpad,
  default tap/double-tap/drag, capability-gated two-finger gestures and a preview-first
  nine-cell keyboard. Common keys and Ctrl/Command shortcuts share the same release barriers.
- Motion uses gravity-referenced grip adaptation with independent axis settings.
  Spoken letters use the pinned English recognition model and require confirmation
  into the draft before Send; this is not general offline dictation.
- One audio manager owns the codec, HFP voice, A2DP playback, USB UAC, local recording
  and speech consumers. Readiness, microphone indicators, mute acknowledgements,
  link identities and cleanup are observable. Concurrent paths require physical qualification.
- Recording uses an explicitly prepared, bounded encrypted NOR partition. Unknown
  storage is preserved. The external NAND is not automatically formatted or used.
- Wi-Fi joins an existing LAN through native scan/password controls and remembers
  up to eight ordered profiles. Authenticated HTTPS/WebSocket APIs and the
  [MCP companion](bridge/README.md) expose the typed operation registry. USB enrollment
  requires an explicit local commissioning window and review; LAN access alone grants no control.
- Saved Bluetooth peers, explicit pairing windows, device naming, target preferences,
  local override and bounded remote-input leases are integrated. Input ownership and
  independent media authorization have separate lifetimes.
- The [live sensor API](docs/sensor-api.md) exports a bounded, read-only journal
  over USB CDC and authenticated station HTTPS, with a host JSONL/CSV collector.
  Raw acquisition errors and retention gaps remain visible; transport and Motion
  usability require separate physical qualification.
- Compact native workspaces, Light/Dark themes, boot logo, battery/connectivity status,
  50% default brightness, configurable display blanking and haptics use actual capabilities.
  Temporary settings remain distinct from saved preferences.
- Signed firmware staging uses an inactive application slot and explicit authorization.
  The new partition layout requires a separately planned initial migration; downloading
  an OTA image does not migrate a partition table or replace recovery backups.
- The official BSP, SDK, dependencies and speech model are pinned. Portable engines,
  named hardware adapters and capability reporting keep other board ports isolated.
  The browser prototype remains a design reference with simulated connections and audio.

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
