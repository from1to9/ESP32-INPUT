# Passport ESP32-C3 firmware

This independent ESP-IDF project is the first buildability batch in the
[Passport compatibility plan](../docs/passport-compatibility-plan.md). It
starts the FoloToy 240 × 320 display and three ADC buttons, the NimBLE host
and controller, Wi-Fi in STA/AP mode, and an on-board read-only HTTP listener
in one image. It uses the Passport 8 MiB flash layout with NVS, PHY data,
and a factory application.

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

## Health image behavior

The display shows the last observed UP, DOWN, or OK press and the configured
health address. The one-client WPA2 AP generates a fresh SSID and password
on each boot and displays them locally. Station initialization leaves
association idle. The AP and URL are labeled as configured. DHCP and client
association need on-device verification. Once associated, open the displayed
`http://<address>/health` URL. The sole HTTP route is `GET /health`.

The page reports initialization results, NimBLE synchronization, current
internal heap, and the largest block. Button setup success leaves each key
pending until its actual press callback is observed. This covers the pinned
button component's unchecked timer creation/start results without changing
the dependency. Press observations are cumulative since boot. HTTP 503
continues until all three keys have produced a press, display initialization
and status-text application have succeeded, and NimBLE is synchronized.
HTTP 200 describes those checks. Physical rendering, ongoing key polling,
AP association, and DHCP reliability still require hardware observation.

Status text uses a fixed persistent buffer under the LVGL lock. A timed-out
update is retried on the next main-loop iteration, with a one-second queue
wait between attempts. Application of text to LVGL is reported separately
from physical display validation. AP credentials reside in main-task-owned
static storage and the LVGL-locked text buffer. Temporary random bytes and
the AP configuration copy are explicitly cleared after use. Credentials
remain in RAM for the screen and Wi-Fi driver during this boot.

STA/AP netif creation uses checked allocation, attachment, and handler
registration APIs. Failures release the partially created interfaces, Wi-Fi
driver, and owned event loop. The global TCP/IP stack remains initialized
for this boot, as ESP-IDF does not support its deinitialization. Later
asynchronous DHCP/interface failures remain unverified in this minimal
image. A display initialization failure can also prevent retrieving the
ephemeral password, so the HTTP diagnostic may be unreachable.

Run `python3 passport/tests/run.py` for focused host fault tests. They exercise
the production startup and board adapter with substituted SDK/BSP boundaries,
covering network cleanup, display lock retry and text lifetime, credential
clearing, and observed-key HTTP status. The tests use AddressSanitizer and
UndefinedBehaviorSanitizer.

The image has no HID service or advertising, browser input, STA credential
workflow, production UI, media services, or OTA. Hardware still needs to
confirm the display and all three buttons, AP association and HTTP behavior,
NimBLE synchronization, heap and stack headroom, and radio coexistence
under traffic.
