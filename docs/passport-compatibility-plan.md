# AI Passport compatibility firmware plan

Status: design proposal. No Passport firmware or hardware qualification is
claimed by this document.

## Two firmware targets in one repository

Keep the current `firmware/` ESP32-S3/S31 Mosaico target and its USB HID,
Bluetooth HID, touch, motion, audio, and enrolled-control behavior. Add a
separate ESP32-C3 FoloToy AI Passport target with its own BSP, SDK
configuration, partition table, BLE adapter, screen, and hardware services.
Only the Mosaico changes necessary to add its requested persistent control
hotspot belong in this work. Do not refactor its unrelated input or media
paths as part of the Passport port.

Reuse the existing `input_core`, `hid_engine`, `interaction`, and
`control_protocol` sources where their contracts apply. The portable
`app_remote_input` owner is a candidate for direct reuse through the new
target's build configuration; confirm its C3 memory footprint and
dependencies before committing to it. If a source must move to a shared
component, change build references without changing Mosaico behavior. Keep
one command vocabulary, lease/owner model, neutral release barrier, stale
request handling, and truthful result meanings across both targets. New
Passport code must not bypass the engine from its web handler, button callback,
or NimBLE callback. Unsupported operations return unavailable.

| Capability | Mosaico S3/S31 | Passport C3 |
| --- | --- | --- |
| USB HID mouse/keyboard | Retain | Unavailable: fixed-function USB Serial/JTAG |
| Bluetooth HID mouse/keyboard | Retain | BLE HID, one active host |
| Device motion and physical touch | Retain | Unavailable: no matching sensors |
| Web touchpad, mouse, keyboard, agent API | Retain existing controller | Required |
| Audio and microphone workflows | Retain | Remove; no Bluetooth audio or capture |
| Local interaction sound | Existing behavior | Optional short speaker cues, default Off |
| STA and persistent control AP | Add AP without changing enrolled policy | STA preferred, AP fallback |
| On-device control | Existing Mosaico UI | New three-button mouse and limited keys UI |

The ESP32-C3 USB Serial/JTAG port stays available for flash and diagnostics;
it cannot be reconfigured as native USB HID. Passport's ES8311 speaker is
used only for local cues. Do not include microphone, recording, media API,
Bluetooth audio, Mosaico motion/touch UI, or incompatible USB/OTA components
in its build. A missing provider is unavailable, never a success stub.

## Network and authorization

Passport joins a saved 2.4 GHz LAN as its normal mode and shows its current
IP. A computer can then reach the board while keeping ordinary LAN/Internet
access. Without usable credentials, it starts its own password-protected AP
and serves the same web controller and input API at its displayed AP IP.
Show a Wi-Fi join QR, a separate page-address QR, and manual SSID/password/IP
text. Scanning a QR does not perform operating-system BLE pairing. The
Passport never needs a desktop relay, cloud service, or external runtime
server. A host using only one Wi-Fi adapter may lose Internet while connected
to the Passport AP.

The AP uses a per-device WPA2 password. A Wi-Fi credential form is available
only on the AP during a short physical setup window. Confirm the exact
pending SSID and request on the screen before committing credentials; a
generic unlocked window does not approve a later or different request.
The locally displayed Wi-Fi join QR may contain the Passport AP password.
The router/STA password never appears in a QR, URL, diagnostic log, or
tracked file.
This HTTP form runs over the WPA2 AP link; it must not be exposed through the
unencrypted STA HTTP interface. Stage AP+STA during joining after closing the
old input lease; save a profile as working only after association and IP
assignment. On failure, retain the previous working profile, reopen the AP,
and show the actual failure. Once STA works, close the AP and show its new IP.
A deliberate local `Use Passport Wi-Fi` choice switches from STA to AP.
For automatic fallback, require a sustained 30-second loss after bounded
reconnect attempts; do not switch on one transient Wi-Fi event. AP channel
changes during AP+STA transition may interrupt browser clients, which must
open a fresh session at the displayed address.

Passport defaults to open input access for clients that can reach its
listener on the LAN, via routing, or after joining its AP. There is no
per-client enrollment or wall-clock authorization. Show `Network control:
open`, active controller, IP, and Lock on the screen. HTTP provides no
confidentiality or integrity against an on-path network peer; Host, Origin,
and CSRF checks do not change that. Open access covers only these exact
input operations: `control.acquire`, `control.renew`, `control.release`,
`mouse.move_relative`, `mouse.click`, `mouse.scroll`, `mouse.drag.begin`,
`mouse.drag.refresh`, `mouse.drag.end`, `keyboard.text.enqueue`,
`keyboard.key_tap`, `keyboard.shortcut`, `mode.select`,
`input.release_all`, and `input.cancel`. The separate allowlist for reads
contains only state, capabilities, operation metadata, and the caller's
request results. Do not infer authority from a broad scope name, provider
presence, or a hidden web control. Reject `ui.*`, `config.*`,
`input.transport.select`, remote unlock, Wi-Fi changes, bond deletion,
firmware update, and media operations. A local Lock rejects new input while
leaving status and recovery pages reachable.

Select Passport's open HTTP/WS policy at build and startup, never through a
request parameter or after a failed Mosaico authentication. Its first-party
page and agents call `POST /api/v1/open-session` with JSON and a required
custom header; the server returns an unpredictable, short-lived bearer token
and session/request identity. A browser keeps the token in memory, sends it
with JSON commands and a server-issued CSRF value, and obtains a single-use
WS ticket through the same session before upgrade. No secure cookie or
WebCrypto proof is assumed on HTTP. An agent uses the same bearer flow at
the board IP. The bearer distinguishes leases and duplicate requests, not
which network client is trusted. Reject unexpected Host/Origin, do not
enable CORS, reject simple-form content types, and bound requests. The
protocol boundary must expose an explicit open-input policy while leaving
Mosaico's current TLS-required initialization and credential behavior
unchanged. Preserve the existing three-second maximum input lease, drag
expiry, session-loss release, target-generation checks, and unknown-result
handling. HTTP and WS share the same session owner. No HTTP fallback is
added to Mosaico's enrolled service.

Mosaico also gains a persistent control AP, separate from its existing
temporary provisioning AP. The new AP must carry its existing HTTPS/WSS
identity, enrolled-client permissions, and input leases; it never inherits
Passport's open policy. Its setup-only AP and previously saved settings must
continue to work. Manual selection of persistent control AP survives reboot
until the user switches back to STA; it does not replace the timed first-boot
provisioning AP. Keep the station certificate hostname. On the AP, DHCP
advertises a local DNS server for the device-specific browser origin, and
native clients may use the separately certified `192.168.4.1` IP. Both
interfaces keep exact Host and Origin checks. An older identity uses
[local USB re-enrollment](../bridge/README.md#control-hotspot-enrollment).
A cold boot without usable time requires an already valid permanent grant.
Time-limited authorization stays unavailable until clock quality returns.
A network switch closes sessions and releases input
before rebinding the listener. The Mosaico UI needs only the new AP
selection/status entry and the network integration required to expose
the existing controller there.

## BLE HID and local controls

Passport advertises BLE HID keyboard and mouse to one host. The user opens
a bounded new-host pairing window on its screen and pairs from the host's
Bluetooth settings. Saved bonds reconnect without reopening new-host
pairing. The screen distinguishes advertising, pairing, bonded, connected,
encrypted, and input-ready. Use the existing Passport NimBLE/esp_hid example
for report and adapter patterns, but do not copy automatic numeric-comparison
approval or automatic bond eviction. Accept Just Works only in the visible
pairing window; reject other methods until a three-button confirmation or
digit-entry flow has been designed and validated. A full bond store requires
an explicit local forget action, which releases input first. Ready means the
required HID service, encryption, bond, subscriptions, and neutral-release
state are usable; a connected link alone is insufficient.

The three-button Bluetooth page lists saved bonded hosts and marks the
currently connected host. It offers Pair new, Advertise for selected host,
Disconnect, Forget selected host, and Back. Selecting Advertise starts a
bounded reconnect window; a phone, tablet, or computer still initiates
the BLE connection from its own stack. BLE HID peripheral mode cannot
enumerate nearby host computers as a normal discovery list. If the stack
cannot restrict advertising to the selected bond, reject an unexpected
host before accepting input and display the actual host that connected.
Forgetting a bond or disconnecting the active host first cancels pending
input and completes neutral release. Show capacity-full and pairing errors
without silently deleting another bond.

The three-button Wi-Fi page has a Scan networks action and a scrollable
2.4 GHz result list. Each row shows a bounded SSID label, security type,
signal strength, and saved/current marker; UP and DOWN clamp at the list
ends. OK opens actions for that exact row. A saved network offers Connect
and Forget with local confirmation. A supported new network offers
Configure via Passport AP: show the AP join QR and web form for its
password, then confirm the exact pending SSID on screen before joining.
The three buttons do not implement full password typing. Refresh, scan
failure, unsupported security, full saved-profile storage, and Back have
explicit states. No list row initiates connection merely by receiving
focus, and a failed join preserves the previous working profile.

The Passport has a 240 x 320 display and UP, DOWN, OK on one ADC ladder.
No two-button chord is required. The BSP callback queues events; UI,
network, and HID work run in their owning tasks. Treat a double click
explicitly and never turn a long press into a short action on release.
The first press after a dark screen only wakes it. A long DOWN press from
any page locks and releases input. Unlock requires a new complete gesture
after the Lock screen appears, so one hold cannot lock then unlock.
Local takeover first cancels and releases remote input; a subsequent
gesture performs the local action. If button initialization fails, the
open network service remains read-only and input stays locked.

| Screen | UP/DOWN click | OK click | OK long |
| --- | --- | --- | --- |
| Home | Focus Mouse, Keys, Connections, Status | Open focus | Open Connections |
| Mouse | Focus Up, Left, Click, Right, Down, Back | Bounded move or left click | Repeat focused direction only while held |
| Keys | Focus Esc, Enter, Tab, Backspace, arrows, Back | Send one key tap | Return Home |
| Connections | Focus Bluetooth, Wi-Fi, Web/API, Sound, Back | Open focus | Return Home |
| Bluetooth | Focus Pair new, saved hosts, Back | Open host actions or pair window | Return Connections |
| Bluetooth host actions | Focus Advertise, Disconnect, Forget, Back | Apply or open confirmation | Return Bluetooth |
| Wi-Fi | Focus Scan networks, saved/current network, Use Passport Wi-Fi, Back | Open list or action | Return Connections |
| Wi-Fi network list | Focus scan result, Refresh, Back | Open exact-row actions | Return Wi-Fi |
| Wi-Fi network actions | Focus Connect/Configure, Forget, Back | Apply or open confirmation | Return list |
| Sound | Focus Off, On, Test cue, Back | Apply or preview | Return Connections |
| Confirmation | Focus Cancel or Confirm; Cancel first | Apply focus | Cancel |

The Mouse page is a focusable directional grid, not a touch screen.
Each OK click sends one bounded relative step; holding OK repeats at a
bounded rate until release, page exit, lock, owner takeover, BLE loss, or
queue pressure. Use the official BSP's 500 ms long-press threshold. Cap a
continuous repeat hold at two seconds, then wait for a release before
accepting another repeat. A missing release therefore times out rather
than running forever.
These are initial timing targets subject to real button/BLE observation,
including BSP press and release callback latency.
The Keys page sends only the small set above. Full text, shortcuts, wheel,
middle/right click, and drag belong to the web controller. Display the
selected BLE target and readiness before every input action. Confirmation
pages always show a Back path and start on Cancel.

The Sound setting persists Off/On, default Off. Use the official ES8311
hardware mapping but an output-only TX initialization for short,
fixed-volume cues; the reference BSP's default full-duplex setup also
initializes RX/microphone and must not be used unchanged. Off allocates no
cue task or PCM buffer. On uses bounded asynchronous work for a completed
selection, connection transition, or error. Never cue each repeat step.
Under pressure, drop the cue rather than delaying HID, Wi-Fi, or BLE;
turning Off or an initialization failure stops and cleans up output.
No audio streaming, recording, microphone, or Bluetooth audio stack is
started for this feature.

## Browser and agent interface

Both targets should present the established mouse/keyboard command meanings.
Passport serves its controller on-device. On desktop, iPhone, and iPad,
the responsive page offers a touchpad, left/right/middle click, wheel,
hold-and-drag, text input, on-screen keys, shortcuts, connection status,
and visible Stop. Use touch-sized controls and support portrait and
landscape; do not rely on hover or a hardware keyboard. Pointer cancel,
lost capture, page hide, session loss, network switch, lock, target change,
and lease expiry stop held input. Browser release on page hide or network
loss is best effort; the device's three-second input lease and drag deadman
are the final stop path. Reconnect never replays an uncertain command or
resumes a drag. Capability discovery hides or disables unsupported
Passport operations. The browser does not use phone motion sensors;
Mosaico keeps its own physical motion mouse.

The agent API remains a direct device IP endpoint. Expose state,
capabilities, an open Passport session, command submission, result lookup,
and a WS input channel. Use the existing operation catalog and envelope
where supported. An acknowledgement means queued or HID transport
acceptance as specified, never proof that the target OS acted.

## Build, review, and delivery

1. Freeze the target capability matrix and project layout. Keep the
   Mosaico application and media sources unchanged except minimal AP
   integration. Add C3-only build configuration and pin the official
   Passport BSP/dependencies and licenses. First compile a minimal C3
   combination of display, three buttons, NimBLE, STA/AP, and an HTTP
   listener, then inspect actual internal-memory use before expanding it.
2. Add Mosaico persistent control AP with its enrolled HTTPS policy,
   matching certificate endpoint, network-switch release, and minimal UI
   entry. Review and publish this batch without altering other Mosaico
   features.
3. Add Passport BLE HID, open-network HTTP input service, STA/AP setup,
   shared input owner, three-button UI, and optional local cues. Review
   and publish in cohesive, buildable batches; a partial target must
   identify unavailable functions honestly.
4. Add the responsive browser and direct agent control, then reconcile
   requirements, API, Wi-Fi, browser, build, and validation docs.
   Independently review each batch before publishing.
5. After the delivered firmware and physical/browser evidence support its
   actual claims, update the root README, both target build/user guides,
   repository description, and focused GitHub topics for discoverability.
   Prepare a bilingual title/summary, cover, public source URL, and verified
   merged Passport image for the official community publisher. Show the
   complete proposed listing to the author before its external upload.

Run focused shared state/failure tests and the Mosaico target build when
its AP integration changes. Build the C3 target with a C3-specific partition
table; do not inherit S31 PSRAM, Bluetooth Classic, USB OTG, or OTA layout.
Measure internal free heap, largest free block, stack high-water marks,
and Wi-Fi/BLE coexistence on a real Passport. Official guidance marks
STA+connected BLE stable and AP+associated client+connected BLE supported
but unstable; AP input therefore needs physical acceptance. Exercise real
desktop, iPhone, and iPad browsers on STA and AP, and actual host pairing
and three-button flows. A build or host test alone does not establish
physical or cross-device qualification.

For every accepted batch, stage exact files, inspect the current remote
head, integrate without force, publish, and verify remote ancestry.
Public commits contain only source, portable documentation, and non-personal
evidence. Keep credentials, pairing keys, private addresses, raw device
logs, personal data, local paths, and performance snapshots out of Git.

## Sources

- [FoloToy Passport BSP](https://github.com/FoloToy/ai-passport)
- [FoloToy hardware guide](https://github.com/FoloToy/ai-passport/blob/main/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
- [Passport BLE HID example](https://github.com/lewangdev/ai-passport-iphone-agent)
- [Passport QR/SoftAP example](https://github.com/kunlun322/Qoder-Passport)
- [ESP32-C3 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html)
- [ESP32-C3 fixed-function USB](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html)
- [FoloToy community publication](https://github.com/FoloToy/ai-passport/blob/main/docs/development/release/publish-to-community.zh_CN.md)
