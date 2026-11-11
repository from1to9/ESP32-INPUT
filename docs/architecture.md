# Architecture

ESP32-INPUT targets the ESP32-S31 on ESP-Mosaico. The
[toolchain lock](../toolchain.lock.json) pins ESP-IDF and the board support
package. Portable components keep product policy independent of board pins,
SDK callbacks and LVGL.

| Layer | Ownership |
| --- | --- |
| Board | The [board port](../firmware/main/board_port.h) describes hardware and capabilities. Mosaico adapters provide touch, display, IMU, buttons, power and codec access. |
| Interaction | The [portable model](../components/interaction/README.md) owns calibration, gestures, the keyboard draft and semantic actions. The coordinator admits work and owns mode, target, input owner and epoch. |
| HID | The [HID engine](../components/hid_engine/README.md) owns the composite descriptor, bounded report work and neutral release. BLE and USB use the same mouse and keyboard contract. |
| UI | The presenter exposes capability and actual state to LVGL. Native controls post intents for the current UI context. See the [UI design](ui/README.md) and [interaction contract](interaction.md). |
| Media | The [audio engine](../components/audio_engine/README.md) owns jobs and PCM flow. The board adapter owns the codec. USB Audio and Classic HFP/A2DP are route adapters. Speech consumes local frames and returns letter candidates. |
| Management | The [LAN API](remote-control.md) and [MCP companion](../bridge/README.md) use the versioned operation registry. Separate owners handle authentication, Wi-Fi, updates, diagnostics and the sensor journal. |

## Input and release

Touch, IMU, AI-button and authenticated remote actions converge on one input
coordinator. A captured epoch, target and visible-context revision prevent
late callbacks from acting on a new owner or page. Lock and local takeover
prioritize neutral releases. A transport switch cancels pending work and
checks old-host release before selecting a new link. Disconnecting USB does
not redirect work to Bluetooth.

Mouse aiming emits relative deltas from timestamped sensor samples. Keyboard
motion moves local focus. A confirmed character enters the draft, which holds
up to 64 printable US ANSI characters. Send consumes the accepted prefix and
leaves the unsent suffix after interruption. It never appends Enter.
See [Mouse aiming](motion-estimation.md) for calibration and recovery.

## Independent routes and services

One selected HID target carries mouse and keyboard reports. The Audio
workspace selects its own USB or Classic route and shows its own profile and
stream state. Saved BLE and Classic records have separate identities.
Transport availability, profile readiness, job progress and observed media
are distinct. See [Bluetooth audio](bt-audio.md) and [validation](validation.md).

Wi-Fi station connectivity leaves the selected HID transport in place. The
provisioning hotspot handles setup. LAN access requires separate enrollment
and scope checks, and remote input requires a current lease. Firmware updates
use signed artifacts, an inactive application slot and a separate activation
step. Maintenance and local release barriers apply before reboot.
See [Wi-Fi and updates](wifi-management-ota.md).

Board support, source implementation and physical compatibility have separate
acceptance checks. See [porting](porting.md) for hardware boundaries and
[validation procedures](test-coverage.md) for reproducible checks.
