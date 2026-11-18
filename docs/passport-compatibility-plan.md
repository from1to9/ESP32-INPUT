# AI Passport compatibility firmware plan

Status: staged implementation plan. This document does not claim completed
Passport behavior or hardware qualification.

## Two firmware targets in one repository

Keep the current `firmware/` ESP32-S3/S31 Mosaico target and its USB HID,
Bluetooth HID, touch, motion, audio, and enrolled-control behavior. Add a
separate ESP32-C3 FoloToy AI Passport target with its own BSP, SDK
configuration, partition table, BLE adapter, screen, and hardware services.
Mosaico changes in this work cover its persistent control hotspot,
Bluetooth/Wi-Fi management and labels, and the connected Web controller.
Do not refactor unrelated Mosaico input or media paths as part of the
Passport port.

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

The ESP32-C3 USB Serial/JTAG port stays available for flash, diagnostics and the
[physical USB control API](passport-usb-control.md), which can perform device
operations without three-button confirmation. This does not change network
authorization or bypass input fault locks. The port
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
contains state, capabilities, operation metadata, the caller's request
results, saved BLE hosts, Wi-Fi profiles, current scan results, and
`display_name.list`. Add a separate exact allowlist for `bluetooth.disconnect`,
`bluetooth.target.request`, `bluetooth.pairing.open`,
`bluetooth.pairing.close`, `bluetooth.pairing.cancel_handshake`, bounded
`wifi.scan.start`/`wifi.scan.cancel`, and the typed display-label set/clear
operations defined below. These operations still pass the owning BLE,
Wi-Fi, storage, and lock-state admission checks. Do not infer authority
from a broad scope name, provider presence, or a hidden web control.
Reject `ui.*`, `config.*`, `input.transport.select`, remote unlock,
Wi-Fi credentials/join/forget, bond deletion, firmware update, and media
operations. A local Lock rejects remote mutations while leaving status
and recovery pages reachable.

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

Cold startup restores the last selected saved host and attempts bounded
reconnection without opening new-host admission. Persist successful manual
host selection and a newly bonded host; forgetting that host removes its
selection. Manual host management and Lock override startup reconnection.
The startup window lasts at most 60 seconds with at most three advertising
starts; input never replays. A legacy installation with no selection adopts
its sole bond, while multiple bonds require one explicit selection.

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

The same BLE HID management is available through the device's web page
and API on STA and the control AP. `bluetooth.peers.list` returns saved
hosts, selected target, connection, encryption, subscriptions, pairing
window, and input-ready state with known/updating/error distinctions.
`bluetooth.disconnect` leaves the bond intact; `bluetooth.target.request`
releases old input, disconnects the old host, selects an exact saved bond,
and opens a bounded reconnect advertisement. The caller sees waiting,
timeout, wrong-host, failed, and ready outcomes instead of success at
request admission. `bluetooth.pairing.open/close/cancel_handshake` manage
the new-host window. Web **Add device** starts advertising; the target
computer or phone discovers Passport or Mosaico in its own Bluetooth
settings. It is not a scan of nearby computers by the BLE HID peripheral.
Web/API actions must not reuse or replay a held key, drag, or old input
lease, and disconnecting a Bluetooth host must not imply that the browser
session disconnected. Mosaico retains `connections.manage` enrollment for
these operations; Passport uses only the narrow open-network allowance
above. The operation catalog alone is not evidence that either target
has a wired backend. Show Classic audio peers separately on Mosaico,
with only actions actually supported by its current audio service;
Passport has no Classic audio list or scanner.

On Mosaico, a user disconnect pauses reconnection for that host and keeps
its bond. A target request waits 60 seconds by default after the selection
is saved and accepts a wait of 1–300 seconds. A timeout keeps the saved
selection and returns to ordinary saved-host advertising. BLE link
readiness and the active input route are reported separately. The device
review for Add explains that confirmation clears the selected-host
preference and keeps all bonds. The result confirms advertising after
the controller reports its start. Closing the window stops new admission,
and cancelling a pending handshake keeps the window's original deadline.

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

Both targets support persistent **display labels** for Bluetooth peers and
Wi-Fi networks through the web page and typed API set/clear operations.
The list responses contain both the original name and optional label,
plus an opaque stable identity and revision. Local and web lists prefer
the label; details always show the original name and identity needed to
distinguish duplicate labels. Clearing a label immediately falls back to
the original. Labels never change an SSID, Wi-Fi credential, BLE broadcast
name, bond identity, target selection, or a healthy link.

Bind a BLE HID label to its saved bond fingerprint, not the visible name,
row number, or rotating BLE address. Mosaico Classic audio saved peers use
a separate Classic identity namespace. Bind a saved Wi-Fi label to its
stable profile ID, not scan order. Also permit persistent labels on up to
16 **unsaved** scanned access points, keyed by exact SSID bytes, BSSID,
and auth type. An AP that changes BSSID is a new row; do not silently
move its label. A scanned row first uses its exact AP label; otherwise it
may show a saved profile's label only when one profile matches its exact
SSID and compatible security. Ambiguous rows show the original SSID.
`display_name.list` exposes retained labels, identity keys, and capacity
even when an annotated AP is no longer in the current scan; never evict an
older label silently. Set/clear requires a stable ID and expected list or
store revision. Setting an unsaved scan-row label also rechecks its fresh
generation and exact identity; clearing a retained label can use its
catalog ID without a current scan. Deleting a confirmed profile or bond
clears only its attached label; a failed cleanup remains visible as an
error and cannot relabel a different identity. Unknown/corrupt storage
and failed writes are not reported as an empty list or success. Re-read
reconciliation does not disconnect Wi-Fi or HID.

The API accepts at most 48 UTF-8 bytes and 16 Unicode scalar values per
label, rejects controls, bidi controls, invalid UTF-8, and characters
absent from the compiled device font instead of truncating or showing
blank glyphs. Measure the C3 flash/RAM cost of an appropriate
Chinese-capable font subset before enabling it. The device may ellipsize
long labels in list rows but must show the complete accepted label in a
detail view; web output is HTML-escaped text, never interpreted as markup.
The three-button UI displays labels but does not implement full-text
editing; users or agents edit them through Web/API. Passport must be able
to scan during its AP setup window without dropping the controller; the
Mosaico control AP may report live scans unavailable while still showing
saved profiles and labels.

Mosaico publishes the label catalog at `/api/v1/display-names` in
eight-row pages. Use its revision as the command's `resource_revision`.
Set takes an `owner_revision` for a saved profile or bond, or the current
`scan_generation` and `scan_index` for an exact AP. Clear accepts the
retained kind and ID with the catalog revision. Classic labeling requires
a verified link-key fingerprint. Legacy synthetic Wi-Fi profile IDs and
scan rows without a usable exact SSID are unavailable for labeling.

The Passport has a 240 x 320 display and UP, DOWN, OK on one ADC ladder.
Cold boot opens a navigable Home page when button initialization succeeds.
Manual and fault locks remain available; boot does not send HID. After 60
seconds without local button activity, the screen turns off without locking
or interrupting BLE/Web input. A local Wi-Fi setup window keeps it awake.
Home Mouse and Keys cards are visibly disabled and skipped by focus until
the output owner reports HID ready; a connection alone is insufficient.
Connections and Status remain accessible. A lost ready state moves Home focus
off a disabled card and cancels its pending selection; readiness enables the
cards again. Passport's USB Serial/JTAG does not provide USB HID readiness.
No two-button chord is required. The BSP callback queues events; UI,
network, and HID work run in their owning tasks. Short actions run on confirmed
release; delayed single/double-click notifications do not execute actions.
Quick UP/DOWN taps each move focus. The first short OK release acts once;
subsequent OK gestures within the BSP click window are consumed to avoid
activation through a newly opened page. Never turn a long press into a short
action on release. Retain the BSP ADC confirmation and release filtering.
The first complete gesture after a dark screen only wakes it, without taking
over an active Web controller. A long DOWN press from any page locks and
releases input. Unlock requires a new complete gesture
after the Lock screen appears, so one hold cannot lock then unlock.
Local takeover first cancels and releases remote input; a subsequent
gesture performs the local action. If button initialization fails, the
open network service remains read-only and input stays locked.

| Screen | UP/DOWN click | OK click | OK long |
| --- | --- | --- | --- |
| Home | Focus Mouse, Keys, Connections, Status | Open focus | Open Connections |
| Mouse | Focus Up, Left, Click, Right, Down, Back | Bounded move or left click | Repeat focused direction only while held |
| Keys | Focus Esc, Enter, Tab, Backspace, arrows, Back | Send one key tap | Return Home |
| Connections | Focus Bluetooth, Wi-Fi, Web/API, Sound, Appearance, Back | Open focus | Return Home |
| Bluetooth | Focus Pair new, saved hosts, Back | Open host actions or pair window | Return Connections |
| Bluetooth host actions | Focus Advertise, Disconnect, Forget, Back | Apply or open confirmation | Return Bluetooth |
| Wi-Fi | Focus Scan networks, saved/current network, Use Passport Wi-Fi, Back | Open list or action | Return Connections |
| Wi-Fi network list | Focus scan result, Refresh, Back | Open exact-row actions | Return Wi-Fi |
| Wi-Fi network actions | Focus Connect/Configure, Forget, Back | Apply or open confirmation | Return list |
| Sound | Focus Off, On, Test cue, Back | Apply or preview | Return Connections |
| Appearance | Focus Dark, Light, Back | Save the selected theme | Return Connections |
| Confirmation | Focus Cancel or Confirm; Cancel first | Apply focus | Cancel |

### Visual system and theme acceptance

The original FoloToy hardware-test menu is a reference for the BSP, not an
application screen to copy. Passport Input needs its own graphical LVGL
interface. A single label containing the whole page or a plain text menu is
not an acceptable finished UI. Use a compact status strip, a main action
area, and a fixed three-key navigation footer within the 240 x 320 panel.
The status strip shows only observed BLE, Wi-Fi and input readiness; do not
invent a clock, battery level or connection state. Home uses large focusable
Mouse, Keys, Connections and Status cards. The Connections card shows the
observed Bluetooth and Wi-Fi state. Mouse uses a drawn directional pad
with a distinct focused direction and center click. Keys use keycap-shaped
rows. Bluetooth and Wi-Fi show bounded, scrollable cards with the display
label as title, original identity as secondary text, and a real connection
badge. Pairing, confirmation, loading, empty and error states each have a
dedicated visual treatment. Long names truncate within the card without
changing the row identity or focus. The footer labels UP, OK and DOWN and
updates the OK action for the focused item.

Provide dark and light palettes with the same layout and interaction
semantics. Dark defaults to a near-black/graphite field with high-contrast
text and a bright cyan/teal focus accent. Light uses an off-white field,
dark ink text and the same accent family. Focus must remain obvious without
color alone, using a border or shape as well as fill. The Appearance screen
lets the user switch themes with three keys and stores the selection; a
failed save is shown as an error rather than a successful preference change.
Keep the selected theme through reboot. Build the interface from bounded
LVGL objects and fonts, update changed widgets rather than recreating the
whole screen on each input tick, and measure flash and internal heap on C3.
Both palettes require actual 240 x 320 render captures; the light palette
also needs on-device display qualification before claiming physical quality.

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
visible Stop, Bluetooth host management, and Bluetooth/Wi-Fi display labels.
Use touch-sized controls and support portrait and landscape; do not rely
on hover or a hardware keyboard. Pointer cancel, lost capture, page hide,
session loss, network switch, lock, target change,
and lease expiry stop held input. Browser release on page hide or network
loss is best effort; the device's three-second input lease and drag deadman
are the final stop path. Reconnect never replays an uncertain command or
resumes a drag. Capability discovery hides or disables unsupported
Passport operations. The browser does not use phone motion sensors;
Mosaico keeps its own physical motion mouse.

The Web controller is a designed application, not a diagnostic form. Use the
same visual tokens as the device while respecting each target's capabilities:
a clear connection/target header, prominent touchpad, keyboard and action
areas, status and result feedback, and card-based Bluetooth/Wi-Fi lists.
Desktop and iPad landscape use a spacious two-column control layout;
phone portrait stacks the touchpad and QWERTY keyboard on the Input page.
Connections has its own navigation entry. Use safe-area padding and touch
targets of at least 44 × 44 px. On phones, dense keyboard rows share the
available width equally with 44 px minimum key height.
Provide dark and light palettes with system preference as the initial choice
and an explicit, persistent browser override. The theme switch must work
before a control session is established. Focus, pressed, disabled and pending
states need distinct shapes or borders as well as color. Keep command
semantics, keyboard accessibility and stop/release behavior identical across
both palettes. Capture desktop, iPhone-sized and iPad-sized renders in each
theme for visual review; inspect the actual browser layout, not CSS alone.

The Passport agent API remains a direct device IP endpoint. Expose state,
capabilities, an open Passport session, command submission, result lookup,
and a WS input channel. Reuse the existing Bluetooth and Wi-Fi list and
management operation names. Add typed `display_name.set` and
`display_name.clear` with an explicit HID-bond, Classic-bond,
Wi-Fi-profile, or exact scanned-AP kind, stable ID, and expected revision;
their result identifies the confirmed write or storage uncertainty.
Mosaico requires `connections.manage` for Bluetooth labels and
`wifi.configure` for Wi-Fi labels, while Passport allows only these exact
label mutations on its open listener. Use the existing operation catalog
and envelope where supported. An acknowledgement means queued or HID
transport acceptance as specified, never proof that the target OS acted.

## Build, review, and delivery

1. Freeze the target capability matrix and project layout. Preserve
   unrelated Mosaico application and media behavior while changing only
   its control AP, connection management, label, and web integration.
   Add C3-only build configuration and pin the official Passport
   BSP/dependencies and licenses. First compile a minimal C3
   combination of display, three buttons, NimBLE, STA/AP, and an HTTP
   listener, then inspect actual internal-memory use before expanding it.
2. Add Mosaico persistent control AP with its enrolled HTTPS policy,
   matching certificate endpoint, network-switch release, and minimal UI
   entry. Review and publish this batch without altering other Mosaico
   features.
3. Add Passport BLE HID, open-network HTTP input service, STA/AP setup,
   shared input owner, three-button UI, and optional local cues. Publish
   cohesive, buildable batches; a partial target must identify unavailable
   functions honestly.
4. Wire Bluetooth host management and persistent display labels to real
   owners on both targets, then add the responsive browser and direct agent
   control. Reconcile requirements, API, Wi-Fi, browser, build, and
   validation docs. Reuse the same independent review conversation for
   fixed, meaningful integration checkpoints rather than creating one
   review conversation per task.
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
Exercise Web/API list unknown/refresh, disconnect without bond deletion,
target switch while a key or drag is held, wrong or offline selected host,
pairing-window expiry, independent browser/BLE disconnect, stale label
revision, duplicate names, unsaved AP label capacity, reboot persistence,
and storage uncertainty on both targets. Verify the actual device lists
render accepted labels and raw-name details, and check the responsive
controls on desktop, iPhone, and iPad in STA and AP modes.

For every accepted batch, stage exact files, inspect the current remote
head, integrate without force, publish, and verify remote ancestry.
Public commits contain only source, portable documentation, and non-personal
evidence. Keep credentials, pairing keys, private addresses, raw device
logs, personal data, local paths, and performance snapshots out of Git.

## Sources

- [FoloToy Passport BSP](https://github.com/FoloToy/ai-passport)
- [FoloToy application UI guidance](https://github.com/FoloToy/ai-passport#start-development-with-one-requirement)
- [FoloToy hardware guide](https://github.com/FoloToy/ai-passport/blob/main/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
- [PassportOS three-key UI reference](https://github.com/zh30/PassportOS)
- [Passport-OS dark UI reference](https://github.com/iCurrer/Passport-OS)
- [Passport BLE HID example](https://github.com/lewangdev/ai-passport-iphone-agent)
- [Passport QR/SoftAP example](https://github.com/kunlun322/Qoder-Passport)
- [ESP32-C3 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html)
- [ESP32-C3 fixed-function USB](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html)
- [FoloToy community publication](https://github.com/FoloToy/ai-passport/blob/main/docs/development/release/publish-to-community.zh_CN.md)
