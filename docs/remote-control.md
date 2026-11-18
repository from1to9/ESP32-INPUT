# LAN control API

ESP-Mosaico serves a local HTTP browser controller and an enrolled HTTPS/WSS
API over station Wi-Fi or the selected
[control hotspot](wifi-provisioning.md#persistent-control-hotspot).
Both control paths use the selected USB or Bluetooth HID host and its current
input readiness. Web/API shows the active HTTP address. The HTTPS browser
uses the enrolled server name or address, including the enrolled hotspot
name in hotspot mode. A mode switch closes both listeners' sessions and
releases remote input before the interface changes.

For a browser remote, direct HTTP commands or an AI-assisted input session,
see [control examples](automation.md).

## Interface definitions

| File | Use |
| --- | --- |
| [OpenAPI](../api/openapi.json) | HTTP routes and request shapes |
| [Operation catalog](../api/control-v1.json) | Typed operations, scopes, argument limits and configuration metadata |
| [Command schema](../api/control-command.schema.json) | Versioned command envelope shared by HTTP and WebSocket |
| [MCP tools](../bridge/tools.json) | Generated tool descriptions and input schemas for the companion |

Start with [local enrollment](../bridge/README.md#local-enrollment) for native
clients or the [browser guide](browser-controller.md) for the same-origin UI.
On a commissioned device, read `GET /api/v1/state`,
`GET /api/v1/capabilities` and `GET /api/v1/operations` to discover the active
target, runtime readiness and available operations. The catalog defines the
wire shape. A listed operation may be unavailable for a missing or unready
provider or a caller with insufficient scope.

`GET /api/v1/state?view=protocol` returns the same authenticated `protocol`
snapshot and records its sequence for control acquire and renew. It omits
device details and does not wait for the coordinator's device-detail read.

## Transport and authorization

- Native API and MCP clients validate the HTTPS identity and use enrolled
  credentials with Bearer sessions. The enrolled HTTPS browser can use its
  own cookie session and obtain a single-use ticket for WSS.
- The Mosaico HTTP browser reads the page without authentication and uses
  the persistent device code to enroll an independent browser credential.
  Subsequent sign-in uses the browser's credential, exact Origin and
  preauthentication cookie. The authenticated cookie and CSRF token are
  separate from the device code. HTTP provides plaintext transport. Passport's
  open HTTP policy is a separate firmware policy.
- Commands enter `POST /api/v1/commands` or the authenticated
  `/api/v1/channel` WebSocket. Both paths use the same operation registry.
  Mosaico's enrolled HTTP channel accepts text frames from approved browsers
  with input permission and a CSRF-issued single-use ticket. Binary media
  stays on the HTTPS channel.
  When `websocket_motion_terminal` is advertised, an input-authorized
  browser can opt in on that channel with `{"v":1,"type":"motion_terminal"}`.
  The device acknowledges the opt-in with `{"motion_terminal":true}`.
  A newly accepted relative move keeps its usual admission reply and key.
  Wait for the matching terminal result, marked `"motion_terminal":true`,
  before sending another command on that channel. An immediate terminal
  reply needs no second frame. When `websocket_renew_terminal` is advertised,
  `{"v":1,"type":"renew_terminal"}` opts in to the same behavior for both
  relative movement and `control.renew`. The reply is
  `{"motion_terminal":true,"renew_terminal":true}`. The admission reply
  includes `now_ms` as a decimal string in the device's monotonic time.
  When the listener advertises `websocket_motion_window`, a browser on its
  ticket-bound WebSocket can send
  `{"v":1,"type":"motion_window","motion_window":2}`. The advertised
  `websocket_motion_window_max` is the device's maximum. An omitted width
  requests two. The reply includes the exact selected integer width. Invalid
  or unsupported requests are rejected. A maximum of one uses the legacy
  channel. In a normal window, send further fresh relative moves only after
  all outstanding moves have matching admissions. The negotiated width caps
  the number awaiting terminal results. A device that also advertises
  `websocket_motion_ordered` can explicitly select `"motion_ordered":true`
  in the request and reply. That selection permits filling the window on its
  ordered channel before the individual admissions arrive. Wait for all pending terminals before
  sending renewal, other commands or resource reads on the channel.
  Replies for recorded commands in this mode carry `command_seq` for the
  original sequence and `command_key` for the individual result. Match both for a pushed
  terminal even when it arrives out of order. `next_seq` reports the current
  session sequence and can advance past the command being reported.
  Admission records the queued command. `sent` reports acceptance by the
  HID transport. Each move keeps its own deadline and can expire while
  waiting to execute or at the report submission boundary. USB relative
  moves stay pending until TinyUSB accepts the report or the device records
  expiry, cancellation or submission refusal. An unknown or mismatched result stops the input
  channel and requires checking the target before resuming. Close the
  channel to stop in-flight input without waiting for its terminal replies.
  A full window rejects additional WebSocket data by closing the channel.
  The original authenticated HTTP session can read results by key or
  recover a recorded result using an exact request retry. The browser uses
  readback for known outstanding keys and never resends unknown input.
  HTTP, other operations, and channels without opt-in use the usual result reads.
  Read status and job resources separately. A session can remain valid
  after its input lease ends.
- Remote input requires an explicitly acquired short lease and current input
  context. A local takeover, lock, target replacement, lease expiry or channel
  closure can revoke input and cancel pending work. New sessions do not
  restore held buttons or replay commands.
- Audio capture, export and firmware updates use separate scopes and grants.
  An input lease does not authorize those operations. See
  [audio and recording](api-audio-recording.md) and
  [Wi-Fi and firmware updates](wifi-management-ota.md).

## Browser authorization

`POST /api/v1/pairing/code` accepts same-origin JSON containing `client_id`
(32 hex digits), `credential` (64 hex digits from the browser's random
credential), `label` (1–32 printable ASCII characters), `code` (eight
digits), and `duration` (`permanent`, `24h`, or `6h`). A 202 response
acknowledges a pending storage operation. Poll `POST /api/v1/pairing/status`
with only `client_id` and `credential` to check the grant before signing in.
A wrong code is rejected. Attempt limits or an occupied authorization
owner can temporarily return 429. An unavailable or uncertain store can
return 503. A timed grant requires usable device time.

The existing HTTPS-only `/api/v1/pairing/requests` route still supports
physical review and the existing credential proof. The code route grants
input and management scopes. Updates and capture retain their own
authorization. Replacing the code prevents new grants from the old code.
Saved grants remain individually revocable. An approved grant can sign in
while input is locked. Input still requires an explicit short lease and an
unlocked, ready target.

## Results and retries

Read the request or job result for work accepted asynchronously. A command
may be accepted, queued, submitted, completed, rejected or left with an
unknown outcome, depending on its owning service. A transport acknowledgement
does not show what the target application did.

After a lost reply, query the existing result or retry only through the
documented request identity and session rules. Never generate a new command
to replay an uncertain effect. See [input semantics](api-input-semantics.md)
and [configuration](configuration.md) for input and settings lifecycles.
