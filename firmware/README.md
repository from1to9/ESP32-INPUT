# Composite input firmware

This development build uses the official Mosaico BSP and ESP-IDF commit pinned in [toolchain.lock.json](../toolchain.lock.json). The current integration adds selectable Bluetooth/USB mouse and keyboard input to the three-mode LVGL product layout. Audio and spoken-letter controls remain explicitly unavailable. Implementation, reviewed builds and actual device/host evidence are distinguished in [validation](../docs/validation.md).

## Implemented boundary

- A stable report-only HID service: keyboard ID 1 (8 input bytes, 1 LED output byte) and mouse ID 2 (4 input bytes).
- LE Secure Connections bonding, encryption, both notification subscriptions, suspension, congestion and connection generation checks before output. Just Works bonding has no MITM authentication. A visible two-minute window admits new peers; existing saved peers can reconnect outside it. Legacy bonds without an exported SC encryption key are rejected and preserved, not silently erased.
- Bounded per-bond subscription storage restores actual saved CCCD values after encrypted reconnect. Records are tied to the bond identity and encryption-key fingerprint; input queues are never restored. Changed bonded subscriptions are committed before a successful ATT reply; unchanged values and idle health logging do not cause commits. Storage or identity failures keep output closed.
- One coordinator owns input mode, lock, epoch and bounded queues. Notification acceptance is not proof of host UI success.
- A relative touchpad with 1x, quarter and eighth gain, left/right clicks, explicit drag/release and separate wheel controls. Live BMI270 samples drive calibrated relative motion or local keyboard focus.
- The approved nine-cell character grid, two-stage selection, case/symbol pages, a 64-character draft and explicit Send/Stop. Bounded 32-character transport batches retain only the unsent suffix after interruption. Enter is a separate shortcut.
- Focused Mouse/Keyboard/Audio pages with a compact actual input-target cue, critical-state action and a shared Menu/AI-double-click quick panel. Status, Settings and compact entry/kind choices are coordinator-owned. Primary controls stay fixed; only the active panel is constructed. Modal focus and physical-only unlock are preserved. Host modifiers are explicitly configurable as Ctrl or Command.
- AI single-click toggles mouse motion or confirms keyboard selection; double-click opens the mode chooser. A 900 ms hold locks or unlocks. Clicks wait through a 280 ms double-click decision window. Button-down and touch freeze motion; unlock never resumes old input.
- Discrete haptic feedback with a persistent enable switch. Continuous movement is silent; motor faults disable capability.
- A versioned settings record for theme, brightness, display timeout, haptics, shortcut modifier and preferred input transport, with coalesced writes and visible storage failures. Schema-1 records migrate to Bluetooth preference without a boot-time rewrite.
- Actual battery measurements and Wi-Fi status. Local protected hotspot provisioning, one remembered station network, bounded reconnect and explicit forgetting are separate from the future remote-control API.
- USB CDC diagnostics and composite HID share the official managed TinyUSB stack. There is no serial command parser, remote wakeup or automatic reset/download action. USB HID integration and real-host acceptance must pass their own gates.
- System transport switching cancels pending work and drains old-host releases before changing selection, or permits explicit switching when the old host is already disconnected without claiming release delivery. A suspended/stalled connected host blocks switching until recovery or timeout. Cable removal never redirects input automatically.
- Default five-minute display blanking with 1 / 5 / 10 / 30 minutes and Off, adjustable brightness, and whole-gesture wake consumption. Radios and enabled mouse control remain active; hidden keyboard selection pauses.
- Periodic health logs and a coordinator task watchdog. Audio, speech and Wi-Fi remote input are not enabled. Real motion axis direction, calibration drift and physical touch usability still need device acceptance.

Bluetooth Battery and Device Information services are not implemented in this minimal transport; the local UI's battery/About pages are separate. Full HOGP profile compliance and host-family interoperability are not claimed. The initial SDK capacity is 15 remembered peers, with one active BLE HID connection. Additional pairing is refused at capacity without evicting existing peers. The UI exposes current-peer identity only when actually known.

## Reproduce locally

Install Git, a C compiler and Python 3.11 or newer, then run from the repository root:

```sh
git submodule update --init --recursive
python3 tools/idf.py setup --python python3.12
python3 tools/idf.py build
python3 tools/check.py
python3 tools/check_repository.py
python3 tests/ui/run.py
python3 tools/idf.py size
```

The setup helper installs the pinned SDK under `.tools/esp-idf` and its tools under `.tools/espressif`, including CMake and Ninja (on-request tools in this SDK). It does not change shell profiles or global tool installations. The build helper accepts only build/reconfigure/size actions and never accesses a device. The SDK is a development version with preview S31 support; the exact commit matters. Resolved components are pinned by `dependencies.lock`.

Build outputs are in `firmware/build/`. First-party source lives in `main/`, portable components in `../components/`, and all Mosaico BSP calls in `main/board_mosaico.c`. See [porting](../docs/porting.md) for adapter boundaries.

The Mosaico configuration uses its documented 16 MiB flash and a 4 MiB factory application partition. There is no OTA update path in this build. Reproduce a clean configuration after deliberate defaults changes by preserving the existing ignored `firmware/sdkconfig` outside the firmware directory before rebuilding. Do not silently discard a contributor's custom configuration.

### Display memory policy

The Mosaico profile in `sdkconfig.defaults` prefers PSRAM for ordinary/UI
allocations, dynamic Bluetooth host data and eligible Wi-Fi/LWIP data. It
reserves 64 KiB of internal RAM for requests needing internal/DMA capabilities;
task stacks remain internal. This reserve is shared, not a guaranteed contiguous
LCD buffer. Ports without PSRAM need their own qualified resource profile.

The tracked profile sets Wi-Fi static RX buffers and the receive Block Ack
window to two each, retaining internal DMA headroom at the expense of peak
network throughput. TCP out-of-order retention is bounded to four pbufs per
connection. The pinned SDK otherwise defaults to 16/16 and unbounded retention
when PSRAM network allocation is enabled. Validate a newly generated
configuration, not only an existing ignored `sdkconfig`; prior local values
can conceal missing defaults. Network throughput and combined-load endurance
still require device qualification.

Keep the pinned BSP's TE synchronization and internal DMA bounce path. TE mode
uses full-frame rendering; reducing LVGL buffer height does not reduce that
mode's frame or transfer demand. The ten-entry LCD transfer queue can retain
roughly 80 KiB of bounce buffers plus overhead, so total free heap or the reserve
alone cannot prove that transfers fit. The earlier expanded device image failed
despite abundant external memory; see the [failed device gate](../docs/validation.md#expanded-baseline-failed-device-gate).

Runtime diagnostics retain six startup memory samples and the first allocation
and Bluetooth failure until reboot, so late CDC attachment does not erase the
cause. The allocation hook uses fixed storage and lock-free publication, with
no logging or allocation inside the hook. Health reports include free and
largest internal DMA blocks. These observations do not alter input admission,
retry errors or restart the device. Check actual generated `sdkconfig` after
changing defaults; an existing configuration keeps its previous values.

## Device procedure

Device deployment is separate from compilation. Identify the actual chip and flash, retain a complete original-flash backup with its hash, and check recovery before writing. Backups may contain credentials and stay in ignored `.agent/local/` with private permissions. Never commit them. The adapter rejects unknown board eFuse revisions before configuring board peripherals.

The main Type-C port uses native USB OTG. If no serial port appears, hold BOOT while powering on to enter ROM download mode. The running application provides its own CDC console; this build deliberately does not implement automatic download. Manual BOOT mode remains the recovery route. See the [official Mosaico guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html).

After separately authorized flashing, capture health logs and perform the [test coverage plan](../docs/test-coverage.md). Pair a host, wait for encrypted HID readiness, then use a disposable text editor to select, preview and explicitly send synthetic text and verify modifiers. Verify lock between press/release, disconnect, host sleep and congestion recovery. A target build or health heartbeat does not prove mouse/keyboard correctness or long-term stability.

The read-only capture helper requires a Python environment with `pyserial` (the local IDF environment includes it). Supply the actual enumerated runtime port, not a guessed ROM port:

```sh
python3 tools/device_smoke.py --port <runtime-port> --seconds 90 --output .agent/local/device-smoke
```

Replace `<runtime-port>` before running. The helper never flashes, resets or sends serial commands; it opens with inactive DTR/RTS. It preserves existing output files, bounds its raw log to 1 MiB, and produces a JSON summary. A fixed one-second grace may finish a line already started at the capture deadline. A pass requires at least three increasing health samples, no recorded error/reset, queue occupancy within capacity, and no initial/intermediate/final health gap longer than 25 seconds. Changing `--seconds` does not convert a health capture into HID or thermal qualification. Use a fresh output prefix for each run.
