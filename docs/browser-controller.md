# Mosaico browser input

Mosaico serves the same browser controller at `/` over local HTTP on its
station Wi-Fi network or [control hotspot](wifi-provisioning.md#persistent-control-hotspot).
The enrolled HTTPS listener can also serve the controller.

## Connect and take control

1. On an unlocked Mosaico, open **Settings → Wi-Fi → Web/API** and select
   **Generate code**. The panel shows an eight-digit access code and the
   current HTTP address. The code survives reboot. **Replace code** changes
   the code for future authorization requests.
2. Join the same LAN or the displayed control hotspot and open the HTTP
   address in a browser. The page opens before sign-in. Name the browser,
   enter the code and choose **Authorize**. Long-term access is
   selected by default and lasts until revoked. The six-hour and 24-hour
   choices require trusted device time.
3. After Mosaico saves the grant, the browser signs in with its own random
   credential. It stores the credential for this origin and can reconnect
   without another code or device interaction. The code is used for
   authorization and is cleared from the browser form after submission.
4. Check the input target and readiness, then select **Take control**.
   Login, reload and saved authorization leave input idle.

The device code authorizes keyboard and mouse access and management. Review,
edit or revoke each saved authorization under **Web/API → Browsers**.
Replacing the code leaves existing browser grants in place. Forgetting the
identity in a browser removes its local credential and requests session
closure. Device-side revocation separately removes access.
An explicit USB replacement of the device's TLS identity retains the existing
trust reset behavior and can require browser authorization again.

The LAN address, hotspot address and HTTPS address are separate browser
origins. Each browser profile or origin gets its own credential even when
two entries use the same name. A new login replaces that browser's older
session on the listener. A network switch closes the old sessions and input
lease. Reconnect on the selected network, then explicitly acquire control.

The displayed HTTP address has plaintext transport. A participant able to
observe or alter network traffic can see the code and browser credentials.
Use enrolled HTTPS when transport confidentiality or server authentication is
needed. On the control hotspot the HTTPS browser uses the enrolled DNS name
and port. See [TLS enrollment](../bridge/README.md#control-hotspot-enrollment)
for certificate setup and renewal.

## Control an iPad or iPhone

First pair the Mosaico input device ending in `-IN` with the iPad or iPhone
and enable AssistiveTouch. In Mosaico **Settings → Wi-Fi → Web/API**, open
the HTTP address on the controlling phone or computer and authorize the
browser as described above. Check the selected Apple device and choose **Take control**
to use the browser touchpad and keyboard.

Bluetooth input keeps the Apple device's USB port available for a Mac running
QuickTime's live preview. The controller page provides input controls; view
the target screen on the device or through that separate preview. Follow the
[iPhone and iPad guide](user-guide.md#control-an-ipad-or-iphone) for pointer
setup, screenshots and Siri actions, or the [automation guide](automation.md#ipad-and-iphone-control)
for API and MCP clients.

## Input and outcomes

Mouse uses relative touchpad movement, four direction buttons and explicit left
and right clicks. Dragging the pad moves the target pointer; it does not hold a
host mouse button. Mouse and touch use Pointer Events and pointer capture.
Movement is coalesced only inside the current contact, limited to the existing
signed 127-unit report bounds, and discarded after 150 ms or contact/context
loss. It is not queued for later replay.

The original sample deadline and contact identity remain attached across state
preflight and lease renewal, and are checked immediately before submission. An ended,
cancelled, replaced or expired sample is discarded without command submission;
fresh movement from the current contact remains eligible. A request already
submitted retains its actual result or uncertainty when the contact ends.
The pad also accepts arrow keys while focused; a key repeat does not generate a
stored movement sequence.

The touchpad and US QWERTY keyboard share one page. After **Take control**, a
click, drag or key tap selects the required device mode when necessary. Input
follows only after that change is confirmed on the same lease. Stop, loss of
control or an unknown reply prevents the pending action from continuing.
Shift, Ctrl, Alt and Meta apply to the next key tap only. Open **Symbols &
navigation** for punctuation, Tab, Esc, Delete, Home, End and arrows. Composed
text preserves case. The host must use the supported US ANSI physical
layout. Composed text is limited to **1–32 printable ASCII characters** per
explicit Send. The entire input is checked before any command: unsupported
characters, line breaks and longer strings are rejected without truncation,
splitting or partial submission. Send never appends Enter. The text stays in the
browser for inspection; there is no clipboard read or implicit physical-key
forwarding. This browser draft is separate from Mosaico's local keyboard draft.

The page uses the existing typed `control.acquire/renew/release`, `mode.select`,
`mouse.move_relative`, `mouse.click`, `keyboard.key_tap` and
`keyboard.text.enqueue` operations. It serializes mutations and reads actual
command results; acceptance is not displayed as successful input. Completed
key pairs, started prefix and any uncertain character are displayed when the
device supplies them. “Sent” means the selected HID send API accepted the
reports and neutral releases, not that the target application received them or
that its focused field was correct.

A lost reply, unavailable result or unexpected sequence acknowledgement stops
local production and lease renewal. Nothing is automatically resubmitted.
**Check result** only reads a known command key; if no key arrived, the outcome
remains unknown. Inspect the target before explicitly disconnecting/reconnecting
and sending any new action. A current target snapshot cannot prove the outcome
of an earlier unknown command.

If the device definitively rejects a relative move as expired, the page
reports the rejection and checks the lease before accepting fresh input.
The expired move is never resent. During a continuous touch, the page can use
newer deltas from that same contact when its state read completes. Each batch
keeps its own deadline.

If the enrolled HTTP or HTTPS listener offers the authenticated input
channel, touchpad and direction buttons use it after **Take control**.
Movement shares the existing command sequence with other button actions.
On a channel that negotiates motion terminal replies, the touchpad combines
unsent deltas from the same contact while it waits. A report can move farther
than a single sample, within the same per-axis limits. The combined movement
expires when its oldest sample reaches 150 ms. When it expires, later samples
start a new batch. Once the current command and its checks finish, the page
tries to send fresh accumulated movement. These pad submissions are at
least 30 ms apart. Lifting the finger or stopping control discards unsent
movement.
While movement or renewal is in flight, input buttons keep their appearance
and focus. Additional button presses during that interval are discarded.
**Stop** remains available.
Lease renewal and device checks continue while the page is visible. A lost
channel stops control and discards the pending contact. Reconnect if a result
is unknown, then take control explicitly to resume.

The active input lease is at most three seconds and is explicitly renewed
while the visible page controls input. Local takeover, lock, changed target,
boot, authorization loss and lease loss stop the controller. Hiding/leaving the
page or the browser's offline event stops production immediately and requests
session closure; a missing close reply is not closure proof, and the device's
short lease expires independently. Reconnection requires explicit acquisition.

Pointer feedback depends on network latency and the selected HID link.
Further tuning of continuous pad movement is deferred.

## HTTP boundary and validation

Static and dynamic requests use the active listener's exact Host and Origin
boundaries. The page has no CORS access and its CSP permits only same-origin
scripts, styles and connections. Browser mutations require the session CSRF
value. Ordinary authenticated GETs may omit Origin. HTTP uses the
`control-http` HttpOnly, SameSite=Strict cookie. HTTPS uses the
`__Host-control` Secure, HttpOnly, SameSite=Strict cookie. The preauthentication
cookies are separate for the two transports. The listener permits up to 3,072
header bytes. Existing body, frame and socket limits still apply.

See the [browser regression instructions](../tests/web_controller/README.md)
for Chromium and native-client tests. The tests use production firmware paths
with substitutes for the listener, NVS, trusted time and HID send boundary.
LAN and hotspot reachability on a physical unit require device observation.

## Local development

Run the loopback development server from the checkout and open
`http://127.0.0.1:8765/`. Replace the example device address with the HTTP
address of the Mosaico under test.

```sh
python3 tools/serve_web_controller.py --device-origin http://mosaico.local
```

Refresh to load edits to the local HTML, CSS and JavaScript. The local proxy
validates the loopback Host and Origin before forwarding API traffic to the
single configured device. Mosaico validates authorization, session cookies and
CSRF, and executes each action through its normal input path.
When the enrolled HTTP listener advertises WebSocket support, the proxy
relays the input channel to the same configured HTTP device with its browser
cookie and single-use ticket.

For an existing approved test browser, pass `--identity-file /path/to/identity.json`
with a private file containing its `client_id` and `credential`. The loopback
page imports the identity before signing in. Taking control remains an explicit
action. On firmware that only offers the full state API, pass `--legacy-state`
to forward protocol-only state reads to that route and preserve its responses.

For the existing TLS input channel, use the device's commissioned HTTPS
address and its public trust certificate. This keeps the device's certificate
and host verification and uses the same browser cookies and channel ticket.

```sh
python3 tools/serve_web_controller.py --device-origin https://mosaico.local \
  --device-ca /path/to/public-device-certificate.pem --port 8766
```

Open `http://127.0.0.1:8766/` in the local test browser. The proxy relays
WebSocket frames to the configured HTTPS device over the verified TLS
connection. Add the same identity and legacy-state options when using a saved
test identity and older firmware.
