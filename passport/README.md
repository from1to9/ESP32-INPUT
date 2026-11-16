# Passport ESP32-C3 firmware

This independent ESP-IDF project implements the first local BLE HID batch
of the [Passport compatibility plan](../docs/passport-compatibility-plan.md).
It combines the FoloToy 240 × 320 display, three ADC buttons, BLE keyboard
and relative mouse, persistent bonds, and the existing read-only Wi-Fi health
listener. It uses the Passport 8 MiB flash layout with NVS, PHY data, and a
factory application. Shared `input_core` and `hid_engine` sources are reused
without changes to Mosaico.

The implemented subset and internal API are documented in [API.md](API.md).
Remote input, network provisioning, labels and stable
cross-device bond fingerprints remain unavailable in this batch. This is a
source/host/build candidate; device and operating-system compatibility are
not qualified.

## Build and inspect

Use a fresh shell with Git, Bash, and Python 3.12 on PATH. From the repository
root, install the dedicated SDK once using the commands below. ESP-IDF 5.5.3
matches the FoloToy BSP's SDK requirement. The existing Mosaico SDK has a
separate installation.

```sh
git init .tools/passport-esp-idf
git -C .tools/passport-esp-idf remote add origin https://github.com/espressif/esp-idf.git
git -C .tools/passport-esp-idf fetch --depth 1 origin 2c211b236707889e8400c4dc5644dd5c4ee071e0
git -C .tools/passport-esp-idf checkout --detach FETCH_HEAD
git -C .tools/passport-esp-idf submodule update --init --recursive --depth 1

export IDF_TOOLS_PATH="$PWD/.tools/passport-espressif"
.tools/passport-esp-idf/install.sh esp32c3
python3 .tools/passport-esp-idf/tools/idf_tools.py install cmake ninja
```

Run the build and size checks from the repository root in Bash. A first
build fetches dependencies using [the manifest](main/idf_component.yml)
and [the lockfile](dependencies.lock). SDK/tool downloads, generated
configuration, and build output are local.

```bash
set -e
export IDF_TOOLS_PATH="$PWD/.tools/passport-espressif"
export IDF_COMPONENT_CACHE_PATH="$PWD/.tools/passport-components"
test "$(git -C .tools/passport-esp-idf rev-parse HEAD)" = 2c211b236707889e8400c4dc5644dd5c4ee071e0
source .tools/passport-esp-idf/export.sh
idf.py --version
idf.py -C passport reconfigure
idf.py -C passport build
idf.py -C passport size size-components
```

The application binary and linker map are written to
`passport/build/passport_c3.bin` and `passport/build/passport_c3.map`.
Assess flash and static RAM with the map and size reports. SPI DMA buffers,
LVGL allocations, RTOS task stacks, Wi-Fi, and BLE also use heap at runtime.
The build does not measure free heap, fragmentation, stack high-water marks,
or radio coexistence.

For an existing generated configuration, select `ESP_SYSTEM_USE_FRAME_POINTER`
in `idf.py -C passport menuconfig` before rebuilding this revision. The checked-in
defaults select it for fresh configurations. Panic register and frame-pointer
backtrace diagnostics remain enabled. The frame-pointer option adds code and
execution overhead, which is included in this revision's size report.

## Dependency sources and licenses

- [FoloToy AI Passport BSP](https://github.com/FoloToy/ai-passport/tree/c27304afe8c4a704b86699f1f6382bde91db86a6/components/bsp)
  is consumed at commit `c27304afe8c4a704b86699f1f6382bde91db86a6`.
  Its [MIT notice](licenses/folotoy-ai-passport.txt) accompanies this project.
- [ESP-IDF 5.5.3](https://github.com/espressif/esp-idf/tree/2c211b236707889e8400c4dc5644dd5c4ee071e0)
  supplies the SDK and controller binaries under its Apache-2.0 and
  bundled third-party notices.
- The lockfile pins LVGL 9.5.0 (MIT) and Espressif's button 4.2.0,
  esp_lvgl_port 2.9.0, esp_codec_dev 1.6.2, and cmake_utilities 1.1.1
  (Apache-2.0). Each distribution carries its own license. The upstream BSP
  brings in the codec component at build time; the application calls only
  the display and button APIs.

## Local interaction

The device starts on Lock. Release the buttons, then hold OK to unlock.
UP/DOWN clicks change focus and an OK click selects it. Home opens Mouse,
Keys, Connections, or Status. Connections contains Bluetooth, read-only
Wi-Fi details, and Appearance. Hold DOWN from any page to lock and request release.
An unlocked OK hold returns to the parent page, except on Mouse where
a direction hold repeats movement.
Double clicks are consumed. The screen dims after 60 seconds of inactivity
and locks input; the first complete waking gesture only wakes the screen.
The BSP provides the 500 ms long-press threshold.

Bluetooth lists up to three saved identity addresses, plus Pair new and Back.
Pair new displays its explanation before requesting a 60-second Just Works
pairing window. Leaving that page or locking closes new-pair admission. Advertise on a saved host opens a 60-second window restricted
to that exact bond. It does not scan for computers or phones. Disconnect
retains the bond; Forget has a Cancel-first confirmation and removes only
the selected identity. Re-pairing requires explicit forget. Full capacity,
unsupported passkey/numeric-comparison methods, and storage errors are
reported without silently evicting a bond or approving a number.

Mouse is a focusable Up/Left/Click/Right/Down grid and Back. Each OK click
requests one 12-unit relative step or left-button tap. A direction hold
repeats at most once per 100 ms after the BSP long event, with no catch-up
burst and a two-second limit from the original press. Release, navigation,
lock, connection generation change, loss of readiness or queue pressure
stops repetition. Another repeat needs a fresh press after release. The
center click and keys never repeat. Keys offers Esc, Enter,
Tab, Backspace, and four arrows, each as a down/up tap. A limited list scrolls
by focus. The connection identity and readiness appear before input actions.
The firmware requires a saved bond, encryption, both report subscriptions,
and completion of the shared engine's two-report neutral barrier. Input is
local-owner-only with a three-second lease and no automatic input replay.

The NimBLE task owns the engine and all BLE changes. The BSP callback queues
16 timestamped button events; the UI queues at most eight copied commands.
The owner checks connection generations and fresh bond-list revisions.
Page changes, lock, expiry, transport loss, queue pressure, and host changes
cancel queued input. Management waits for both zero reports before
requesting disconnect, with a 300 ms limit; unavailable or exhausted release
is marked unknown. Successful notification submission means NimBLE accepted
the report, not that the host OS processed it.

Bonds are saved in one NVS snapshot under `passport_bonds`, using the pinned
SDK's record ABI. Unknown schema, restore failures, and uncertain commits
make the store unavailable; the application never erases NVS to recover.
The old snapshot may remain after a failed deletion, so an uncertain forget
requires restart and inspection. Saved rows show the bond's resolved public
or static-random identity, never a transient RPA. Names/labels are not row
identifiers. See [API.md](API.md) for the later fingerprint/label boundary.

## Native interface and appearance

The 240 x 320 interface uses retained LVGL cards, directional controls,
keycaps, a BLE/Wi-Fi status strip, and a fixed UP/OK/DOWN footer. Focus has
both a thicker border and a changed fill. Bluetooth cards keep the canonical
identity as secondary text; a check and Connected host title mark the actual
connected saved identity. Pairing has a countdown, removal starts on Cancel,
and startup, lock, empty hosts and errors have distinct presentations.
The list scrolls by focus and bounded text truncation never changes identity.
No user-label editing or Wi-Fi action controls are exposed in this batch.

Appearance selects Dark or Light with the same three-key interaction and
layout. Dark is the initial default. The `passport_ui/theme` NVS byte is
separate from bonds. A new theme is applied only after a successful write
and commit; an uncertain save leaves the active theme unchanged and shows
an error. A failed load uses Dark and reports the failure in Appearance.
NVS power-loss behavior still needs device verification.

The native renderer owns a fixed pool of 54 objects including the screen.
Only a changed view reaches LVGL; labels are updated only when text changes,
and palette updates preserve the object tree. Fonts are Montserrat 12, 14
and 18. The BSP retains its single 19,200-byte DMA draw buffer. Host object
counts and target static-memory figures do not establish C3 runtime free heap.

## Health and validation

Status shows the existing one-client WPA2 AP's fresh SSID and password,
generated on each boot, and the configured `/health` URL. Station association
remains idle. DHCP, association, radio coexistence, and physical rendering
need on-device verification. No HTTP input or configuration endpoint is
exposed; the sole HTTP route remains `GET /health`.

Health reports startup results, NimBLE synchronization, internal free heap
and largest block, and cumulative observed UP/DOWN/OK presses. HTTP 503
continues until all three press callbacks have occurred, the display and
buttons initialized, the UI view was applied to LVGL, and NimBLE synchronized.
HTTP 200 describes those bootstrap observations, not HID readiness or
continuing device health. Applying a view to LVGL is distinct from physical
screen verification. A failed update is retried after the next bounded
20 ms button wait. The LVGL lock attempt is bounded at 50 ms.

AP credentials stay in static main-task/view storage and LVGL-owned labels. Temporary entropy and configuration copies are explicitly cleared.
Credentials are excluded from HTTP. Checked STA/AP netif creation releases
partial interfaces, Wi-Fi driver and the owned event loop on failure. The
SDK TCP/IP stack stays initialized for this boot. A failed display may make
the ephemeral AP password unavailable.

Run `python3 passport/tests/run.py` for production startup, board, UI, BLE
owner and bond-store tests with substituted SDK/BSP boundaries and the real
shared input engine. AddressSanitizer and UndefinedBehaviorSanitizer cover
network cleanup, render retry, secret clearing, event ordering, lock/wake,
pairing admission, exact identity selection, subscriptions, stale work,
release ordering, lease expiry, transport failure, bounded repeats, theme
restore and NVS failure handling.
The CCCD capacity regression compiles the pinned SDK functions from the
dedicated SDK installed above; nine slots cover Service Changed, keyboard
and mouse subscriptions for all three hosts.
These substitutions do not simulate radio timing, controller privacy
resolution, SDK flash atomicity, or host interoperability.

Run `python3 passport/tests/render/run.py` after fetching the pinned LVGL
with the target build. It uses CMake/Ninja (PATH or the dedicated tools above),
the real production view and widgets, RGB565 software rendering, and SDK/BSP
fixtures. PNG captures and contact sheets go to `.agent/local/passport-ui/render`.
ASan/UBSan, visible-object bounds, fixed object count and unchanged-frame
checks cover both palettes. These are actual LVGL renders, not photographs;
the light palette especially still needs on-device display qualification.

Hardware qualification still requires the actual display and three keys,
pairing/reconnect and individual forget across reboots, subscription behavior
on intended host OS versions, disconnect/release under congestion and power
loss, free heap, largest block, task high-water marks, and Wi-Fi/BLE coexistence.
There are no media services, OTA, web controller, STA credential workflow,
or sound cues in this candidate.
