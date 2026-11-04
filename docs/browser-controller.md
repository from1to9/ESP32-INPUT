# Mosaico browser input

The station HTTPS listener serves a same-origin controller at `/`, with only
`/controller.css` and `/controller.js` as static dependencies. It works with a
desktop mouse and a phone touchscreen. The provisioning hotspot is not a
controller. The listener still requires its actual installed TLS identity and
station connection; this page does not create an insecure HTTP fallback.

## Connect and take control

1. Open the station HTTPS address shown by Mosaico. The browser must trust the
   installed certificate. Use the same hostname and port on later visits.
2. Name the browser and choose **Pair this browser**. Web Crypto creates an
   independent random client ID and credential, saved only in that origin's
   browser storage. Compare the complete SHA-256 fingerprint with the proposal
   on Mosaico, then approve keyboard/mouse access and the desired duration there.
   The default is six hours; the device also offers 24 hours or permanent access.
3. Choose **Check approval**. An approved browser signs in using its own
   credential. It receives a Secure, HttpOnly, SameSite=Strict session cookie
   and an in-memory CSRF value. No native bridge credential is required.
4. Check the actual input target and readiness, then choose **Take control**.
   Select Mouse or Keyboard with the explicit mode buttons. Login, reload and
   saved approval never restore an input lease or start a pending action.

Saved identity is specific to the browser storage and HTTPS origin, not its
name, IP address or the HID destination. Another same-name browser must pair
independently. A new login for the same browser identity replaces its own older
browser session; use one controlling tab at a time. **Forget identity in this
browser** removes that local credential and closes the current session. Revoke
its saved permission separately on Mosaico when desired.

## Input and outcomes

Mouse uses relative touchpad movement, four direction buttons and explicit left
and right clicks. Dragging the pad moves the target pointer; it does not hold a
host mouse button. Mouse and touch use Pointer Events and pointer capture.
Movement is coalesced only inside the current contact, limited to the existing
signed 127-unit report bounds, and discarded after 150 ms or contact/context
loss. It is not queued for later replay.

The original sample deadline and contact identity remain attached across state
preflight and lease renewal, and are checked immediately before POST. An ended,
cancelled, replaced or expired sample is discarded without command submission;
fresh movement from the current contact remains eligible. A request already
submitted retains its actual result or uncertainty when the contact ends.
The pad also accepts arrow keys while focused; a key repeat does not generate a
stored movement sequence.

Keyboard provides individual letter/number taps and Space, Enter, Backspace,
Tab, Esc, Delete, Home, End and arrow keys. Letter buttons send lowercase;
composed text preserves case. The host must use the supported US ANSI physical
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

The active input lease is at most three seconds and is explicitly renewed
while the visible page controls input. Local takeover, lock, changed target,
boot, authorization loss and lease loss stop the controller. Hiding/leaving the
page or the browser's offline event stops production immediately and requests
session closure; a missing close reply is not closure proof, and the device's
short lease expires independently. Reconnection requires explicit acquisition.

## HTTP boundary and validation

Static assets have an exact Host/Origin boundary, no CORS and a CSP permitting
only same-origin external scripts/styles and connections. Dynamic requests use
the existing production HTTPS adapter and protocol: same-origin JSON POSTs,
Secure session cookie, exact Origin and mutation CSRF. Normal GETs may omit
Origin; they still authenticate with the browser cookie. The listener permits
up to 3,072 header bytes to accommodate browser UA, Fetch Metadata and cookies;
normal body/frame and socket limits remain in their existing owners.

Run `node tests/web_controller/test_client.mjs` for client state/validation
regressions. [The browser test instructions](../tests/web_controller/README.md)
exercise real Chromium over loopback HTTPS with the production HTTP adapter,
protocol, access manager, coordinator and HID engine. Those tests substitute
SDK listener I/O, NVS, trusted time, physical approval and the HID send boundary;
they do not establish actual LAN, certificate-installation or BLE/USB host
compatibility on a physical Mosaico.
