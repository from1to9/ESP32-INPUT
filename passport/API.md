# Passport HID and network interface

This file describes the implemented subset, not the full
[compatibility plan](../docs/passport-compatibility-plan.md).

## External interfaces

| Interface | Available behavior |
| --- | --- |
| BLE HID | Report-only keyboard input (ID 1, eight bytes), keyboard LED output (ID 1, one byte), relative mouse input (ID 2, four bytes), report map, HID information and control point |
| BLE management | One connection, three saved bonds, explicit 60-second pairing or selected-bond advertising, disconnect, individual forget |
| Local input | 12-unit directional steps, bounded direction repeats, left click, Esc/Enter/Tab/Backspace/arrows; all through the shared engine |
| Wi-Fi | Four saved 2.4 GHz profiles, bounded asynchronous scan, explicit AP, observed STA/AP/DHCP state and local confirmations |
| HTTP/WS | Open-session input at the observed LAN/AP IP, read-only `/health`, plus AP-only `/setup` and physically gated Wi-Fi provisioning |

The BLE report payloads exclude the report ID; characteristics and Report
Reference descriptors select it. Both input characteristics must be subscribed
and the connection encrypted and bonded before the initial neutral keyboard
and mouse reports can complete. The network input API adds bounded ANSI text,
drag, wheel and left/right/middle click. Boot protocol and sound are not
exposed. Numeric comparison, passkey entry and repeat pairing are rejected.
Just Works pairing supplies no numeric-comparison authentication.

## Internal ownership and state

[passport_ble.h](main/passport_ble.h) is the UI-to-owner boundary.
`passport_ble_request` copies a command into an eight-slot queue. `true` means
queue admission only; the next snapshot exposes the owner's current result.
There is no command receipt ID or durable result history. This interface is
not a network API and must not be exported as one without the plan's separate
session, owner, generation and result contract.

The NimBLE task is the sole mutator of BLE, the bond store and `hid_engine`.
It services the engine every 20 ms, with at most four command admissions and
one report submission per service. Button callbacks only queue timestamped
events; the main task owns the UI. Queue loss or a button event older than
250 ms requests an emergency lock. Local input has a maximum three-second
lease. Network input uses the same engine and shared remote owner, with a
maximum three-second lease. A first physical takeover gesture suppresses
ordinary input; the next complete press may produce local input. While awake,
long DOWN still locks during takeover. Queued local
commands also carry `input_revision` so a held direction cannot cross takeover.

The snapshot separately exposes synchronization, storage availability,
connection, encryption, bonded state, each input subscription, lock,
advertising, pairing, release uncertainty and input readiness. Readiness
also requires exact selected identity, a nonsuspended transport and a
completed two-report release barrier. Input submission cannot bypass the
shared engine's owner, epoch, mode, lock or release gates.

Each connection/security invalidation changes `generation`; input commands
created against an older value are rejected. Saved-host commands carry the
binary identity address and `bond_revision`. Advertise/Forget recheck the
revision and bond after disconnect, before mutation. Disconnect is scoped
to the selected active host. Pair-window cancellation is a separate command.
Forget defaults to Cancel in the UI and never deletes other bonds to make
space.

A successful report result means synchronous NimBLE submission, not a
controller acknowledgement or observed host action. Host changes cancel the
engine and wait for both neutral reports before disconnect, bounded to
300 ms. If reports cannot be submitted, the state records release unknown.
A send failure cancels remaining input and retries neutral reports through
the shared engine's bounded budget. Exhaustion closes the connection.
Unexpected link loss also marks the previous release unknown. That historical
flag remains available on Device status until reboot; a new connection must establish its own
neutral barrier before input is ready.

## Local presentation and repeat timing

The main task owns the UI view and active theme. LVGL widgets are updated
under the BSP lock; a failed update is retried without acting on an unapplied
view. Dark/Light selection writes `passport_ui/theme` outside that lock.
Only successful NVS set/commit changes the active theme. Failure is visible
in Appearance and does not claim that persistence succeeded.

A Mouse direction hold starts at the BSP 500 ms long event and sends one
12-unit command at most every 100 ms, until two seconds after the original
press. There is no catch-up burst. Release, page exit, lock, changed owner
connection generation, unavailable input, stale buttons or queue loss cancel
repeat; no release is needed for the two-second deadline to stop it. A new
press after release is required for another hold. The unchanged BLE owner
still applies its admission and lease checks to every repeated command.

[passport_wifi.h](main/passport_wifi.h) is the local UI/owner boundary. A
four-entry copied command queue and sixteen-entry event queue keep SDK event
callbacks bounded. The Wi-Fi task owns scanning, transitions, pending
credentials and profile persistence. Queue admission is not connection or
save success. Event loss makes network state unavailable and requires restart.
Raw SSID bytes and stable profile IDs remain separate from presentation.

A switch first requests the BLE owner's existing Cancel operation and waits
for its published cancellation revision and cleared release-pending flag,
or a disconnected/invalidated link. A 500 ms deadline rejects an unconfirmed
release instead of switching. This adds two observations to the Passport
snapshot without changing shared engine behavior. Wi-Fi stop events delimit
network generations; credential joining can interrupt the AP browser and
requires reopening at the newly displayed address.

The installed embedded credential provider and its owner protocol
uses a 120-second physically opened window, an unpredictable token, one
immutable pending request and an exact local request-ID/window confirmation.
SSID/BSSID/security are copied from the selected scan generation; passwords
are never published in snapshots. Lock, timeout and network changes clear
pending secrets. A confirmed candidate is saved only after association and
an assigned live IP; uncertain NVS writes block further mutations.
Its six routes are registered before setup becomes available; registration
failure stops the listener and leaves provisioning unavailable.

## AP setup HTTP boundary

| Route | Behavior |
| --- | --- |
| `GET /setup`, `/setup.css`, `/setup.js` | Embedded dual-theme page and assets; no external dependencies |
| `GET /api/setup/state` | Current window, scan rows, non-secret status and request receipt; token only while open |
| `POST /api/setup/scan` | Queue a scan for the exact physical setup session |
| `POST /api/setup/submit` | Queue credentials for one exact scan row; never confirms or saves remotely |

Every setup route requires the socket's local IPv4 address to equal the
observed AP address, AP start plus DHCP, and a confirmed unassociated STA.
Host must be that numeric AP address (optionally `:80`); an Origin, when
present, must be its exact HTTP origin. This also rejects ambiguous ingress
when LAN and AP address spaces overlap. No CORS access is enabled.

POST uses `Content-Type: application/json` and `X-Passport-Setup` containing
the current 32-character window token. Scan accepts only `{"setupId":17}`.
Submit accepts only `{"setupId":17,"scanRevision":4,"row":0,"password":"example123"}`;
these values are illustrative, not device credentials. Integer identities,
unique keys, a flat object, a maximum 512-byte body and ASCII passwords are
validated. WPA2 requires 8–63 characters; an open network requires an empty
password. Unicode escapes, embedded NUL, nested values, extra keys and
unsupported networks are rejected. The owner repeats window/token/scan
validation after queueing; the HTTP handler cannot change Wi-Fi directly.

HTTP 202 returns `queued` and a generated `requestId`. This confirms queue
admission only. State exposes `requestId` with `requestState` (0 none, 1 local
confirmation pending, 2 joining, 3 connected and saved, 4 failed or uncertain,
5 cancelled), plus `requestResult`. `lastSubmissionId` and
`submissionAccepted` distinguish owner rejection from queue admission.
The page claims success only for its matching request ID and saved outcome;
AP loss during joining instead directs the user to the native result and IP.
Receipts are in-boot observations, not durable or cross-device identifiers.

Raw SSIDs are hex-encoded in the JSON. The browser displays decoded names as
literal text and provides the complete raw identity; labels cannot replace
row identity. No AP/router password is included in state. The token stays in
page memory, never a URL or browser storage. Responses are `no-store` and use
a same-origin content security policy with framing disabled. The single
4096-byte HTTP task serializes three client sockets with LRU idle-session
purging; receive/send waits are two seconds. Body reading additionally has a
three-second deadline checked between reads (one read can extend that by its
socket timeout). Parsing depth and static response capacity are bounded.
The independent BLE and Wi-Fi tasks continue during HTTP waits. These limits
are engineering bounds, not measured coexistence or runtime stack margins.

## Stable identities and future labels

Saved rows come from the persisted bond store. A public identity or resolved
static-random identity is displayed as `AA:BB:CC:DD:EE:FF/type`; a rotating
resolvable private address is never accepted as a saved identity. An unbonded
connection shows `Identity pending` until the SDK supplies the bonded identity.
Controller whitelist filtering limits a saved-host advertisement, and the
connection callback rechecks the resolved identity against the selected bond.
Failure to configure filtering does not fall back to unrestricted advertising.

`bond_revision` is an in-boot list revision, not an enduring bond incarnation
or cross-device ID. The current UI has no user labels, peer-name persistence,
label edit API, opaque bond fingerprint or cross-device label import. A later
label implementation must create a stable opaque bond fingerprint tied to the
persisted bond incarnation, expose it with a fresh revision, and key local/web
labels and reconnect/forget requests by that fingerprint. It must not derive
identity from display text, list position, advertised name or an RPA. Forget
and re-pair must create a new incarnation instead of reusing an old label's
operation authority.

## Persistence boundary

The `passport_bonds/records` NVS blob holds a versioned snapshot of the pinned
NimBLE RAM-store ABI, including security, subscriptions, peer address mapping,
local IRK and client supported features. A format/SDK ABI change needs a
migration; this implementation fails closed on an unknown format. Individual
forget performs the SDK unpair operation and writes one final snapshot.
Restore/write/commit/partial-delete errors make the store unavailable and
block input. NVS is never erased automatically. Restart and inspection are
required after uncertain persistence; no successful forget is reported then.

Host tests substitute the SDK store and transport boundaries. Actual NVS
power-loss behavior, controller resolution of changing RPAs, reconnect across
reboots, host OS subscriptions, hardware rendering, radio coexistence and
runtime heap/stack margins require physical qualification.

## Direct HTTP and WebSocket input

This target explicitly initializes `CP_POLICY_PASSPORT_OPEN_HTTP`. The ordinary
`cp_initialize` still selects enrolled TLS. No request can change policy. The
current Passport provider implements only the exact input operations in the
[compatibility plan](../docs/passport-compatibility-plan.md), with `script` as
its sole input producer. Connection-management and label providers remain
unavailable until their separate integration. Operation metadata reports this
availability; broader scope names do not grant additional operations.

Use the observed numeric LAN/AP address, optionally with `:80`, as Host. A
browser's Origin must be exactly `http://` plus that Host; no CORS is enabled.
POST/DELETE require `application/json` (optionally `; charset=utf-8`). Bodies
are bounded to 2048 bytes. Wi-Fi passwords, join/forget and bond deletion are
never accepted by this API; provisioning keeps its separate AP/local-window
boundary. Lock rejects all remote mutations, including input cancellation
(the local Lock already releases input), while state/result reads remain usable.

1. Send `POST /api/v1/open-session`, `X-Passport-Open: 1`, and body `{}`.
   The response contains `session_token`, `session_id`, `boot_id`,
   `request_prefix`, `csrf` and `next_seq`. No cookie is used. Keep browser
   credentials in memory; do not put them in URLs or persistent browser storage.
2. Send `Authorization: Bearer <session_token>` on subsequent HTTP requests.
   Browser mutations also send the returned `X-CSRF-Token`. Native agents omit
   Origin. Read `/api/v1/state` before acquire/renew and use its current
   protocol state, including the server monotonic `now_ms`.
3. Submit the shared v1 envelope to `POST /api/v1/commands`: `v`, `boot_id`,
   `session_id`, monotonic `seq`, `request_id` (the 16-hex-character prefix plus
   the sequence as 16 hexadecimal digits), `deadline_ms`, `op`, `args`,
   `epoch`, `transport_generation`, `target_id`, and where required
   `ui_revision` and `lease_id`. Integer state/deadline values use decimal
   strings. Movement/scroll deadlines are at most 250 ms ahead; other
   nonsafety requests are at most 2000 ms ahead. Acquire args contain the
   freshly read `state_sequence`, optionally `lifetime_ms` (up to 3000) and
   `producer: "script"`. Wait for the neutral barrier and active lease before
   input. Renew requires a new observed `state_sequence`.
4. Inspect the result and poll `GET /api/v1/requests/<command_key>` for queued
   work. `accepted` is admission, and `sent` means NimBLE accepted reports,
   never observed host delivery. Text reports `started_prefix`,
   `completed_pairs` and `uncertain_index`; an interrupted key pair is not
   silently reported as completed. An exact retry preserves the original
   envelope and sequence. Do not issue a new action to resolve an unknown
   outcome. Results are bounded to sixteen records; eviction is explicitly
   `result_unknown`.
5. For browser WS, obtain `POST /api/v1/ws-ticket` with an empty body and the
   same Bearer/CSRF headers, then open `/api/v1/ws` with subprotocols
   `["passport.v1", ticket]`. The single-use ticket expires after ten seconds;
   only `passport.v1` is echoed. Authentication runs before the SDK sends its
   upgrade response. Native WS uses `passport.v1` and the Bearer header without
   Origin. WS accepts complete text frames up to 2048 bytes; binary and
   fragmented messages close the channel. Frames carry the same command
   envelope, `{"v":1,"resource":"/api/v1/state"}`, or
   `{"v":1,"type":"heartbeat"}`. Losing WS releases its input owner while
   preserving independent HTTP read access. A new channel never resumes input.
6. `DELETE /api/v1/sessions/current` closes the session and releases its input.
   Open sessions expire after sixty seconds without authenticated activity or
   thirty minutes absolute. Two slots are available; opening a third returns
   busy and never evicts an active controller. HTTP close alone does not end
   a logical session; the input lease still expires within three seconds.

`GET /api/v1/capabilities` identifies `access_policy: "open_network"` and
resource bounds. `/api/v1/operations?cursor=0&limit=8` pages the common catalog
and current provider availability. All transport requests are serialized onto
the existing NimBLE owner. An HTTP owner timeout reports an unknown outcome;
its mailbox remains unavailable until that owner completes or discards it.
Runtime free heap, stack high-water marks, radio coexistence, network timing
and host input behavior still require physical qualification.
