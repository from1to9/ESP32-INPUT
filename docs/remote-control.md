# Remote control and Mac bridge design

The station HTTPS listener also provides the [browser mouse and keyboard controller](browser-controller.md), with its own device-approved browser identity and explicit input lease.

Status: implemented source and host-tested integration candidate, 2026-10-21. The station HTTPS/WSS transport, typed protocol, protected local trust import, native MCP companion, Wi-Fi profiles, clock and OTA workers now exist. Actual manager/resource availability remains authoritative; source/host/target evidence does not establish device, browser, audio-route or release qualification. See [companion operation](../bridge/README.md) and [validation](validation.md).

## 1. Baseline and contract ownership

The original Wi-Fi foundation was inspected at published commit `d0f1f60706613ce7b7339e1cd14066b7e81f84ae`; that remains historical research provenance. Current working-tree sources integrate the production coordinator and shared HID queue, local/Bluetooth/USB media managers, recording storage, spoken letters, schema-4 preferences, station TLS control, generated API/MCP metadata and update workers. The generated [catalog](../api/control-v1.json) contains 147 operations and 77 configuration fields. A typed entry is not evidence that an absent provider, profile, trust anchor or storage partition is available.

The [complete API/function matrix](api-function-matrix.md) owns action coverage, proposed command IDs, field inventory and tests. [Shared configuration](configuration.md) owns metadata, config.stage/apply/cancel, runtime/apply/save evidence and migration. [Wi-Fi/OTA](wifi-management-ota.md) owns scan/profiles/background/update mechanics. The [remote test plan](remote-control-test-plan.md) supplements their failure gates. Do not replace approved core-mode UI/interaction with a diagnostic layout; navigation/status/settings follow the current interaction contract.

Wi-Fi provides independent LAN management and control. Bluetooth/USB are selected HID transports. Network settings/jobs do not select a HID target or redirect queued input. One selected HID target and one input owner/producer are active at a time. Standalone input needs neither Wi-Fi nor a Mac. Mac-hosted MCP/AI/capture is optional; Mosaico cannot capture an iPad screen or unlock its host.

Remote access starts disabled/unclaimed. A locally enrolled service may retain its enable preference; boot/reconnect never restores input leases, held input, microphone unmute, active capture consent/consumers, recording or executable jobs. A saved standing grant remains permission only. Existing network credentials/provisioning tokens are not LAN authorization. Implemented functions use typed API/MCP bindings; missing drivers/capabilities and deliberately unsupported optional operations remain unavailable with reasons.

## 2. References and reuse boundary

These primary sources were checked online on 2026-10-21. Floating documentation and branches are research references, not dependency pins. At implementation, select compatible versions against the pinned S31 SDK, record exact commits and preserve file-level notices. No external implementation is copied by this design.

| Source | Concrete lesson for this product | Reuse boundary |
| --- | --- | --- |
| [PiKVM HTTP API](https://docs.pikvm.org/api/), [HID API source](https://github.com/pikvm/kvmd/blob/master/kvmd/apps/kvmd/api/hid.py) | Separate state queries, event subscriptions and typed HID actions; expose reset/release explicitly | KVMD is [GPL-3.0-or-later in the reviewed source](https://raw.githubusercontent.com/pikvm/kvmd/master/kvmd/apps/kvmd/api/hid.py). Use behavior as inspiration; do not transplant its daemon or UI into the Apache-2.0 firmware. No PiKVM protocol-compatibility claim |
| [Official esp-mosaico-claw factory firmware](https://github.com/esp-mosaico/esp-mosaico-claw/tree/2e1017e42d507cd9216d4da5b2c7d45cb2007971) | Closest board-specific reference for network state, web configuration, capability registration and MCP wiring; concrete assessment below | [Apache-2.0](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/LICENSE), with separately reviewed submodules/assets. Prefer selected adapters and service patterns over importing the entire AI runtime |
| [TinyPilot architecture](https://github.com/tiny-pilot/tinypilot/blob/master/ARCHITECTURE.md) | Static browser controls, REST for inspection/settings and WebSocket input; backend owns HID transport | [MIT license](https://raw.githubusercontent.com/tiny-pilot/tinypilot/master/LICENSE); review attribution before any later reuse. Its Flask, Linux USB gadget and HDMI pipeline are not an embedded BLE implementation |
| [S31 HTTP server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/protocols/esp_http_server.html) | Use session context and pre-upgrade authorization; serialize access because server APIs are not thread-safe | [ESP-IDF Apache-2.0 root license](https://raw.githubusercontent.com/espressif/esp-idf/master/LICENSE); inspect exact component/example headers |
| [S31 HTTPS server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/protocols/esp_https_server.html), [official WSS example](https://github.com/espressif/esp-idf/blob/master/examples/protocols/https_server/wss_server/README.md) | Persistent TLS connections, asynchronous state and liveness checks | Example mechanics are a starting point, not authentication, browser trust or memory qualification. Do not copy example keys/certificates |
| [S31 protocomm](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/provisioning/protocomm.html), [network provisioning SoftAP source](https://github.com/espressif/idf-extra-components/blob/master/network_provisioning/src/scheme_softap.c) | Local setup using an established secure provisioning exchange | Prefer Security 2, SRP6a plus AES-GCM, with per-device setup material; [provisioning component license](https://raw.githubusercontent.com/espressif/idf-extra-components/master/network_provisioning/LICENSE) is Apache-2.0. Provisioning security is separate from runtime authorization |
| [MCP transports, version 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports), [MCP tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools), [official Python SDK](https://github.com/modelcontextprotocol/python-sdk) | Local stdio bridge with typed tools and structured results; keep network exposure optional | SDK [MIT license](https://raw.githubusercontent.com/modelcontextprotocol/python-sdk/main/LICENSE). Pin a compatible release when implemented; MCP annotations are not device authorization |
| [OWASP WebSocket security guidance](https://cheatsheetseries.owasp.org/cheatsheets/WebSocket_Security_Cheat_Sheet.html) | Authenticate subscriptions, validate exact origins, recheck session validity and bound messages | Security reference only; the limits and protocol below are original product proposals |
| [S31 OTA documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/system/ota.html) | Separate image verification, boot selection and post-boot confirmation | At the original research checkpoint, the single-app partition could not support OTA; current v3 source requires an actual matching table and app-controlled rollback |

### Official Mosaico factory code: selection assessment

The following files were inspected at factory-firmware commit `2e1017e42d507cd9216d4da5b2c7d45cb2007971`. Its [submodule declaration](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/.gitmodules) identifies `espressif/esp-claw`; the reviewed tree pins it to `62aaeed7de8f3055615448620dd00ed39fd6af13`. These are research snapshots; no dependency has been added here.

| Reviewed implementation | What exists | Proposed adaptation |
| --- | --- | --- |
| [Network provisioning service header](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/network_provisioning_service/include/network_provisioning_service.h) and [implementation](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/network_provisioning_service/network_provisioning_service.c) | Create/start/stop/status/callback API, four observer slots, STA/AP snapshot, Wi-Fi-join QR and reload/apply of persisted config. The advertised portal URL is HTTP | Reuse the bounded status/service shape and board-network integration after review. Strip setup QR secrets from normal state. This wrapper is not evidence of Security 2 enrollment, runtime client authentication or safe HID access |
| [Wi-Fi manager API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/wifi_manager/include/wifi_manager.h) | Apply STA config, queued radio enable/disable, status, scan records and independent event subscribers | Adapt the event boundary into the network manager. Add revisioned staging, bounded recovery, coordinator cancellation and operation-specific authority; no guaranteed storage rollback or blanket physical prompt. Do not expose driver calls directly to remote input |
| [HTTP server core](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_core.c), [status API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_status_api.c), [configuration API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_config_api.c) | `httpd_start` on port 80; `GET /api/status`, `POST /api/restart`, and GET/POST `/api/config`; configuration response distinguishes save from restart-to-apply | Useful route separation and adapter callbacks. Do not import restart, general file/Lua/IM routes or assume its portal is an authenticated input service. Use this document's TLS, session, origin, scope and presence gates; versioned product settings must expose saved versus applied revisions |
| [Capability API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_capabilities_api.c) | `GET /api/capabilities` enumerates compiled capability groups | Adapt discovery; add physical presence/driver initialization, target qualification, current availability and reason. A compiled group alone cannot authorize a button or advertise working audio |
| ESP-Claw [MCP server header](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/include/cap_mcp_server.h), [server implementation](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/src/cap_mcp_server.c), [dependency manifest](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/idf_component.yml) | Init/add-tool/start/stop wrapper, string-property tool registration, HTTP MCP manager and mDNS; manifest requests `espressif/mcp-c-sdk ^2.0.0` | Valid embedded-MCP reference for a later alternative. Current design keeps typed MCP schemas and authentication on the Mac to avoid another embedded HTTP manager and tool runtime. Do not expose these tool callbacks as a second direct HID path |
| ESP-Claw [MCP client implementation](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_client/src/cap_mcp_client_core.c) | Initializes remote HTTP sessions and lists/calls remote tools through the MCP manager | Not needed for a device that receives Mac control. Leave outbound tool discovery, arbitrary server URLs and on-device autonomous agent execution out of the initial firmware |

The reviewed first-party service/MCP files carry Apache-2.0 notices; the [ESP-Claw root license](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/LICENSE) is also Apache-2.0. Review transitive modules, assets and exact selected files before any copy, retain provenance and pin replacements. This focused source review is not a full security audit of either upstream project. It supports selective reuse without adopting conversations, memory, Lua, IM, cloud credentials or the full agent lifecycle.

## 3. Components and one operation registry

```text
Local physical input -> production coordinator -> selected BLE / USB transport
                                  ^
Browser / Mac MCP -> authenticated command registry
                         |        |           |
                     config   network/jobs   audio/recording/update managers
                         \        |           /
                         authoritative scoped state/events
```

All product mutations enter **POST /api/v1/commands** or the same envelope over authenticated WSS. Both adapters dispatch one typed registry; no HTTP-to-BLE/USB/codec/NVS bypass. UI intents and generated MCP tools use the same command/configuration metadata. Context-sensitive UI commands capture a UI revision; remote virtual gestures never become physical presence.

Input authority and selected producer are separate: remote-authorized IMU/touch producers use the same coordinator-owned gyro and gesture gates; a network identity cannot bypass them. One authorized producer emits through shared mode/epoch/release rules. The whole first asleep gesture is consumed by wake routing before takeover or normal handling. A fresh local touch/AI gesture after that guard takes over immediately and is consumed for takeover. Queued callbacks cannot resurrect a replaced generation.

Portable domain/registry logic has no BSP pins or driver types. Board adapters own buses, display/touch/motor/codec/radios and publish presence/initialization/qualification. ESP32 variants differ: [the official chip capability table](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/bt-architecture/overview.html) identifies S3 as BLE-only, not Classic HFP/A2DP. No motor/display/mic/transport success-shaped stubs.

The shared configuration family stages inert bounded candidates, then rechecks scopes/revision/capability/conflicts and applies through managers. Keep effective/desired/saved revision, requires-restart and sticky storage uncertainty independent. A failed flash operation can already have changed data; no generic atomic hardware or storage rollback claim. Coalesce explicit changed preferences, avoid per-frame/boot/reconnect/heartbeat writes, and preserve unknown records. Recording data writes have separate quotas/retention and are not preference saves. Board/build ceilings and safety/privacy rules are not ordinary writable settings.

## 4. Enrollment, secure transport and sessions

### Local enrollment

1. A local person opens the enrollment window on an unlocked device. The protected window lasts 120 seconds; a network/USB command cannot open it or assert local presence.
2. The [native commissioning tool](../bridge/README.md#local-enrollment) creates private TLS material, a random client credential, exact scopes and optional purpose-bound capture/update delegations. Client credentials go to Keychain; verifiers go into the private device package. No live Keychain or USB operation is part of host tests.
3. Upload the at-most-8-KiB package through the existing non-echoing CDC owner while the local window is open and the actual USB bus is mounted. Numbered chunks, exact length/hash and a random window token bind one candidate. No second TinyUSB owner or diagnostic command/reset parser is installed.
4. The device exposes a public Review containing the exact package digest, labeled PEM certificate fingerprint, principal IDs/enabled/native status/scopes, capture grant bounds and update source/policy. Only a trusted fresh local action approves that exact digest. Changing the candidate clears approval; expiry/close clears private staging. Upload alone is not enrollment.
5. The trust owner stops the previous server/sessions before writing checked versioned banks in the separate `control` NVS partition. Write/commit errors may already have applied and remain uncertain until explicit verification/replacement. Replacing unknown trust requires its own reviewed flag and does not erase the partition, Wi-Fi/bonds or other namespaces. Initial trust grants permission only: it never resumes input, capture, recording or update work. Additional Security-2 provisioning and browser certificate installation are separate deployment options, not implemented by an ordinary HTTP portal.

Wi-Fi provisioning, application-client enrollment and Bluetooth target pairing are three different trust relationships. None authorizes the other two implicitly.

### Per-controller permissions

The browser generates a random 128-bit client ID and 256-bit credential. Same-origin
`POST /api/v1/pairing/requests` submits the ID, SHA-256 credential fingerprint and
1..32 printable ASCII label. The device displays that exact fingerprint for physical
approval; names and addresses are not identity. One proposal lasts 120 seconds;
repeating it does not extend its lifetime. `POST /api/v1/pairing/status` proves the
raw credential over HTTPS. Both endpoints have bounded schemas in [OpenAPI](../api/openapi.json).
They grant no permission by themselves and cannot approve another controller.

Local Web/API settings choose independent INPUT, MANAGE and UPDATE groups. Approval
defaults to INPUT for 6 hours, with 24 hours and permanent-until-revoked options.
Finite grants require known trusted wall time and fail closed if time is unknown.
The bounded client table includes native/API identities from the existing local
trust package. Editing one migrates it to these groups; untouched native identities
retain their original scopes, capture grants and OTA policy.

| Group | Effective scopes and limits |
| --- | --- |
| INPUT | `view`, `input.control`; explicit short input lease and all existing local/target/release gates |
| MANAGE | `view`, `input.configure`, `config.write`, `connections.manage`, `wifi.configure`, `diagnostics.read` |
| UPDATE | `view`, `update.check`, `update.stage`, `update.activate`, `system.restart`; enrolled signed releases only, at most 600 seconds and one existing 6 MiB OTA slot per job, no continuation after owner loss |

Groups never imply capture, recording, playback, policy/source editing or client
administration. Permanent UPDATE authorization removes the old expiring delegated
policy dependency; release signatures, source trust, known release time, health and
maintenance checks still apply. The generated per-controller update policy ID and
revision must match the actual request. Network permission does not verify firmware.

A separate dual-bank `ctrl_access` namespace in the existing protected `control`
partition stores credential hashes and grants, bound to the enrolled trust-package
digest. A replacement package starts with its own native identities; old browser
authorizations never transfer to it. Commit and exact readback precede an APPLIED
receipt. An uncertain write disables the store until a verified reload; no implicit
erase or success fallback occurs. A concurrent save cannot publish an old client
list under a new revision. UI admission and pending receipts are not durable success.

Every login, active protocol session, cached execution identity, input admission and
active input service checks current authority. Revocation/expiry stops old sessions
and uses the common neutral release barrier for held input. Browser re-login replaces
only that credential's previous browser session. Boots and visits restore permission,
never a lease, queued input, held key or job. Local takeover releases the old lease;
a still-authorized controller may later explicitly take a new lease. Lock, maintenance
and active local interaction still block it. The old Allow/Block switch applies only
to unmigrated legacy API input; it is not an extra boot allowance for saved grants.

### Practical HTTPS choices

| Deployment | Trust and browser path | Tradeoff |
| --- | --- | --- |
| Initial Mac bridge, recommended | Native stdio bridge verifies the enrolled CA/name and exact certificate pin, authenticating directly over TLS; no human web frontend is bundled | Avoids browser-to-device certificate provisioning. Mac is required for this remote path; standalone BLE still works without it |
| Direct device browser, integration candidate | Device HTTPS/WSS uses a certificate trusted by the browser for the exact configured hostname/SAN, provisioned through a managed local CA or explicitly installed trust | Requires real certificate issuance, installation, renewal and hostname handling on each browser/OS; mDNS does not confer trust |
| LAN UI hosted by Mac, later | Browser trusts the Mac's HTTPS hostname/certificate; Mac independently pins device TLS and authenticates both sides | Adds another server, session boundary and certificate lifecycle; do not expose a loopback HTTP service by changing its bind address |

Do not make users bypass TLS warnings, disable certificate verification, or send credentials over ordinary LAN HTTP/WS. Public CA issuance for private names/IPs is not automatic. Browser mixed-content and private-network policies also need actual Safari/Chrome testing. If direct browser trust cannot be installed, offer the Mac bridge or keep the feature unavailable; do not downgrade security. Private-LAN placement is an exposure limit, not authentication. No default Internet forwarding, cloud relay, UPnP or WAN listener is proposed.

### Runtime authentication and browser boundaries

- Use the scopes in the [function matrix](api-function-matrix.md): view, input.control/configure, config.write, connections.manage, wifi.configure, audio/recording scopes, update.check/stage/activate, and separate admin.clients/policy/privacy authority. Private draft/media access is separately granted. Every command rechecks its effective scope; an input lease grants no extra authority.
- Native clients exchange their enrolled random credential through `POST /api/v1/sessions` over verified TLS. Browser login uses its own credential approved on the physical device; never expose the Mac's device credential to browser JavaScript. Reject login with an unexpected Origin or Host as well as authenticated mutations.
- Browser sessions use a random, host-only `Secure; HttpOnly; SameSite=Strict; Path=/` cookie, proposed 15-minute idle and 8-hour absolute expiry. Session tokens stay in the HttpOnly cookie. The browser retains only its own random client credential in same-origin localStorage for later proof; never put credentials in a URL. Same-origin script integrity is therefore part of that credential boundary. Logout/expiry close input channels and revoke their leases. A finite background job may outlive its initiating browser session only through explicit policy-bound delegation; principal/policy revocation and local stop still win. Capture defaults session-bound and needs its own explicit privacy consent.
- Mutating HTTP bootstrap endpoints require same-origin JSON, an unpredictable session-bound CSRF token in a custom header, exact Origin and allowed Host checks. Before login, a same-origin challenge endpoint issues a restricted pre-authentication cookie/nonce usable only for login; login requires both that nonce and the credential and rotates the session. GET does not mutate product/configuration/input state. Deny wildcard CORS, arbitrary reflected origins, `Origin: null`, unexpected proxy hosts and `file://` use. Serve UI and browser API from one origin. Apply a restrictive CSP with local assets, `frame-ancestors 'none'` and no third-party scripts; insert preview/error text as text, never HTML.
- Browser WSS upgrade requires the session cookie, an exact Origin/Host match and a single-use 10-second upgrade ticket obtained via CSRF-protected `POST /api/v1/ws-ticket`. Use an empty JSON-content-type POST and pass the returned 64-hex ticket as `ticket.<ticket>` alongside `esp32-control-v1` in `Sec-WebSocket-Protocol`, never a query string; redact it from handshake logs and consume it once. This is necessary because browser WebSocket constructors cannot freely set Authorization headers. Negotiate the ordinary protocol subprotocol separately. Do not send commands before authorized upgrade and channel initialization.
- Native WSS uses an Authorization header and a native-only enrolled principal. Missing Origin is accepted only on this authenticated native path; supplying an invalid Origin is always rejected. Origin checks complement authentication and are not a credential.
- The loopback Mac UI uses its own browser session, CSRF token, exact configured loopback Origin/Host allowlist and single-use upgrade ticket. Bind to explicit loopback addresses, not all interfaces. A local bootstrap handoff from the native utility creates the browser session without putting credentials in URLs. Loopback HTTP/WS is limited to this on-Mac boundary; device traffic remains authenticated TLS. Verify the supported browser's localhost secure-cookie behavior; if required cookie guarantees are unavailable, use trusted loopback HTTPS/WSS or a native authenticated view, not weaker cookies or a LAN HTTP listener. Other local processes are outside the browser-origin isolation guarantee, so Keychain access and OS user permissions still matter.
- Bind runtime listeners to the intended station interface or enforce equivalent verified filtering for the chosen SDK. Do not accidentally expose control on a provisioning AP. mDNS advertises service identity/version only; possession of an advertisement proves no trust.


## 5. Ownership, approval and time

Input guards are an authenticated input.control scope, exclusive short lease, unlocked state, selected transport/target generation, input epoch, mode, deadline and neutral-release barrier. Management/config/audio/recording/update requests instead bind device/boot/management generation and their resource/consent/policy revision; they do not require or change a BLE/USB host. Input-affecting manager operations still acquire the coordinator's release/maintenance barrier and cannot steal active local ownership.

| Event | Required behavior |
| --- | --- |
| Explicit input acquire | Require locally authorized remote allowance and safe local idle state. Increment epoch, stop prior producer and complete neutral reports; lease default 3,000 ms, renewal about 1,000 ms, within configured ceilings |
| Competing input owner | owner_busy; no preemption, queued acquisition or borrowed lease |
| Renew | Same authenticated input session/lease and fresh state acknowledgement; ping alone does not renew |
| Fresh local gesture after wake routing | A whole asleep wake-only gesture changes no ownership/lock/input. After its guard, fresh AI down/touch immediately revokes remote input, invalidates epoch, cancels/releases and is consumed for takeover. Continued fresh hold may lock. Close the unmigrated legacy allowance until explicitly restored; saved INPUT permission still requires a new explicit acquisition, never auto-resume |
| Mode/producer/output-setting transition | Cancel residual input, clear carry, release and increment input epoch. Old envelopes cannot cross the transition |
| Input socket loss/lease expiry/stale input state | Cancel that input producer, release and clear motion intent; no replay. Independent audio/recording jobs follow their own authenticated owner-channel/session liveness and grants, never the HID lease. HTTP-polling input stops by lease expiry; closing an ordinary HTTP response socket is not a management outcome |
| Selected HID disconnect/transport/target replacement | Invalidate input state and revoke lease; do not alter Wi-Fi profiles or silently reroute to the other transport |
| Lock/reboot | Release/cancel/mute, stop capture and cancel cancelable privileged work; no background policy can unlock. Read-only status and saved-network reconnect may remain. Boot resumes no executable jobs |
| Unlock | Qualified physical control only, motion off/mic muted, no resumed tasks |
| Update maintenance | Current administrator policy or fresh explicit approval, plus actual input release/maintenance readiness; preserve mode/transport and no automatic old input afterward |

Trust enrollment, physical unlock and required new-trust challenges cannot be satisfied by a remote virtual touch or operation.confirm. A physically gated proposal is inert until a new trusted physical event; cancelled requests never resume. Ordinary scoped settings and delegated updates do **not** require a physical prompt for every request.

An authorized enrolled remote admin can stage/apply a constrained updater policy: named principal/scopes, policy revision, source/channel, compatible version/signature/layout/size, finite job lifetime, maintenance/idle and restart budgets, and expiry. Matching jobs use that authority; outside-policy work fails or needs new authorized approval. A policy cannot expand its creator's scopes, bypass lock/local stop, remove signature checks or expose raw flash/partition/eFuse writes. The same revocation and limits apply through MCP.

Capture/recording needs explicit purpose-bound consent or an owner-enrolled bounded grant, qualified resources, readable indication and finite limits. In-grant authenticated starts need no repeated local confirmation; ordinary admin/input scopes do not create that grant. Saving format/volume/privacy preferences is not unmute or capture consent. No setting disables required indication. Default session-bound capture stops on owner-channel/session expiry or loss. File-only `continue_local` requires source consent plus a distinct `background_grant_id`: an enrolled `capture.consent` delegation with purpose `background_recording`. Explicit successful transfer may let it continue without a HID lease after the owner channel disappears, while all live egress stops. Lock/privacy Stop/revoke/maintenance still terminate it. On this board capture prevents blanking, permits only qualified readable dimming, and stops/rejects on missing/stalled indication; failed hardware stop retains the privacy warning. See the [audio/recording plan](api-audio-recording.md).

All timing uses device monotonic time. Input defaults are motion/scroll expiry 250 ms, ordinary discrete horizon 2,000 ms, atomic keys at most 100 ms, mouse hold deadman 1,500 ms, each within the lease and qualified metadata ceilings. An expired request cannot emit nonzero input; neutral releases bypass ordinary deadlines/capacity. Long management/media/update jobs have their own finite deadlines and authority, not an input lease silently kept alive by the bridge.

Release failure stays release_pending and blocks nonzero input. Retry only the bounded production transport policy, then request disconnect/recovery with truthful outcome. Never clear a release barrier because a timer elapsed, a socket closed or teardown was requested. Reconnect needs neutral reports for its new generation. Actual host receipt remains unobserved.

## 6. Protocol, progress and replay

Session/login/logout and upgrade tickets are transport bootstrap. Domain mutations share POST /api/v1/commands and the registry; command and live-media WSS is /api/v1/channel. Authenticated /api/v1/events subscriptions are read-only/scoped. Read resources include state, capabilities, operations, config/schema, config, jobs and paged Wi-Fi/peer/recording/release metadata. Cached GET never starts a scan, capture, save or download. Media/file bytes use separately authenticated bounded resources, not command JSON.

```json
{
  "v": 1,
  "boot_id": "019ab012b8364a589ce09206b5402f33",
  "session_id": "6c1f67ecaf88437393b4ca61ca5bd291",
  "request_id": "41d5308efce542270000000000000008",
  "seq": 8,
  "epoch": "42",
  "transport_generation": "7",
  "target_id": "target-1",
  "lease_id": "ae92c6803978456ab40dfb8dc7bb80b3",
  "deadline_ms": "813250",
  "op": "mouse.move_relative",
  "args": { "dx": 3, "dy": -2 }
}
```

Illustrative IDs are not secrets. Boot/session/lease IDs are random 128-bit hex; each logical session has a random 64-bit request prefix followed by seq as 16 hex digits. This binds request ID to sequence without an unbounded old-ID set. seq is 1..4,294,967,295; rotate before exhaustion. Epoch/generation/revision/time values use decimal strings to avoid JavaScript precision loss. Overflow fails closed.

The example is input-specific. Management/configuration commands instead require management_generation plus expected resource revision and omit/null the HID epoch/target/lease fields. Capture adds consent identity; delegated background work adds policy identity/revision. Scopes, trusted source and presence come from authentication/device events, never caller-controlled fields. A logical command session persists across ordinary HTTP requests; WSS/input reconnection starts a fresh channel generation and requires a fresh input acquire, with no replay or implicit lease. Channel loss invalidates only the old input authority and exact live network consumers. The same HTTP logical session, independent management and file recording retain their separate liveness/grants; explicit logout, principal revocation and session expiry still close the full session.

Validate bounded framing/schema, authentication, sequence/duplicate identity, boot/domain generation, scope/capability/lock, resource revisions/authority, deadline and capacity; recheck mutable gates at execution. Reject invalid/unknown types, fields, encodings, ranges, nonfinite values and unsupported versions rather than clamp or truncate.

| Observation | Evidence / limitation |
| --- | --- |
| rejected | No new work admitted from this envelope; a prior duplicate may already have run |
| accepted | Bounded immutable work reserved; no report/hardware/persistence outcome implied |
| applying/running | Actual manager started; progress does not imply completion |
| applied | Manager-confirmed runtime state/revision, not merely requested value |
| saving/saved | Storage work in progress / acknowledged and read-verified candidate under its codec contract; does not prove runtime application or physical power-loss qualification |
| storage_uncertain | Current stored contents are not established; neither old-record preservation nor absence is implied |
| sent | Every required report/release accepted by the selected BLE/USB send API; not radio delivery, focus or target UI effect |
| cancelled/failed/cleanup_pending | Remaining work stopped/errored or resource release still incomplete; emitted effects are not undone |
| observed | Separate Mac capture evidence with target mapping, frame/time/predicate and verified/inconclusive/failed evaluation; no guaranteed causality |
| result_unknown | Evidence unavailable/evicted/rebooted; never zero effects, success or permission to repeat |

Current local preview removal follows accepted key-downs. New API progress must distinguish started_prefix, completed down/up pairs, uncertain_index and never-started suffix. Do not reinsert/retry a possibly typed character; sent requires all matching releases. Preview capacity is 64, engine batches 32, no implicit Enter. Explicit drag remains running until its matching release or truthful cancellation/failure.

Serialize each logical session's commands. Only next seq is new; a well-formed processed rejection consumes it, a gap does not. Same seq/request ID/canonical payload digest returns cached status without execution/deadline extension. Changed reuse is request_conflict and closes/revokes that input session. Keep 64 bounded records, pin executing records and retain the sequence high-water mark after eviction; old uncached sequence is duplicate_too_old/result_unknown, never fresh work. New sessions/boots cannot replay old envelopes. Desired-state setters are explicit values/revisions; movement/click/confirm/text remain non-idempotent effects despite duplicate suppression. No exactly-once target-delivery claim.

Finite manager jobs have separate IDs and lifetime. A policy-bound update may remain queryable after its initiating browser leaves, but principal/policy revocation, local stop/lock and its deadline apply. No bridge can invent indefinite renewal or resurrect jobs on boot. Initial audio v1 has no pause/resume/seek capability; expose those as unavailable until their owning semantics are defined.

## 7. Complete function and configuration coverage

The [function matrix](api-function-matrix.md) covers the 61 current UI action families, private local Wi-Fi/name editing, microphone/speaker control/routes/playback, speech, recording/artifacts, settings, BLE/USB/Wi-Fi, firmware policies, status and MCP. Generated registry metadata and actual per-operation/provider availability are authoritative; optional pause/resume/seek, target observation and event subscription remain explicitly unsupported where their owner is absent. Every row states parameters/ranges, authority/ownership, lifecycle/evidence, cancellation/errors, capability/status and tests. Native joining keeps the computer on its existing LAN; the fallback AP is explicit. LAN IP does not imply a running backend, and local editor/ticket context cannot be forged by a remote client.

One common config.stage/apply/cancel family replaces parallel per-field network setters. Practical preferences are configurable within validated metadata; board/build ceilings and release/replay/privacy/signature invariants remain immutable. Current source defaults include 64-character preview, nine-cell selector, pairing 120 s with 300 s service ceiling, brightness 50%, timeout 300 s, tap-to-click enabled and local Bluetooth name ESP32-INPUT. Schema-4 persistent fields are defined by `device_settings`; broader typed configuration metadata does not imply every field has an effective/persistent provider. Effective/applied/save evidence stays separate. Configurability not yet implemented is marked planned, not exposed as a fake writable control.

Audio/Spoken letters pages are not proof of listening/playback. Recording/media are implemented manager and API paths, with explicit enrolled consent/indicator, one codec/resource owner, format/quota/retention/export policy and failure/cancellation state. A compiled codec or sample cannot advertise functional recording. No blanket prohibition replaces those plans.

Wi-Fi scan/profile/join and persistent LAN are independent of the selected HID transport. Update check/download/activate and delegated administrator policy are separate operations. An enrolled remote admin may enable a constrained policy without a new physical prompt for every matching update. Ordinary input clients gain no capture/update/admin permission implicitly. Factory/partition/eFuse migration remains separately reviewed cabled work.

## 8. UI, state and events

Follow the current [interaction contract](interaction.md): core-only mode screens, on-demand Menu/Status/Settings and the AI double-click panel. Device/browser clients derive controls and choices from the same metadata; this document does not duplicate UI geometry.

Publish a complete bounded status snapshot, default at most 10 Hz with 500 ms heartbeat, plus immediate bounded safety/availability events. Include boot/domain generations, state sequence, monotonic time and firmware/config revisions. Large schemas, scans, bonds, profiles, files and history are paged resources. A stale/missing field is unknown/unavailable, not zero/healthy. No generic green Connected conflates Wi-Fi, selected BLE/USB link, audio profile and readiness.

State exposes actual owner/producer/lease, mode/target/transport, release barrier/held input, queue capacities, draft/send started/completed/uncertain progress; hardware/driver/implementation/qualification/availability with reason; effective/desired/saved/apply/restart/storage uncertainty; network and peer identities without invented names; audio consent/indicator/codec owner/profile/mute/volume; recording limits/bytes/duration/finalization/artifact/retention/export; background policy/job phase/deadline/cancel/cleanup; update staged/selected/running/confirmed/rollback; and measured health with units/age.

Always expose acquire/release/stop/lock and capture stop in their authorized context. No automatic acquire/resume on page focus/reconnect. At 1,500 ms stale input state, disable input and stop lease renewal; blur/hide/pointercancel best-effort stop, backed by device deadman. Background jobs use their separately declared lifetime. Unavailable audio/recording/update actions show reasons and cannot animate pretend output. Required physical challenges and active administrator policy are shown distinctly.

Subscriptions authenticate before first snapshot and throughout lifetime, scope private preview/media/results and enforce bounded consumer queues. Gaps request replacement state, never reconstruct input. A bridge with multiple tabs/AI callers must arbitrate distinct logical owners instead of silently combining them under one lease.

## 9. Resource and failure budget

These are bounded implementation/resource contracts; physical timing and coexistence qualification remains separate.

| Resource | Initial bound / failure behavior |
| --- | --- |
| Network clients | Two authenticated clients, one input owner; four total HTTP/TLS sockets and one handshake in progress. Reject newcomers rather than evict healthy input |
| Enrolled clients | Four principals/two live auth sessions; no silent trust eviction |
| Commands | 2 KiB UTF-8 JSON, depth 4, 32 members, no batches/compression/binary media. Fragment total bounded before allocation |
| Queues | 16 remote admission records, one 64-character preview job; current production HID queue 32 actions and text batch 32 bytes. Account each layer and reserve neutral-release resources |
| Rates | Default 60 input commands/s, burst 8, with separate bounded safety/renewal lane; excess rejected, abuse closes session |
| Results/state | 64 retained command records with 16 active slots, 16 copied completion mailbox entries, one <=4 KiB replacement status and at most two queued outgoing media frames. Slow readers lose their input lease; result loss is unknown |
| Manager work | One radio scan/join and one update/recording resource owner as advertised; finite job/byte/storage/retention budgets, no unlimited offline work |
| Persistence | One coordinated settings apply/save; no per-frame/heartbeat preference writes. Separate authorized recording/OTA flash jobs, explicit uncertainty and power-loss limits |

Network writes/TLS/JSON/storage never run in Bluetooth/audio/IMU callbacks. Input release/local stop outrank scans, diagnostics, optional recognition and update throughput. Flash operations can stall cached execution despite low task priority: initial OTA-to-flash needs a qualified maintenance barrier; concurrent input+flash is not promised. Keep watchdogs effective and measure heap/fragmentation/task/radio/interrupt timing under actual load. Stop/defer optional producers on pressure, never hide dropped key-up.

## 10. Mac MCP and observation

The [implemented companion](../bridge/README.md) pins the official Python MCP SDK v1.30.0 and runs local stdio. It bundles no browser web server. A future Streamable HTTP listener must retain the selected MCP version's authentication/Origin/Host rules; it is not enabled by changing a bind address. Device TLS credentials remain in protected Mac storage, never browser/model arguments.

Generate typed MCP tools from the full operation/configuration registry, with the same scopes, ranges, reasons and results. Include every functional family for authorized clients; capability discovery explains unavailable operations. No divergent smaller MCP feature list, arbitrary command/code execution, secret parameter injection or remote unlock. A policy-authorized update tool has exactly the device's delegated bounds, not an AI-specific bypass.

Tool timeouts/cancellation best-effort stop remaining work and report known progress/unknown outcome, never automatically retry non-idempotent input. Input lease renewal requires the actual caller/fresh state. Explicit finite policy-bound jobs may outlive that caller only within their device-approved delegation.

Screen observation is an opt-in Mac service with real permissions, target mapping, age and provenance. Mosaico cannot capture an iPad or infer host focus. Treat OCR/screen text as untrusted data; it cannot alter scopes, satisfy physical challenges, approve recording/update authority or choose another target. Dependent plans stop on unavailable/stale observation.

## 11. Completion sequence and open evidence

1. Keep the generated function/configuration schema, local manager behavior and authenticated transport contract consistent; update concrete coverage and unsupported-operation reasons as owners change.
2. Derive OpenAPI 3.1, shared JSON/event/media schemas, compact firmware metadata, UI choices and MCP bindings from one registry. Validate examples and coverage; schemas alone do not make capabilities functional.
3. Integrate the shared registry with existing local managers first, then authenticated read-only LAN and scoped input/configuration commands. Qualify local takeover/release/replay/security on actual target transports.
4. Implement complete audio/recording or scan/profile/update managers only under their coordinated contracts; expose them after driver/resource/privacy/failure gates. OTA additionally needs safe migration/backup, signing/trust and app-controlled confirmation.
5. Independently review source and exact delivered artifacts; record hardware/host/power-loss/soak evidence separately.

The reviewed SDK pin is 383dda6c182e3dabf6355160166fe007df775629. Requirements record a v1.2 development board; adapters still identify each actual board. Remaining evidence includes TLS/browser trust/time onboarding, JSON/SDK allocation bounds, BLE/USB/radio/flash coexistence, actual audio/recording routes, capture/storage/privacy behavior, target observation, migration/recovery, signed-image rollback and extended powered operation. Multiple simultaneous HID targets remain future work with independent identities/generations/queues.
