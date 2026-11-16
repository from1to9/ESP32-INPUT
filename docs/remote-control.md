# LAN control API

The ESP-Mosaico firmware exposes HTTPS and WebSocket control over the
station connection or the locally selected [control hotspot](wifi-provisioning.md#persistent-control-hotspot).
The selected USB or Bluetooth HID host remains the input target. The device
requires an enrolled identity and checks the caller's permissions at the
operation boundary.

Use the address shown in Web/API. In control hotspot mode the browser uses
the exact enrolled hotspot DNS name and port for Host, Origin and TLS. The
fixed IP `192.168.4.1` is reserved for native TLS clients using credential
login and Bearer authorization. Browser assets, cookies, pairing and WS
tickets are served at the device-specific name. A mode switch retires the previous sessions
and input lease. Open a fresh session and acquire a new lease on the selected
network. Time-limited authorization is unavailable after an offline cold
boot until the device has usable time. Valid permanent grants follow their
existing enrollment and permission rules.

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

## Transport and authorization

- Native clients validate the HTTPS identity and authenticate their session.
  Browser clients use their own approved identity, secure cookie and CSRF
  token. The browser checks the exact HTTPS origin and obtains a single-use
  ticket before opening the WebSocket. See the
  [companion transport details](../bridge/README.md#http-and-media).
- Commands enter `POST /api/v1/commands` or the authenticated
  `/api/v1/channel` WebSocket. Both paths use the same operation registry.
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

## Results and retries

Read the request or job result for work accepted asynchronously. A command
may be accepted, queued, submitted, completed, rejected or left with an
unknown outcome, depending on its owning service. A transport acknowledgement
does not show what the target application did.

After a lost reply, query the existing result or retry only through the
documented request identity and session rules. Never generate a new command
to replay an uncertain effect. See [input semantics](api-input-semantics.md)
and [configuration](configuration.md) for input and settings lifecycles.
