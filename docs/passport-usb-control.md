# Passport physical USB control

Passport C3 provides a local control API on its existing USB Serial/JTAG console.
It shares the port with diagnostic logs and flashing. It does not create USB HID,
a second endpoint, a command task, or a driver ring buffer. Only the UI task reads
stdin; radio commands still execute on the existing BLE and Wi-Fi owners.
The pinned SDK's no-driver RX function polls the hardware FIFO without waiting.
Its VFS flag is deliberately kept clear of `O_NONBLOCK`: that separate branch
checks only an installed driver's ring and otherwise skips FIFO reception.
Initialization refuses a preinstalled driver instead of risking a blocking UI read.

Physical possession of this USB connection authorizes explicit local changes,
including unlock, bond/profile deletion and confirmation of exact Wi-Fi credentials.
No device button or UI focus is needed. Existing HTTP permissions, local network
setup confirmation, input fault locks, storage uncertainty, revision checks and
neutral input release remain in force. A host OS can still require its own Bluetooth
pairing permission. USB control cannot provide another computer's OS authorization.

The C3 has no native USB HID, Bluetooth Classic/audio, microphone/recording,
motion/touch sensor or Mosaico media support. Firmware flashing remains an external
esptool operation; this protocol has no flash, arbitrary memory, NVS dump, factory
erase, reboot or shell command. USB/API availability also depends on firmware and
the UI loop running. The known AP driver allocation panic is not repaired by this API.

## Host CLI

Use Python 3.9+ and pyserial 3.5 (also present in the pinned Passport SDK Python
environment). Install the dependency in your chosen virtual environment if needed:

```sh
python -m pip install pyserial==3.5
python tools/passport_usb.py --port /dev/cu.YOUR_PASSPORT_PORT status
```

The port is mandatory. Close a serial monitor before opening it. The CLI requests
exclusive access on POSIX, sets DTR/RTS false before opening, and never deliberately
pulses reset, scans other devices, flashes, unlocks, reconnects or retries a mutation.
Some OS/driver implementations can change modem lines on open; a reset is never
treated as success. The command's boot nonce prevents old requests from executing
after a new boot. There is one writer per port; exclusivity is cooperative on some
platforms and does not replace ownership coordination.

Each invocation obtains `hello`, then sends the requested command exactly once.
JSON stdout contains `boot`, `id`, `accepted`, `result`, `detail`, and `data` records.
`accepted` records whether the queue ACK was observed, not whether the host app
handled an input. It can be false if an ACK was lost but a final reply was received.
Exit 0 means final `applied`; 2 means a final rejection/failure; 3 means an unavailable
transport/handshake or an unknown command outcome. Failure output includes its phase.
Console logs are discarded, never mixed into JSON. Default final timeout is 75 s;
`--timeout` accepts 1..120 s. A local timeout does not cancel a device operation.

Read `status` first. Use its current generation/revisions explicitly; the CLI does
not silently refresh and replay rejected work. For example, with BLE generation 4
and input revision 9, Enter is:

```sh
python tools/passport_usb.py --port /dev/cu.YOUR_PASSPORT_PORT key 4 9 0 40
```

If locked, issue `unlock GENERATION`, observe status, then explicitly `takeover
GENERATION` when changing from Web control or another mode. Read the new input
revision before sending input. `release_required` means the mode/ownership changed
and requires a fresh request; the original input was not queued.

Wi-Fi join selects an exact scan row, not a free-form SSID. Pass the station password
through stdin so it is absent from argv and shell history:

```sh
python tools/passport_usb.py --port /dev/cu.YOUR_PASSPORT_PORT --password-stdin wifi.join SCAN_REVISION ROW < private-passphrase.txt
```

The file contains one passphrase line; an empty line selects an open network. Protect
and remove such files according to your own credential handling policy. Input passwords
are never returned or logged by this API. Firmware wipes the parser, local decoded
submission, USB mailbox and owner incoming copy; a USB password never enters retained
FreeRTOS queue storage. Normal active/persisted Wi-Fi owner credentials remain necessary
for connection. Python/OS/pyserial immutable copies are not guaranteed to be securely
erased. `wifi.ap_credentials` is a separate, explicit physical read of this boot's
generated AP password, never a read of a saved station password; protect its JSON output.

## Operations

All numeric arguments are unsigned decimal unless specified. Indexes are zero-based.
Strings are byte-preserving hex; `-` represents an empty string. SSID bytes are not
assumed to be UTF-8. Label validation follows the existing 16-scalar/48-byte contract.

| Operation | Arguments | Final observation |
| --- | --- | --- |
| `hello` | None | Protocol version, firmware version as hex, current boot, last sequence, pending request |
| `status` | None | USB capabilities, BLE state/revisions, Wi-Fi/AP/DHCP/IP, setup, theme, sound, label revisions |
| `result` | Request ID | Retained mutation receipt, or `unknown` if no longer retained |
| `ble.peer` | Bond revision, index | Exact address type/address and stable bond ID |
| `wifi.profile` | Profile revision, index | Saved profile ID, raw SSID and security |
| `wifi.row` | Scan revision, index | SSID, BSSID, auth, security, RSSI, channel and matching profile |
| `wifi.ap_credentials` | None; unlocked | This boot's generated AP SSID/password |
| `lock` | None | Lock asserted and required neutral reports accepted; otherwise uncertain/failure |
| `unlock` | BLE generation | Lock cleared; unavailable buttons/emergency faults still prevent unlock |
| `cancel` | BLE generation | Queued input cancelled and required neutral reports accepted |
| `takeover` | BLE generation | Local Mouse ownership and required neutral release |
| `ble.pair` | BLE generation, window milliseconds (1000..60000) | Pairing advertisement opened, not a completed host bond |
| `ble.close` | None | Pairing closed, advertisement stopped, pending new handshake ended |
| `ble.target` | BLE generation, bond revision, peer index | Exact saved peer connected with encrypted/bonded/subscribed HID ready |
| `ble.disconnect` | BLE generation, bond revision, peer index | Selected peer disconnected; bond retained |
| `ble.forget` | BLE generation, bond revision, peer index | Exact bond removed; label cleanup uncertainty reported |
| `mouse.move` | BLE generation, input revision, X, Y (-127..127) | Movement report accepted by BLE transport |
| `mouse.scroll` | BLE generation, input revision, wheel (-127..127) | Scroll report accepted by BLE transport |
| `mouse.click` | BLE generation, input revision, button mask (1..7) | Press and release accepted by BLE transport |
| `key` | BLE generation, input revision, modifier (0..255), HID usage (4..101; 0 with nonzero modifier) | Press and release accepted by BLE transport |
| `wifi.scan` | Scan revision | Owner received scan completion and collected results |
| `wifi.scan_cancel` | Scan revision | Cancel requested and stale scan completion drained |
| `wifi.use_ap` | None | AP started, DHCP running, station mode no longer desired |
| `wifi.connect` | Profile revision, profile ID | Matching saved profile online with IP |
| `wifi.disconnect` | Profile revision, active profile ID | Station disconnected and AP/DHCP ready |
| `wifi.forget` | Profile revision, profile ID | Profile deleted durably; active-profile AP fallback may continue |
| `wifi.join` | Scan revision, row, password hex (CLI uses stdin) | Exact scan target online, profile saved after matching association/IP |
| `wifi.setup_open` | None | Provisioning window open on AP/DHCP |
| `wifi.setup_close` | None | Setup window and pending unconfirmed submission closed |
| `wifi.confirm` | Setup ID, pending request ID | Exact pending network online and profile saved |
| `wifi.reject` | Setup ID, pending request ID | Exact pending submission cancelled |
| `theme` | 0 dark / 1 light | Preference saved and UI theme state updated |
| `sound` | 0 off / 1 on | Preference known and output OFF/READY observed |
| `sound.cue` | 1 confirm / 2 success / 3 error | All cue frames accepted by speaker writes; not acoustic verification |
| `name.ble` | Label revision, bond revision, peer index, optional label hex | Read label if omitted; otherwise wait for owning catalog's durable result |
| `name.profile` | Label revision, profile revision, profile index, optional label hex | Same, for a saved Wi-Fi profile |
| `name.ap` | Label revision, scan revision, row, optional label hex | Same, for an exact scanned AP; use `name.profile` for profile-backed rows |

For name writes, `-` clears the alias. Label reads return `label=-` when absent;
the supplied object revision is checked, while label revision governs writes.
Security values are 0 open, 1 WPA2-compatible, 2 unsupported. Theme values are above.
Sound output values are 0 OFF, 1 STARTING, 2 READY, 3 STOPPING, 4 UNAVAILABLE.
Setup request states are 0 none, 1 waiting, 2 joining, 3 saved, 4 failed, 5 cancelled.
BLE/input `applied` never proves receipt by the remote OS or behavior in Notes/another
app. Read actual state after uncertainty; do not infer replay is safe.

## Wire format and lifecycle

```text
RS P1 BOOT_ID REQUEST_ID req OPERATION [ARG ...]*CRC16 LF
RS P1 BOOT_ID REQUEST_ID ack accepted*CRC16 LF
RS P1 BOOT_ID REQUEST_ID data RECORD [key=value ...]*CRC16 LF
RS P1 BOOT_ID REQUEST_ID done RESULT detail=NUMBER*CRC16 LF
```

`RS` is one byte 0x1e immediately followed by `P1` (no space between them).
Boot and request IDs are exactly eight hexadecimal digits. CRC16-CCITT uses initial
0xffff and polynomial 0x1021 over `P1` through the byte before `*`, excluding RS;
the suffix is four hex digits. `123456789` yields 0x29b1. Only single spaces separate
tokens. Frames are at most 255 bytes on the wire before optional CRLF translation;
firmware uses 256-byte storage including NUL, and allows at most 12 operation/argument
tokens. The host emits LF; replies may be CRLF. Parser overflow, non-ASCII, malformed
CRC and partial-frame inactivity of one second discard the entire frame without
echoing it. A new RS resynchronizes. No malformed request receives a success reply.

`hello` uses boot zero and any nonzero request ID. Other requests use the returned
boot nonce and a monotonically increasing nonzero request ID, without wraparound.
All valid requests consume sequence numbers, including reads and rejected commands.
One pending mutation and its most recent receipt are retained. Repeating that
mutation's ID returns its receipt regardless of payload and never executes again.
Older requests return `stale`; `result` can read the retained receipt with a new ID.
This is volatile, one-boot deduplication, not durable exactly-once delivery.

Read commands remain available while a mutation is pending. `lock`, `cancel`,
`ble.close`, scan cancel, setup close/reject and sound off can supersede a pending
mutation. The old request reports `unknown` because it may already have produced
side effects; its receipt is then replaced. Other concurrent mutations return `busy`.
Owner actions can also be superseded by native/Web work. The API never cancels an
operation just because USB disconnected. A lost reply, transport failure, reboot,
owner deadline or missing completion is `unknown`, not success. Obtain a fresh hello
after reconnect and inspect current state before deciding what to do next.

`ack accepted` means admission only. `done applied` is operation-specific as listed
above. Other results include `invalid`, `stale`, `locked`, `busy`, `unavailable`,
`storage_uncertain`, `cancelled`, `unknown`, `timeout`, and `release_required`.
`detail=0` means no additional native detail, not an inferred successful SDK call.
Final receipt deadlines are bounded (up to 70 seconds); the underlying owner may
continue recovery. No session token is exported over HTTP and CRC is error detection,
not authentication or confidentiality.

## Validation and limits

```sh
python passport/tests/run.py startup ble wifi sound usb
python passport/tests/test_usb_cli.py
python tools/check_repository.py
```

The CLI test opens only a freshly created pseudoterminal and requires pyserial.
Host checks exercise production framing/dispatch and owner state machines with
SDK boundaries substituted. The pinned C3 build and compiler stack reports cover
source integration and resource budgets. They do not establish real USB latency,
host compatibility, display pixels, audible sound, radio coexistence or immunity to
the existing AP panic. Device validation and flashing are separate, explicit work.
