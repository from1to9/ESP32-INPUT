# Passport ESP32-C3 firmware

This independent ESP-IDF project implements the local BLE HID and Wi-Fi owner batches
and local HTTP provisioning and direct HTTP/WS input of the [Passport compatibility plan](../docs/passport-compatibility-plan.md).
It combines the FoloToy 240 × 320 display, three ADC buttons, BLE keyboard
and relative mouse, persistent bonds, a Wi-Fi setup page, and a read-only health
listener. It uses the Passport 8 MiB flash layout with NVS, PHY data, and a
factory application. It reuses `input_core`, `hid_engine`, `interaction`,
`control_protocol`, and the shared `app_remote_input` owner. The ordinary
Mosaico protocol initializer retains enrolled TLS and its existing capacities.

The implemented subset and internal API are documented in [API.md](API.md).
The board-hosted Web controller, narrow BLE management, Wi-Fi scans and
HTTP display-label editing are integrated. Bond IDs are device-local, not
cross-device identities. This is a source/host/build candidate; device and operating-system compatibility are
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
defaults select it for fresh configurations. Also enable `HTTPD_WS_SUPPORT`,
`HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`, and set `BT_NIMBLE_HOST_TASK_STACK_SIZE`
to 6144 for an existing configuration. Panic register and frame-pointer
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
  (Apache-2.0). Each distribution carries its own license.

## Local interaction

The device starts on Lock. Release the buttons, then hold OK to unlock.
UP/DOWN clicks change focus and an OK click selects it. Home opens Mouse,
Keys, Connections, or Status. Connections contains Bluetooth, Wi-Fi, Sound,
and Appearance. Hold DOWN from any page to lock and request release.
An unlocked OK hold returns to the parent page, except on Mouse where
a direction hold repeats movement.
Double clicks are consumed. Outside a bounded local Wi-Fi setup window, the screen dims after 60 seconds of inactivity
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
shared between local and network owners with a three-second maximum lease
and no automatic input replay. A physical press takes over an active network
controller and consumes ordinary input from that gesture; release and press again for local input.

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
identifiers. See [API.md](API.md) for the stable identity and label interface.

## Native interface and appearance

The 240 x 320 interface uses retained LVGL cards, directional controls,
keycaps, a BLE/Wi-Fi status strip, and a fixed UP/OK/DOWN footer. Focus has
both a thicker border and a changed fill. Bluetooth cards keep the canonical
identity as secondary text; a check and Connected host title mark the actual
connected saved identity. Pairing has a countdown, removal starts on Cancel,
and startup, lock, empty hosts and errors have distinct presentations.
The list scrolls by focus and bounded text truncation never changes identity.
Saved hosts, saved Wi-Fi profiles, and scanned APs show an available display
label with the original name or identity underneath. Open Host details or
Network details to see the full label and the original identity. The current
Montserrat fonts define the accepted glyphs. The buttons display labels
and navigate to details. Edit labels through the Connections view or typed Web/API operations.

Appearance selects Dark or Light with the same three-key interaction and
layout. Dark is the initial default. The `passport_ui/theme` NVS byte is
separate from bonds. A new theme is applied only after a successful write
and commit; an uncertain save leaves the active theme unchanged and shows
an error. A failed load uses Dark and reports the failure in Appearance.
NVS power-loss behavior still needs device verification.

Sound offers Off, On, Test cue, and Back. Off is the default on a fresh
installation. The preference is stored in `passport_ui/sound` and restored
on boot. The screen shows the saved setting and current output state
independently. An unconfirmed Off save stops output for this session and
reports uncertainty about the next boot. Test cue plays a short fixed-volume
tone when the output is ready. Selected menu actions, observed BLE input
readiness or Wi-Fi IP acquisition, and certain errors have distinct tones.
HID hold-repeat is silent.

Only On starts the cue task. It uses the BSP's shared I²C bus and ES8311
speaker wiring, 16 kHz stereo I²S TX, and three 128-frame DMA buffers. The
[ES8311 register definitions](https://download.kamami.pl/p1199750-ES8311.DS.pdf)
are used to keep the codec ADC reset, unclocked, and powered down across
the pinned DAC driver's writes. Cues last 32–64 ms and one pending cue is
retained when output is busy. Off stops playback and releases the task and
output resources. The shared bus belongs to the BSP. If firmware cannot
identify the codec or initialize output, the screen reports Speaker
unavailable. The codec ID check cannot detect a disconnected speaker.

The native renderer owns a fixed pool of 58 objects including the screen.
Three scan metadata labels keep security, signal, and saved status visible
alongside the label and original SSID.
Only a changed view reaches LVGL; labels are updated only when text changes,
and palette updates preserve the object tree. Fonts are Montserrat 12, 14
and 18. The BSP retains its single 19,200-byte DMA draw buffer. Host object
counts and target static-memory figures do not establish C3 runtime free heap.

## Health and validation

Wi-Fi prefers the last working 2.4 GHz station profile on boot. Four profiles
fit in the separate `passport_wifi/profiles` NVS snapshot, with monotonic
profile IDs and original SSID bytes; display text is never an identity or a
credential. Display labels attach separately to stable profile IDs. Unknown
storage or uncertain writes are reported and prevent further profile changes.
A failed join keeps the previous working snapshot. New credentials are saved
only after an exact association and an observed IP event consistent with the
live network interface address.

The native Wi-Fi page offers Scan, Saved networks, Use Passport Wi-Fi, and
status. Up/Down clamp at scan-list ends. Scans are asynchronous, limited to
six seconds and sixteen retained strongest results; cancellation waits for
the SDK scan-done event before another scan. AP+STA scanning leaves the AP
running. Saved-profile Connect and Forget require Cancel-first local
confirmation; Disconnect checks the exact active profile. WPA2-Personal and
open networks are supported. Hidden SSIDs, WEP, enterprise and WPA3-only
networks are explicitly unsupported. Original non-ASCII SSIDs use a complete
hex spelling in confirmation because the current device font is Latin-only.

A saved LAN gets at most three connection attempts. Automatic AP fallback
requires thirty seconds of sustained loss; one transient event does not
switch networks. Explicit Use Passport Wi-Fi selects the AP immediately.
Network transitions wait for BLE input cancellation and neutral release,
then stop/start Wi-Fi across observed stop events. Failed release leaves
the old network in place. The temporary AP has one client and fresh WPA2
credentials each boot; only its native details and Wi-Fi join QR expose them.
A separate address QR opens the device page, with manual SSID/password/IP
fallback. AP start, DHCP server running, client association and assigned
lease are distinct observations, as are STA association and assigned IP.

For a new network, use the native Wi-Fi page to select Use Passport Wi-Fi,
then Open setup. Join that AP using the on-device Wi-Fi QR or manual
credentials, and open the separate page-address QR (`http://192.168.4.1/setup`
with the actual AP address shown on screen). The setup window lasts two
minutes. The embedded page offers a bounded scan, network selection and
password entry; it works without internet or external assets. Confirm the
same raw network identity and decimal request number on the device. Cancel
is initially selected, and confirmation requires a fresh OK press. No full
password is entered with the three keys. Lock, expiry, a network switch or
provider removal closes the window and clears pending credentials.

Setup assets and API are AP-only, including when the listener also serves
health over LAN. An associated station, wrong local socket address, unexpected
Host or foreign Origin is rejected. A token from the physically opened window
is required for scans and submissions and is checked again by the Wi-Fi owner.
HTTP queue admission is displayed separately from owner acceptance, local
confirmation, association/IP and a successful profile save. Passwords are
cleared from the form after submission and are not retried automatically.
The AP can disconnect during a confirmed join; check the device for the final
result and new LAN address. Network loss alone never means the profile saved.

The page adapts to desktop, phone and tablet widths, using the native dark
and light color palettes. Its initial theme follows the browser preference;
the explicit choice is stored in that browser only. It does not change the
device's saved Appearance setting. No password or setup token is stored in
browser storage. Scan names are literal text with a raw hexadecimal identity
view; hidden and unsupported networks cannot be submitted.

Real AP association/DHCP, phone QR scanning, station reconnect, radio
coexistence, flash power-loss behavior and physical rendering remain untested.

Health reports startup results, NimBLE synchronization, internal free heap
and largest block, and cumulative observed UP/DOWN/OK presses. HTTP 503
continues until all three press callbacks have occurred, the display and
buttons initialized, the UI view was applied to LVGL, and NimBLE synchronized.
HTTP 200 describes those bootstrap observations, not HID readiness or
continuing device health. Applying a view to LVGL is distinct from physical
screen verification. A failed update is retried after the next bounded
20 ms button wait. The LVGL lock attempt is bounded at 50 ms.

AP credentials stay in Wi-Fi owner storage and native view/LVGL labels. Temporary entropy and configuration copies are explicitly cleared.
AP credentials are excluded from HTTP. Router passwords are accepted only by
the AP setup submission and are never returned in responses or health, placed
in URLs, or logged. Checked Wi-Fi owner startup releases partial interfaces, queues, event
handlers, Wi-Fi driver and the owned event loop on failure. The
SDK TCP/IP stack stays initialized for this boot. A failed display may make
the ephemeral AP password unavailable.

Run `python3 passport/tests/run.py` for production startup, board, UI, BLE
owner and bond-store tests with substituted SDK/BSP boundaries and the real
shared input engine. AddressSanitizer and UndefinedBehaviorSanitizer cover
network cleanup, render retry, event ordering, lock/wake,
pairing admission, exact identity selection, subscriptions, stale work,
release ordering, lease expiry, transport failure, bounded repeats, theme
restore, Wi-Fi state transitions, NVS failure handling, and sound preference
and output failures. The setup suite
compiles the pinned SDK cJSON parser and exercises AP/Host/Origin/window/token
admission, bounded flat JSON, stale selections, queue results and allocation
failures. Its socket, HTTP and radio boundaries remain substituted.
The Sound suite requires the managed dependencies from the target build. It
runs the pinned ES8311 codec driver against I²C and TX fixtures.
The CCCD capacity regression compiles the pinned SDK functions from the
dedicated SDK installed above; nine slots cover Service Changed, keyboard
and mouse subscriptions for all three hosts.
These substitutions do not simulate radio timing, controller privacy
resolution, SDK flash atomicity, or host interoperability.

Run `python3 passport/tests/render/run.py` after fetching the pinned LVGL
with the target build. It uses CMake/Ninja (PATH or the dedicated tools above),
the real production view and widgets, RGB565 software rendering, and SDK/BSP
fixtures. PNG captures and contact sheets go to `.agent/local/passport-wifi/render`.
ASan/UBSan, visible-object bounds, fixed object count and unchanged-frame
checks cover both palettes. These are actual LVGL renders, not photographs;
the light palette especially still needs on-device display qualification.

Run `node passport/tests/web/check.mjs` with Playwright and Chromium installed
for browser checks of the production embedded assets. Optional
`PASSPORT_PLAYWRIGHT` and `PASSPORT_CHROMIUM` select existing installations;
`PASSPORT_WEB_OUTPUT` selects the local screenshot directory. The test starts
and closes a loopback fixture server. It covers desktop, iPhone and iPad
viewport sizes in both themes, password/open-network submission, exact
request receipts, storage uncertainty, stale scans, literal SSIDs, closed
windows and theme persistence. Mobile viewports do not establish Safari or
physical phone/tablet compatibility.

Hardware qualification still requires the actual display and three keys,
pairing/reconnect and individual forget across reboots, subscription behavior
on intended host OS versions, disconnect/release under congestion and power
loss, free heap, largest block, task high-water marks, and Wi-Fi/BLE
coexistence. Speaker output, loudness, Off/On across physical power cycles,
and cue behavior during concurrent input and radio activity need device testing.
There are no network media services, OTA, phone motion input or USB HID in this target.

## Direct network input

The board's observed LAN or AP IP accepts the [open session API](API.md#direct-http-and-websocket-input).
Anyone who can reach this listener can request input; bearer tokens bind
sessions, request results and leases, not trusted clients. HTTP does not
provide confidentiality or integrity against an on-path peer. The display
shows `Network control: open`, the active controller, IP and Lock. Lock
rejects remote mutations while status and local recovery remain available.
No HTTP downgrade is enabled on Mosaico.

Passport uses two sessions, sixteen retained results, four active requests,
four remote jobs, a 2048-byte input frame and a single transport mailbox.
The compact parser and owner settings apply only to the Passport target.
Network callbacks never call the engine: the NimBLE task processes one
bounded mailbox item per 20 ms service and remains the sole report sender.
Timeout leaves an explicit unknown outcome and prevents mailbox reuse until
the owner has finished or discarded the request. Session/WS loss, network
change, Lock, local takeover and target changes use the shared release path.

Run `python3 passport/tests/control.py` for the real protocol/remote-owner/HID
chain, including mode changes, maximum text, exact duplicate replay, partial
text results, local takeover and release boundaries. The `control_http`
suite in `passport/tests/run.py` exercises production ingress, pre-handshake
WS authentication, bounded frames, owner timeout and backpressure with
substituted socket/RTOS boundaries. Use `--control-only` with the native
renderer for the changed status, active-controller and Lock views.


## Browser control

Open the numeric LAN or AP IP shown on Passport, then select **Connect
browser** and **Take control**. Unlock on the device and pair the intended
host from its Bluetooth settings first. The page works directly from the
board and contains all its assets. Mouse, Keyboard and Connections have
separate navigation; phone portrait uses one column, while desktop/tablet
layouts provide a touchpad or keyboard beside host/result cards.

Mouse offers a relative touchpad, all three click buttons, scrolling and
hold-to-drag. With touch, hold Drag with one finger and move another on the
pad; with a mouse, hold Drag and move the pointer. Keyboard offers explicit
keys, bounded US ANSI text and Ctrl/Command shortcuts. Choose Command for
Mac/iPad/iPhone hosts. Text is never silently shortened, and a reported HID
send does not prove the host application acted. **Stop** is always visible
while connected. Loss of focus, page visibility, network, session or target
ends held input; reconnect does not replay or resume it.

Connections lists BLE hosts and Wi-Fi saved/scanned rows with original
identities and optional labels. Add device advertises for a new host;
Connect host waits for the selected saved host to become input-ready.
Disconnect host retains its bond and does not disconnect the browser.
Remote Forget and Wi-Fi joining are not enabled: use the explicit native
confirmation or AP setup route explained on the page. Label changes use
stable identities and expected revisions; unconfirmed storage stays visible.
See [the management API](API.md#board-hosted-controller-and-management) for
agent envelopes, IDs, list pagination and asynchronous result meanings.

Appearance defaults to the browser's system theme, with a persistent
Dark/Light/System override. Only that preference is stored in the browser;
session tokens and input remain in memory. The device's own Appearance and
Sound preferences keep their separate native controls.

Run `node passport/tests/web/control.mjs` for actual Chromium renders and
interaction checks at desktop, iPhone and iPad viewport sizes in both themes.
The same `PASSPORT_PLAYWRIGHT` / `PASSPORT_CHROMIUM` installation overrides
as the setup-page suite apply. `PASSPORT_CONTROL_OUTPUT` selects screenshot
output; the default is `.agent/local/passport-web/browser`. The fixture
substitutes HTTP/device observations, including uncertain replies and target
changes; it does not qualify Safari or a physical Apple device. Run
`python3 passport/tests/run.py management ble wifi control_http` for protocol,
management-adapter, actual owner, ingress and failure tests, and
`python3 passport/tests/control.py` for the shared HID command path including
explicit Command versus Ctrl reports. Runtime radio coexistence, heap,
stack high-water, physical keys/display and host outcomes remain unmeasured.
