# Remote control and Mac bridge design

Status: proposed architecture, reviewed 2026-10-21. This document specifies future behavior; it does not establish an implemented API or device qualification. Product priorities and local behavior remain defined by [requirements](requirements.md), [architecture](architecture.md) and [interaction](interaction.md). The companion [test plan](remote-control-test-plan.md) defines evidence needed before enabling each capability.

## 1. Baseline and decisions

At inspection, `main` was at `5350666a2947fddf33804781490353bfe7815418`. The [firmware entry point](../firmware/main/main.c) initializes the BSP and a bring-up label. It does not connect BLE HID, product controls, audio or a remote server. The [portable core](../components/input_core/README.md) supplies mode/owner/epoch gates, a release barrier, motion integration and ASCII encoding. It does not implement authentication, leases, a command queue or a network adapter. Other work may add components after this baseline; that does not automatically qualify this proposed protocol.

The intended product has two independent uses:

- Standalone: local IMU, touch and AI button control one BLE mouse/keyboard target without Wi-Fi or a Mac. Microphone/speaker functions become available only after their own implementation and qualification.
- Remote: an authenticated LAN browser or Mac bridge exposes every implemented product function, including settings and state, through the same coordinator. Some operations require physical presence or a separate administrative privilege. A Mac can host MCP, AI and separately authorized screen capture/recognition.

Initial decisions:

1. Remote control starts **disabled and unclaimed**. Existing network credentials alone never authorize input. A provisioned device may later have remote service explicitly enabled, but every boot/reconnect starts without an owner or resumed job.
2. One HID target and one remote owner at a time. Multiple remembered bonds or viewing clients do not imply multiple active targets.
3. The device enforces authentication, lock, ownership, epoch, capability and deadline checks. AI and the Mac bridge cannot bypass these checks.
4. Use ESP-IDF HTTPS plus a small versioned JSON-over-WSS control protocol. Run MCP on the Mac, not on the microcontroller. Serve static embedded browser assets without a frontend framework or CDN dependency.
5. No target video runs through Mosaico. PiKVM/TinyPilot provide interaction references, not a video-capture feature for this board. iPad screen capture requires an independently configured Mac-side capture path; its availability and permissions remain separate from BLE connectivity.
6. Domain logic, command schemas and ownership policy are portable across supported ESP32 boards. Hardware-specific features are discovered through board adapters and truthful capabilities, never inferred from the ESP32 family name.

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
| [S31 OTA documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/system/ota.html) | Separate image verification, boot selection and post-boot confirmation | Future privileged operation. The current single-app partition configuration does not establish rollback-capable OTA |

### Official Mosaico factory code: selection assessment

The following files were inspected at factory-firmware commit `2e1017e42d507cd9216d4da5b2c7d45cb2007971`. Its [submodule declaration](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/.gitmodules) identifies `espressif/esp-claw`; the reviewed tree pins it to `62aaeed7de8f3055615448620dd00ed39fd6af13`. These are research snapshots; no dependency has been added here.

| Reviewed implementation | What exists | Proposed adaptation |
| --- | --- | --- |
| [Network provisioning service header](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/network_provisioning_service/include/network_provisioning_service.h) and [implementation](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/network_provisioning_service/network_provisioning_service.c) | Create/start/stop/status/callback API, four observer slots, STA/AP snapshot, Wi-Fi-join QR and reload/apply of persisted config. The advertised portal URL is HTTP | Reuse the bounded status/service shape and board-network integration after review. Strip setup QR secrets from normal state. This wrapper is not evidence of Security 2 enrollment, runtime client authentication or safe HID access |
| [Wi-Fi manager API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/wifi_manager/include/wifi_manager.h) | Apply STA config, queued radio enable/disable, status, scan records and independent event subscribers | Adapt the event boundary into the network manager. Add explicit transactional staging, rollback, coordinator cancellation and privileged local confirmation; do not expose driver calls directly to remote input |
| [HTTP server core](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_core.c), [status API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_status_api.c), [configuration API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_config_api.c) | `httpd_start` on port 80; `GET /api/status`, `POST /api/restart`, and GET/POST `/api/config`; configuration response distinguishes save from restart-to-apply | Useful route separation and adapter callbacks. Do not import restart, general file/Lua/IM routes or assume its portal is an authenticated input service. Use this document's TLS, session, origin, scope and presence gates; versioned product settings must expose saved versus applied revisions |
| [Capability API](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/http_server/http_server_capabilities_api.c) | `GET /api/capabilities` enumerates compiled capability groups | Adapt discovery; add physical presence/driver initialization, target qualification, current availability and reason. A compiled group alone cannot authorize a button or advertise working audio |
| ESP-Claw [MCP server header](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/include/cap_mcp_server.h), [server implementation](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/src/cap_mcp_server.c), [dependency manifest](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_server/idf_component.yml) | Init/add-tool/start/stop wrapper, string-property tool registration, HTTP MCP manager and mDNS; manifest requests `espressif/mcp-c-sdk ^2.0.0` | Valid embedded-MCP reference for a later alternative. Current design keeps typed MCP schemas and authentication on the Mac to avoid another embedded HTTP manager and tool runtime. Do not expose these tool callbacks as a second direct HID path |
| ESP-Claw [MCP client implementation](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/components/claw_capabilities/cap_mcp_client/src/cap_mcp_client_core.c) | Initializes remote HTTP sessions and lists/calls remote tools through the MCP manager | Not needed for a device that receives Mac control. Leave outbound tool discovery, arbitrary server URLs and on-device autonomous agent execution out of the initial firmware |

The reviewed first-party service/MCP files carry Apache-2.0 notices; the [ESP-Claw root license](https://github.com/espressif/esp-claw/blob/62aaeed7de8f3055615448620dd00ed39fd6af13/LICENSE) is also Apache-2.0. Review transitive modules, assets and exact selected files before any copy, retain provenance and pin replacements. This focused source review is not a full security audit of either upstream project. It supports selective reuse without adopting conversations, memory, Lua, IM, cloud credentials or the full agent lifecycle.

## 3. Components and data flow

```text
Browser UI -------------------+        Local touch / AI / IMU
                              |                  |
AI host -> Mac MCP server -> HTTPS/WSS adapter -> input coordinator
              |               |                  |
       screen observer        |        mouse / keyboard / preview
       (Mac only)             |                  |
                              |             HID scheduler -> BLE target
                              |
                         state publisher <- coordinator + typed manager events
                                                |
                            audio / settings / network / update managers
```

The network adapter authenticates, parses and bounds data, then posts typed intents. It never calls BLE, alters `input_state_t` or touches LVGL directly. The production coordinator is the only state owner. It uses the same mode transitions, release barrier, preview rules and cancellation path for local and remote producers. The future audio manager exclusively owns codec/capture/playback; remote APIs do not open another audio pipeline.

The HID scheduler rechecks the request's epoch, target, owner, lease, mode, deadline and release barrier immediately before each report. Asynchronous BLE, audio, provisioning and calibration callbacks carry the originating generation; stale completions cannot clear a newer barrier or update a new target. Slow network consumers never block HID scheduling. Snapshot generation observes authoritative state and does not alter it.

Settings use typed schemas, versioned revisions and atomic validation. No arbitrary register/GPIO write, shell, URL fetch, file upload or general script evaluator exists in the ordinary control API.

### Board adapters and portability

Keep GPIOs, bus instances, sensor axes, codec formats, power controls, display/touch setup, motor drivers and Bluetooth controller/stack details inside a selected board/platform adapter. Portable coordinator, lease, parser, selection, preview and scheduler policy code depend on typed interfaces and capability descriptors, not Mosaico headers. A board profile is build-selected and validated at startup; remote callers cannot select arbitrary pins or change the MCU target.

Report hardware presence, driver initialization, implementation and qualification separately. A missing motor, display, touch controller, IMU, microphone, speaker, BLE HID path or Classic Bluetooth controller yields an unavailable capability with a reason, rather than a success-shaped stub. A headless board may still expose the authenticated web UI, but enrollment, local takeover, lock/unlock and microphone consent need a qualified physical control/presence mechanism. Without that mechanism, remote input/capture stays disabled; a browser click does not replace physical presence.

ESP32 variants differ in radio and peripheral support. In particular, ESP32-S3 does not supply Classic Bluetooth HFP/A2DP merely because another board supports BLE; see the official [chip Bluetooth capability table](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/bt-architecture/overview.html). Qualify each board/SoC/SDK/host combination and gate Classic audio independently. External audio hardware cannot create a missing controller transport. Shared domain tests run unchanged against adapter substitutes; board integration tests validate the actual drivers and presence controls.

### Persistent settings

Distinguish `protocol_version`, `settings_schema_version` and monotonically increasing `settings_revision` for the committed desired configuration. Also expose each manager's `applied_revision`, `pending_apply` keys and apply failure reason; saving a preference does not prove hardware accepted it. A configuration command carries the expected saved revision; validate the whole change, then commit one transaction and publish its resulting saved revision only after persistence succeeds. Return `storage_failed` on failure and retain the previous committed configuration. A durable save awaiting application is `saved`, not `applied`; the latter requires the manager's actual completion. Coalesce desired-state changes into one explicit save, rate-limit flash commits, and never persist each motion event or lease renewal.

Persist only explicit preferences: supported gain/axis/filter profiles, host shortcut/layout profile, volume preference, global haptics enable, and separately authorized network/trust configuration. Versioned migrations preserve the user's haptics-off setting and validate values against the new board capabilities. An unsupported stored setting is reported inactive with its reason; it must not enable missing hardware. On corrupt or incompatible settings use documented safe defaults and report recovery; do not erase trust or silently enable remote access.

Leases, owners, jobs, held keys/buttons, motion-enabled intent, recording/listening, microphone consent/unmute and queued text are session state and never persisted for automatic resumption. Audio starts off/muted after boot. The remote-service enable preference may be saved only after explicit local setup; even then boot starts unclaimed. Store secrets outside ordinary preference exports. Any settings import requires schema/board validation and cannot import a live session, unlock or presence approval.

## 4. Enrollment, secure transport and sessions

### Local enrollment

1. A local person opens **Settings -> Remote -> Enable / enroll controller** on the unlocked device. Remote input cannot open this presence window. The display shows device identity, a short-lived QR/setup secret and remaining setup time. Initial proposal: 120 seconds, one enrollment attempt in progress, five failed authentications before closing the window. A fresh local action is needed to reopen it.
2. Provision Wi-Fi and enroll a client using an official Security 2 provisioning client over a locally enabled transport, or a separately qualified USB setup channel. A native Mac setup utility is the initial client; a normal browser does not automatically implement ESP provisioning or certificate pinning. Setup credentials must be unique and high entropy, not a fleet default or a short unauthenticated PIN.
3. Generate device TLS identity and client credentials using the platform CSPRNG. Bind the enrollment to the device certificate fingerprint conveyed through the authenticated setup channel and checked on the physical display. Do not use unauthenticated trust-on-first-use over the LAN. Expire setup material on success or timeout; stop the setup AP/service. Production input is never available over the provisioning HTTP endpoint.
4. Store the Mac's client secret in Keychain and device verifiers in appropriately protected persistent storage. Establish the release security configuration, including available flash/NVS protection and key recovery, before distributing credentials. Never place real credentials, Wi-Fi passwords, private keys or QR payloads in source, logs, screenshots retained in Git, command-line arguments or model prompts.
5. Local settings list enrolled principals with names and scopes and allow revocation or disabling remote service. Revocation immediately closes that principal's sessions and cancels its work. Resetting trust requires physical presence; it does not silently erase BLE bonds or unrelated data.

Wi-Fi provisioning, application-client enrollment and Bluetooth target pairing are three different trust relationships. None authorizes the other two implicitly.

### Practical HTTPS choices

| Deployment | Trust and browser path | Tradeoff |
| --- | --- | --- |
| Initial Mac bridge, recommended | Native bridge pins the enrolled device certificate, authenticates over TLS, and serves a browser UI from a strictly loopback-bound local service | Avoids browser-to-device certificate provisioning. Mac is required for this remote path; standalone BLE still works without it |
| Direct device browser, later | Device HTTPS/WSS uses a certificate trusted by the browser for the exact configured hostname/SAN, provisioned through a managed local CA or explicitly installed trust | Requires real certificate issuance, installation, renewal and hostname handling on each browser/OS; mDNS does not confer trust |
| LAN UI hosted by Mac, later | Browser trusts the Mac's HTTPS hostname/certificate; Mac independently pins device TLS and authenticates both sides | Adds another server, session boundary and certificate lifecycle; do not expose a loopback HTTP service by changing its bind address |

Do not make users bypass TLS warnings, disable certificate verification, or send credentials over ordinary LAN HTTP/WS. Public CA issuance for private names/IPs is not automatic. Browser mixed-content and private-network policies also need actual Safari/Chrome testing. If direct browser trust cannot be installed, offer the Mac bridge or keep the feature unavailable; do not downgrade security. Private-LAN placement is an exposure limit, not authentication. No default Internet forwarding, cloud relay, UPnP or WAN listener is proposed.

### Runtime authentication and browser boundaries

- Principals have explicit `view`, `control`, `configure`, `pair`, `update` and optional `preview.view_private` scopes. A controller receives `view`; the other scopes are separate grants. Each session's effective scope is checked on every command. A lease never grants additional scopes.
- Native clients exchange their enrolled random credential through `POST /api/v1/sessions` over verified TLS. Browser login uses a separate locally enrolled browser credential or a one-time browser enrollment approved by an administrator; never expose the Mac's device credential to browser JavaScript. Reject login with an unexpected Origin or Host as well as authenticated mutations.
- Browser sessions use a random, host-only `Secure; HttpOnly; SameSite=Strict; Path=/` cookie, proposed 15-minute idle and 8-hour absolute expiry. No long-lived bearer token in localStorage or a URL. Logout, expiry, trust reset and revocation close WSS and cancel associated leases immediately.
- Mutating HTTP bootstrap endpoints require same-origin JSON, an unpredictable session-bound CSRF token in a custom header, exact Origin and allowed Host checks. Before login, a same-origin challenge endpoint issues a restricted pre-authentication cookie/nonce usable only for login; login requires both that nonce and the credential and rotates the session. GET does not mutate product/configuration/input state. Deny wildcard CORS, arbitrary reflected origins, `Origin: null`, unexpected proxy hosts and `file://` use. Serve UI and browser API from one origin. Apply a restrictive CSP with local assets, `frame-ancestors 'none'` and no third-party scripts; insert preview/error text as text, never HTML.
- Browser WSS upgrade requires the session cookie, an exact Origin/Host match and a single-use 10-second upgrade ticket obtained via CSRF-protected `POST /api/v1/ws-ticket`. Carry the ticket in an agreed WebSocket subprotocol value, not a query string; redact it from handshake logs and consume it once. This is necessary because browser WebSocket constructors cannot freely set Authorization headers. Negotiate the ordinary protocol subprotocol separately. Do not send commands before authorized upgrade and channel initialization.
- Native WSS uses an Authorization header and a native-only enrolled principal. Missing Origin is accepted only on this authenticated native path; supplying an invalid Origin is always rejected. Origin checks complement authentication and are not a credential.
- The loopback Mac UI uses its own browser session, CSRF token, exact configured loopback Origin/Host allowlist and single-use upgrade ticket. Bind to explicit loopback addresses, not all interfaces. A local bootstrap handoff from the native utility creates the browser session without putting credentials in URLs. Loopback HTTP/WS is limited to this on-Mac boundary; device traffic remains authenticated TLS. Verify the supported browser's localhost secure-cookie behavior; if required cookie guarantees are unavailable, use trusted loopback HTTPS/WSS or a native authenticated view, not weaker cookies or a LAN HTTP listener. Other local processes are outside the browser-origin isolation guarantee, so Keychain access and OS user permissions still matter.
- Bind runtime listeners to the intended station interface or enforce equivalent verified filtering for the chosen SDK. Do not accidentally expose control on a provisioning AP. mDNS advertises service identity/version only; possession of an advertisement proves no trust.

## 5. Ownership, lock and bounded time

There is one active owner: `local`, `remote(session_id, lease_id)`, or `none` while release/recovery prevents output. Viewing state never takes ownership. The existing portable enum is a primitive; the adapter/coordinator must implement the complete lease record and absence/recovery states.

| Event | Coordinator transition |
| --- | --- |
| Authenticated explicit `lease.acquire` | Require remote enabled, unlocked, matching target/mode, safe local idle state and no other lease; cancel prior producer work, increment epoch, freeze motion and establish release barrier. Grant at most 3,000 ms, with HID output unavailable until linked and neutral reports are accepted. A null-target lease may manage permitted device settings but cannot emit HID |
| Competing acquisition | Reject `owner_busy`; do not preempt, steal, queue an acquisition or extend the current lease |
| Renewal | Same authenticated live channel and lease only; explicit renewal about every 1,000 ms, extending at most 3,000 ms from device receive time. Include a state sequence received within the last 1,500 ms; socket ping alone cannot renew ownership |
| Local AI down or first touch | Immediately revoke remote lease, invalidate epoch, cancel queue, schedule neutral reports and consume this first physical action. Continued AI hold can lock; the next deliberate local action controls UI |
| Local motion during remote ownership | IMU output remains gated off. Wrist movement alone is not a takeover signal |
| Mode/settings transition affecting output | Invalidate epoch and release/reset producer state. An otherwise valid remote lease may remain, but later commands must use the new epoch after the transition and barrier. No batch crosses this boundary |
| Lease expiry/release, control-channel loss, state-consumer timeout, session expiry or remote disable | Cancel jobs, invalidate epoch, release HID, clear motion intent and stop remote capture/listening. Do not resume after reconnect |
| BLE disconnect, target replacement, reboot, global lock or update entry | Invalidate epoch, revoke lease, cancel residual work and mute. Reconnect/new boot needs a fresh channel, snapshot and explicit acquisition |
| Unlock | Physical AI long press only; keep motion off, microphone muted and remote unclaimed. Remote `unlock` is always `local_presence_required` |

Do not grant a remote lease while a local drag, send, listening window, modal confirmation or motion producer is active. Return `local_busy`; the device must visibly enable **Allow remote control** before acquisition. A local takeover closes that allowance until the local user explicitly enables it again. Remote acquire cannot immediately fight local activity after takeover.

Presence-gated operations use a two-step proposal, not an exception to takeover. A remote request can display one inert, expiring review proposal but cannot apply it. The first physical action still revokes the lease and is consumed; the cancelled remote job never resumes. The local person then reviews the exact action, principal, target and settings revision. They may execute it as a new local intent, or grant one short-lived, single-use approval bound to that action digest, principal/channel and target. A remote continuation requires explicit local allowance, a fresh lease and a new request using that approval. Bind approval to that new lease when acquired; invalidate it on lock, channel loss, target change, cancellation or expiry. Default approval/proposal lifetime is 60 seconds. No remote command can approve its own prompt, and delayed callbacks from the cancelled request remain stale. Microphone consent follows this flow and remains limited to the approved session; trust enrollment and global unlock remain physical-only.

All deadlines use the device's monotonic milliseconds, never wall time or an untrusted client clock. Publish `device_now_ms` with each state snapshot. The client estimates an offset only to choose a conservative absolute `deadline_ms`; the device rejects a past deadline or one beyond the operation's horizon. Queueing and retries never extend deadlines. An expired command must not emit a new nonzero report. Required neutral releases bypass job deadlines, revoked permissions and normal queue capacity, but are restricted to clearing output.

On detected close, revoke immediately. On a silent broken connection, stop no later than lease expiry; proposed watchdog scheduling budget is an additional 50 ms to initiate release, to be measured on hardware under load. Mouse hold has its own 1,500 ms maximum lifetime, refreshed explicitly while a valid lease remains. Atomic key taps/chords schedule their own release with a maximum 100 ms hold. There is no indefinitely held raw keyboard API in v1.

If neutral mouse or keyboard reports fail, retain `release_pending=true`, show `release_failed/recovering`, block new nonzero HID and retry a bounded three attempts within 300 ms while the link is usable. On continued failure request Bluetooth link teardown and report its actual result; do not report a successful release because a timer expired or a disconnect was requested. On reconnect accept neutral reports for the new epoch before enabling input. Host-side receipt remains unobserved without external evidence.

## 6. Protocol v1 proposal

### Routes and channels

| Endpoint | Access | Meaning |
| --- | --- | --- |
| `GET /` and versioned static assets | Public shell only | No sensitive state or credentials embedded |
| `GET /api/v1/login-challenge` | Exact allowed browser Origin/Host | Restricted pre-authentication cookie/nonce, no product state or control access |
| `POST /api/v1/sessions`, `DELETE /api/v1/sessions/current` | Enrollment credential / active session; browser origin and CSRF rules apply | Login/logout, never input |
| `GET /api/v1/state` | `view` | Complete authorized snapshot; `Cache-Control: no-store` |
| `GET /api/v1/capabilities` | `view` | Operation schemas, limits, availability and qualification for this exact build/target |
| `POST /api/v1/ws-ticket` | Browser session plus CSRF | One-use upgrade ticket |
| `GET /api/v1/ws` with `esp32-input.v1` | Authenticated upgrade | Initial snapshot, commands, request results and state updates |
| `GET /api/v1/requests/{request_id}` with originating session ID | Same principal as original request | Bounded recent result lookup; unavailable history returns `result_unknown`, never resubmits work |

All ordinary mutations use the single ordered WSS channel. There is no parallel REST HID endpoint. On upgrade the device generates a fresh channel `session_id`, sends a complete snapshot and waits for its acknowledgement before allowing acquire. An authentication session may outlive a channel, but a lease cannot. Reconnect creates a new channel session and never retransmits unacknowledged actions. A read-only observer also authenticates before receiving the first snapshot.

### Command envelope

```json
{
  "v": 1,
  "boot_id": "019ab012b8364a589ce09206b5402f33",
  "session_id": "6c1f67ecaf88437393b4ca61ca5bd291",
  "request_id": "41d5308efce542270000000000000008",
  "seq": 8,
  "epoch": "42",
  "target_id": "target-1",
  "lease_id": "ae92c6803978456ab40dfb8dc7bb80b3",
  "deadline_ms": "813250",
  "op": "mouse.move_relative",
  "args": { "dx": 3, "dy": -2 }
}
```

IDs shown are non-secret illustrative values. Actual boot, session and lease IDs are fresh 128-bit random identifiers encoded as 32 lowercase hexadecimal characters. Each channel also receives a random 64-bit request prefix; its `request_id` is that prefix followed by `seq` encoded as 16 zero-padded lowercase hexadecimal characters. This makes ID/sequence binding verifiable without retaining an unbounded set of old IDs. Request IDs are correlation values, never authentication credentials. `target_id` is a device-issued opaque binding to the selected bond, never a caller-chosen Bluetooth address. Target reconnect/replacement also changes epoch. `epoch`, state counters and device monotonic timestamps use unsigned decimal strings to avoid JavaScript integer precision loss. `seq` is an integer from 1 through 4,294,967,295; renew the channel before exhaustion. Epoch overflow fails closed and requires a fresh boot generation; it never silently wraps.

For acquire, use `lease_id:null`; for device-wide operations without a paired target, use `target_id:null` only when the schema permits it. The current target must match even for a device-wide mutation when one is selected, preventing stale dashboards from changing another session's configuration. The sender cannot choose its principal, local source, scopes or presence status. These come from trusted session/physical event records.

Validation order is bounded framing/schema, authentication/session, duplicate/sequence decision, boot/epoch/target, scope/capability/mode/lock, lease/deadline, then capacity. Recheck mutable gates at execution. Reject unknown operations/fields, invalid types, nonfinite/fractional integers, invalid encodings or out-of-range arguments rather than clamping/truncating. Unsupported major versions never reach the coordinator.

### Results and truthful progress

Keep admission, execution and observation as separate fields/events:

| Term | Exact evidence | Does not prove |
| --- | --- | --- |
| `rejected` | Validation or admission failed; no effect from this request | A previous duplicate request did not run |
| `accepted` | Coordinator reserved bounded work, recorded immutable arguments and returned a request record under the current gates | Any HID report, persisted setting or target UI effect |
| `running` | Execution has begun, with explicit current report/progress counters | Completion or delivery of all required releases |
| `saved` | Desired settings transaction committed durably; runtime application is separately pending/failed | An active hardware or network change |
| `applied` | Device-local setting/preview/state transition completed, with resulting revision/epoch | Any target input; use this for non-HID operations |
| `sent` | For HID, every required report, including matching releases, was accepted by the production BLE send API for this request and generation | Radio delivery, OS processing, focus, text insertion or application success |
| `cancelled` / `failed` | Execution stopped, with a reason and exact report/prefix progress; `failed` denotes an execution error | That already emitted effects were undone |
| `observed` | Separate Mac observer evidence with target mapping, capture session/frame ID, time, predicate and `verified`, `inconclusive` or `failed` evaluation | Device acknowledgement, guaranteed causality or proof beyond the stated predicate |
| `result_unknown` | The caller cannot retrieve sufficient execution evidence, for example after result eviction or reboot | Failure, success, zero effects or permission to retry |

An `accepted` reply includes `request_id`, originating session, epoch, queue depth and deadline. A terminal result also includes `execution`, `reason`, `reports_accepted`, `sent_prefix`, `uncertain_index`, `release_pending`, resulting epoch/revision and device timestamp. Non-HID runtime operations use `execution:applied`; a save-only operation can end `saved` with explicit application status. Request receipt or HTTP 200 is neither result. A pairing job terminates with an actual link/bond result, not merely an open dialog. Results never use an undifferentiated `success:true` for input.

An explicit mouse-button hold remains `running` after its down report; it terminates only when its matching release is accepted, or with cancellation/failure and a truthful release state. Refresh updates its deadman only; it cannot extend the original request deadline. Thus the initial protocol bounds one continuous hold to at most the 2,000 ms discrete-operation horizon; the UI shows this limit. A later longer-gesture capability needs its own bounded stream contract and tests. Atomic clicks/chords include both down and release in `sent`. A movement request needs no extra button transition and cannot claim that an independently active drag has ended.

For text, `sent_prefix` counts characters whose key-down and zero key-up have both been accepted by the BLE API. If key-down is accepted but release fails, expose that character as `uncertain_index`, retain only never-started characters as a retryable draft, and show the uncertain character separately for manual reconciliation. Never silently requeue the uncertain character. The phrase accepted prefix in local UI refers to transport-accepted report pairs, not admission of the whole text job. No automatic suffix retry or appended Enter is permitted. An operation interrupted after emitting reports cannot be described as having no effect.

The device cannot produce target-screen `observed` events. The Mac keeps observations in a separate namespace, links them to a request, and exposes their provenance. Stale frames, wrong target mapping, denied capture or ambiguous recognition yield `inconclusive`/`unavailable`. Reading text on a screen is an inference about the target outcome, not an elevation of `sent` into a device guarantee.

### Replay and idempotency

- Serialize commands by channel sequence. Only the next `seq` is eligible for new processing. Every authenticated, well-formed command at that sequence consumes it, even if admission is rejected. Return the next expected sequence. Gaps reject `sequence_gap` without consumption. Malformed framing closes the channel and revokes its lease.
- Within a channel, repeating the same `(seq, request_id, canonical payload digest)` returns its cached record without execution or deadline/lease extension. Reusing an ID or sequence with changed arguments returns `request_conflict` and closes the channel. Include operation, arguments and all gating fields in the digest.
- Cache a bounded 64 records per active channel; do not evict an executing record. The sequence high-water mark survives cache eviction for the channel. A lower sequence absent from the cache is `duplicate_too_old/result_unknown`, never new work. Reject a request ID whose prefix or encoded sequence differs from the issued channel/sequence. An old ID cannot become fresh work by using a new sequence, even after result eviction. Rotate the prefix with the channel.
- No durable command journal or automatic replay. A new boot/session/epoch rejects old envelopes. A reconnecting UI first fetches state and recent status if available; it does not reconstruct clicks, key-down, text or leases from an offline queue. Unknown outcomes require reconciliation before the person or AI deliberately creates new input.
- Idempotent desired-state setters use explicit values and an expected revision. Avoid toggles for retryable state APIs. Relative motion, click, confirmation and text are non-idempotent effects even though duplicate envelopes can be suppressed. This is bounded at-most-once admission within a channel, not an exactly-once target-delivery guarantee.

## 7. Function coverage and policy

Every operation is capability-gated. A capability describes `implementation: absent|present`, `qualification: pending|passed|failed|not_applicable`, `available:boolean` and a concrete `reason`; availability also depends on target, mode, resources, trust and lock. A UI may show a future control disabled with **Not implemented**. It may not report a working microphone, pairing flow or send path from a simulated prototype or compiled symbol alone.

Unless stated otherwise, mutations require `control` and the active lease; all output-producing actions require unlocked state and the appropriate mode. Administrative actions additionally require their scope and local presence where listed. `safety.stop`, `lock.set:true`, `audio.mute.set:true` and a neutral-only `release_all` are authorized-controller safety operations without an owner lease; they still require a current authenticated channel, boot/epoch/target and valid envelope. Stop/release revoke active remote work and cannot emit nonzero input or unlock. A viewer alone cannot invoke them. Initial limits below are protocol proposals, not measured performance claims.

| Function / proposed operations | API/UI behavior and constraints |
| --- | --- |
| Mouse: `mouse.move_relative`, `mouse.scroll`, `mouse.click`, `mouse.button`, `mouse.hold_refresh` | Signed integer HID units, never screen pixels; each axis/wheel -127..127 per request, reject overflow. Left/right, explicit drag/release; optional buttons require a supported descriptor. Click schedules its own release. Hold refresh cannot outlive lease or 1,500 ms deadman. No absolute-coordinate claim |
| Motion and touch fine control: `mouse.motion.set`, `mouse.fine_gain.set` | Fine gain selects supported values, initially 1/4 or 1/8. Remote can turn motion off and configure gain. Enabling the local gyro producer ends the remote lease, increments epoch, and requires a local confirmation before motion starts; no simultaneous remote and physical motion streams. Remote UI displays this handoff rather than claiming gyro output while remote owns input |
| Keyboard: `keyboard.key_tap`, `keyboard.chord`, `keyboard.shortcut` | Allowlisted HID usages, at most six nonmodifier keys with an 8-bit modifier mask; atomic down/release only. Host-specific Ctrl/Command mapping is visible. Explicit Enter key is distinct from text sending. Reject layout/usage mismatches and no arbitrary report-byte injection |
| Preview: `preview.replace`, `preview.delete`, `preview.clear`, `preview.send`, `job.cancel` | 128 printable US ANSI characters per initial draft/job; reject unsupported Unicode, controls and implicit newlines. Revision-match edits; show target, draft and sent/uncertain/unsent progress. Sending requires a separate explicit command and a matching draft revision |
| Selection: `selection.page`, `selection.focus`, `selection.confirm`, `selection.back` | Shared two-level group/character grid and preview, not direct HID. State includes page, focus and revision; confirmation requires expected selection revision so jitter/stale UI cannot confirm a new item. Remote selection does not turn local IMU selection on |
| Physical button equivalents: `ui.primary.activate`, `ui.mode_chooser.open`, `lock.set` | Semantic intents map to the visible context: mouse motion handoff, selection confirmation, spoken-letter action, audio mute. No raw AI/BOOT/POWER GPIO or fabricated local-presence event. Mode chooser is AI double-click equivalent. `lock.set:true` is the stop-and-lock equivalent; `false` always requires local long press. Hidden/blocked pages reject activation; remote UI should prefer explicit desired-state APIs |
| Modes: `mode.set` | Mouse/keyboard/audio using shared transitions; release drag, clear motion intent, cancel sending and increment epoch. Audio mode requires implemented capability. Cold boot remains mouse/motion off |
| BLE target: `pairing.request`, `pairing.cancel`, `bond.remove`, `target.connect`, `target.disconnect` | `pair` scope plus local confirmation for pairing/rebond, bond removal or changing target. Window proposed 60 seconds; visible identity/passkey and SDK-supported secure pairing. Remote can request or cancel a window but cannot confirm its own presence challenge. One target initial; no silent target choice, passkey acceptance or target unlock |
| IMU: `imu.calibrate`, `imu.settings.set` | Calibration suppresses host output, requires stationary samples and returns measured pass/fail/bias/quality, not a fixed success timer. Calibration/settings cancel output and change epoch. Advertised schema defines axis orientation, gain in HID units/degree, deadzone in degrees/second, filter constants, actual sample timing and bounded freeze durations. Reject unqualified profiles; no remote raw flash writes |
| Haptics: `haptics.enabled.set`, `haptics.test` | Global on/off preference, `configure` scope, expected settings revision and atomic persistence. Proposed default is enabled only on a present, initialized motor; expose the saved preference separately from actual availability. Off suppresses all interaction feedback, including test pulses; state/error indication remains visual. On uses short bounded feedback for mode change, confirmation, lock and error, not every motion sample, pointer move or repeated network packet. Test is explicit, rate-limited and at most 200 ms; board adapter defines supported amplitude/patterns. Motor activity freezes IMU output and clears residual motion; missing motor rejects test as unavailable |
| Spoken letters: `speech.listen`, `speech.stop`, `speech.candidate.confirm` | Implemented recognizer only; bounded listening window, candidate with generation/revision, explicit confirmation into preview and separate send. Local microphone consent required; old recognition results cannot enter a new epoch. No implied full transcription |
| Microphone: `audio.voice.set`, `audio.mute.set` | Audio defaults off/muted. Muting is immediate and permitted as a safety action for authorized controllers without owning the lease. Remote unmute/capture requires local consent scoped to this session and explicit visible indication; lock/disconnect/expiry cancels it. No persisted remote-unmute preference or audio recording/upload through this control channel |
| Speaker: `audio.speaker.set`, `audio.volume.set`, optional `audio.playback` | Expose actual HFP/A2DP link, codec, volume, mute and resource ownership separately. AVRCP play/pause exists only if implemented/qualified; volume is explicit 0..100 with manager mapping disclosed. No arbitrary remote audio URL fetch, fake playback or stereo claim. Same-host HID/audio requires combination-specific qualification |
| Wi-Fi: `network.scan`, `network.stage`, `network.apply`, `network.cancel`, `remote.disable` | `configure` scope and local confirmation for applying credentials, changing trust/listeners or re-enabling access. Scan bounded/rate-limited; never return passwords. Stage one candidate, validate, release/revoke, then attempt new network with a 30-second rollback to previous working configuration; report an unknown result if connection is lost. A failed rollback requires visible local recovery. Disabling remote access needs no ability to turn it back on remotely |
| Diagnostics: `diagnostics.snapshot`, optional `diagnostics.capture` | `view` gets bounded redacted health/version/counters/reason codes. Capture requires `configure`, explicit local activation and a short duration; no key/text payloads, secrets, microphone samples or screen images by default. No unauthenticated log stream |
| Firmware: separate `/api/v1/update/*` service, later | `update` scope, physical confirmation, explicit image identity/size/hash/version and signed compatible image. Enter maintenance only after release/mute/revocation. Dedicated streaming limits, A/B partition/rollback design and post-boot self-check; no arbitrary URL fetch, bootloader/partition rewrite or generic MCP update tool. Normal control credentials cannot perform updates |

Remote access to all supported functions means a complete control surface with truthful availability and the same safety constraints as local operation. Physical power-off, global unlock, trust enrollment and microphone consent remain presence boundaries, not missing remote-HID features.

## 8. State and browser UI

Publish an initial complete snapshot and complete replacement snapshots on change, capped at 10 Hz, plus a 500 ms heartbeat. Each includes boot ID, epoch, monotonically increasing `state_seq`, monotonic device time, schema version and firmware/config revisions. Critical lock/revoke/release failures also generate immediate bounded events. State is authoritative; client animation is not a state transition.

Required fields:

- Readiness: remote enabled/claimed, board profile and adapter version, hardware presence and driver status, implementation/qualification/availability per function, release barrier, update/recovery/calibration state and explicit block reason.
- Connectivity: Wi-Fi station state and safe network label, authenticated control channel, selected HID target/bond/link, distinct audio peer/profile/link, layout and qualification profile. No single green **Connected** indicator.
- Ownership: local/remote/none, safe controller label, lease expiry/remaining time, permission scopes, local allowance and consent status. Never expose another client's token, ticket or lease secret.
- Modes/input: foreground mode, motion intent and actual output gate, fine gain, selection page/focus/revision, mouse held buttons, modifier/key release state and mute/capture indication. Haptics exposes saved global enable, motor availability, active pulse/freeze and suppression reason; settings expose schema/revision, persistence state and recovery reason.
- Work: queue depth/capacity, active request, deadline, draft revision, sent/uncertain/unsent counts, last cancellation/error, state freshness and dropped-observation counters. Preview contents require an explicitly granted private-preview view; a basic observer gets length/progress only.
- Health: bounded heap/queue/task-watermark/radio/audio counters with measurement units, latest fault and capability reasons. Missing measurements are `null` with `unavailable`, never zero or healthy.

The browser mirrors Mouse, Keyboard and Audio pages plus settings. A persistent header shows target, separate HID/audio links, mode, owner, lock, freshness and readiness. **Acquire control**, **Release**, **Stop** and **Lock** remain easy to find. Acquire is explicit; focusing the page or opening a socket never claims input. Display lease countdown and **Stopped: local takeover / expired / disconnected**. No automatic reacquire or script resume.

Mouse shows relative touchpad, click, drag/release, scroll and motion handoff. Keyboard shows selection, revisioned preview, explicit send and cancel with precise progress. Audio shows **Not implemented**, **Not qualified** or **Unavailable: reason** when appropriate; disabled controls cannot produce optimistic capture/playback indicators. Settings includes one global haptics switch and truthful save status; missing motor/display/microphone capabilities stay visible as unavailable where relevant, and the browser never implies those peripherals exist. Pairing/network/update panels show pending local confirmation and real job progress separately from completed state.

If no fresh snapshot/heartbeat arrives within 1,500 ms, mark state stale, disable controls, stop emitting and stop lease renewal. On tab blur, page hide, pointer cancellation or capture loss, best-effort release/stop immediately; the device deadman remains authoritative if that message is lost. Return to the tab requires fresh state and explicit acquire. Pointer gestures never accumulate an offline backlog. Keep text drafts in memory with visible target association; persistence/export is opt-in and excludes credentials.

Subscriptions enforce `view` at upgrade and throughout their lifetime. A read-only viewer cannot invoke mutations or inspect private preview without its separate grant. Only the owner gets detailed request progress for its jobs; privileged diagnostics remain scoped. A state sequence gap requests a replacement snapshot, not event replay or reconstructed input. In the one-browser-client Mac bridge, multiple tabs map to distinct logical clients; the bridge must arbitrate them rather than merge their commands under one lease silently.

## 9. Resource and failure budget

These initial bounds are to be encoded in schemas and measured, then revised deliberately if hardware evidence requires it:

| Resource | Proposed bound / overflow behavior |
| --- | --- |
| Connections | Two authenticated WSS clients total, one owner and one observer; at most four HTTP/TLS sockets total and one handshake in progress. Reject excess without evicting a healthy owner; reserve local control resources |
| Principals / sessions | Four enrolled principals and two live authentication sessions. New enrollment requires explicit local selection if full, never silent eviction |
| Client command frames | 2,048 bytes decoded UTF-8 JSON, nesting depth 4, 32 object members total, no batches/compression/binary control frames. Bound length before allocation; fragmented message total obeys the same limit. Outbound state has its separate 4 KiB bound |
| Command queue | 16 fixed-size coordinator records total and one text job, 128-character payload maximum. Reject `queue_full` before acceptance; do not silently drop an accepted action or truncate text |
| Rate | 60 commands/second per owner, burst 8; renewal/stop/release have a separate bounded lane. Excess data requests reject `rate_limited`; sustained abuse closes the session |
| Time | Movement/scroll deadline horizon 250 ms; ordinary discrete/job start-to-finish horizon at most 2,000 ms and never beyond lease expiry. Long calibration/pair/network operations use separate bounded manager jobs and the same cancellation generations |
| Holds | Atomic keys at most 100 ms; mouse hold at most 1,500 ms between explicit refreshes, also bounded by lease/deadline. A release for an admitted hold is not rejected because its job expired |
| Results | 64 metadata records/channel, executing records pinned; up to 16 queued result events/client. Full result output closes a stalled channel and cancels its lease; an undelivered result becomes unknown to the caller |
| State | One replacement snapshot at most 4 KiB per client plus a bounded critical-event slot. Supersede old snapshots with a complete newer one; this never coalesces command execution or hides a failed release |
| Safety | Allocation-free stop/release latch outside data queue; coordinator checks local/lock/lease faults before nonzero reports. Neutral mouse and keyboard reports use reserved scheduler resources |
| Storage | No per-keystroke flash writes, recordings or durable remote job replay. One transactional settings operation at a time; cap diagnostic capture by bytes and duration before enabling it |

Queue limits are global across clients. No unlimited list of jobs, observers, reconnect attempts, text chunks or status history exists. Serialize server API access in the network task; do not run TLS handshakes, JSON parsing, filesystem operations or network writes in Bluetooth/audio/IMU callbacks. Stop low-priority diagnostics/recognition and reduce state refresh under pressure; never hide missing releases. TLS heap cost, task stacks, buffers and Wi-Fi/Bluetooth coexistence need measurement on the actual S31 board before exposing remote readiness.

## 10. Mac MCP and observation bridge

Implement a small local service using a pinned official MCP SDK. Start with stdio to the AI host and a loopback-only human UI; do not open a LAN MCP listener by default. If Streamable HTTP is later needed, follow the selected MCP version's authentication and Origin/Host requirements and keep its access policy separate from the device credential. The reviewed [MCP transport specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports) explicitly addresses local binding and origin validation.

Proposed tools are `device_get_state`, `device_get_capabilities`, `control_acquire`, `control_renew`, `control_release`, `mouse_move_relative`, `mouse_click`, `mouse_drag`, `mouse_scroll`, `keyboard_key_tap`, `keyboard_shortcut`, `preview_edit`, `preview_send`, `selection_action`, `mode_set`, `device_stop` and `device_lock`. Add typed configuration/audio/pairing tools only when the corresponding device capability and principal scope permit them. No `unlock`, arbitrary BLE report, shell or unrestricted macro tool. Keep firmware update outside the general AI tool set.

The bridge owns credentials, sequence assignment and device-time conversion. Model arguments contain intent, expected target and state revision, not device secrets or raw envelopes. Each AI caller/human tab is a distinct logical principal; the device owner binds the bridge session while the bridge also enforces the selected logical owner. One caller cannot borrow another caller's lease. Device checks remain mandatory even if the bridge is compromised or its UI is stale.

Each tool returns structured admission/execution/progress evidence and the latest state. A tool timeout triggers best-effort cancellation/stop, then reports the known prefix or `result_unknown`; it must not retry input automatically. MCP cancellation stops the remaining job and releases held output; it cannot undo already sent input. The bridge renews only while the requesting caller is alive, authorized and consuming fresh state, never indefinitely because its process is still running. Local takeover and lock require a new human enable/acquire flow before any AI task can continue.

Screen capture/recognition is a separate opt-in Mac service, with actual capture permissions and a verified mapping to the BLE target. Offer an independent `target_observe` tool only when configured. Record capture availability, age, target mapping and source; do not claim Mosaico can capture the iPad, unlock it or know host focus. Treat screen/OCR text as untrusted task data. It cannot change tool permissions, satisfy local-presence challenges, select a different target or auto-confirm a dangerous action. AI plans must stop on stale/unavailable observation when their next step depends on it.

## 11. Implementation phases and open evidence

| Phase | Deliverable | Gate before use |
| --- | --- | --- |
| R0 | This design and failure matrix | Documentation/link review only; no new running capability |
| R1 | Production coordinator, neutral-report scheduler and standalone composite HID | Existing P0 host/device gates; cancellation and release-failure regressions pass before adding a network producer |
| R2 | Local secure provisioning, device TLS, scoped authenticated read-only state | Real S31 memory/handshake measurements, revocation/origin/session tests, truthful capabilities. Remote input remains disabled |
| R3 | One-owner WSS mouse/keyboard, matching browser UI and physical takeover | Protocol fault tests, watchdog timing, real BLE interruption tests and LAN/Bluetooth stress. Enable only qualified functions |
| R4 | Local stdio MCP bridge, optional Mac observation | Tool schema/errors/cancellation, secret isolation, caller arbitration and actual capture-target identity/freshness tests |
| R5 | Pair/configuration/IMU/haptics management, versioned persistence and independent audio controls | Per-operation tests, board adapters and local-presence policy; audio/profile/host combinations separately qualified |
| R6 | Firmware update and later direct-browser/LAN deployment | Signed-image lifecycle, partition/rollback/recovery and certificate/session deployment qualification. Separate authorization and release work |

Unresolved evidence is explicit: exact board revision and production SDK pin; full TLS/JSON/HID memory and latency under Wi-Fi/Bluetooth/audio load; browser certificate lifecycle and provisioning client UX; BLE pairing security supported by each target; cancellation/release behavior on each host; actual iPad-to-Mac capture path and permissions; IMU calibration ranges; HFP/A2DP/AVRCP and same-host coexistence. Numbers in this document are initial bounds to test, not measured compatibility or timing results. Multiple HID targets remain future work with per-target identities, generations, ownership and queues; do not simulate them with a single shared connection flag.
