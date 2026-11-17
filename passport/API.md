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
drag, wheel and left/right/middle click. Boot protocol and network sound controls are not exposed. Numeric comparison, passkey entry and repeat pairing are rejected.
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

## Stable identities and display labels

Saved rows come from the persisted bond store. A public identity or resolved
static-random identity is displayed as `AA:BB:CC:DD:EE:FF/type`; a rotating
resolvable private address is never accepted as a saved identity. An unbonded
connection shows `Identity pending` until the SDK supplies the bonded identity.
Controller whitelist filtering limits a saved-host advertisement, and the
connection callback rechecks the resolved identity against the selected bond.
Failure to configure filtering does not fall back to unrestricted advertising.

`bond_revision` is the in-boot host list revision. `passport_peer_t.bond_id`
contains a separate device-local 128-bit fingerprint. It is a truncated
HMAC-SHA256 of the stable peer identity and immutable local pairing material
stored by the pinned NimBLE SDK. The per-device secret and pairing bytes
never leave the firmware. A repeated pairing gets new pairing material,
while reconnect and reboot preserve the fingerprint of an existing bond.
An all-zero fingerprint means unavailable. `peer.label` and `current` still
carry the original formatted identity.

The native owner interface is in `passport_names.h`. The Web/API
adapter takes `passport_names_catalog` for each domain, enumerate
`passport_names_entry` with its revision, and use `passport_names_lookup`
to resolve a typed key. Match the active BLE/profile snapshots when
presenting live rows. Saved Wi-Fi labels use the stable profile ID.
`passport_wifi_ap_name_key` builds an exact scan key from the SDK-reported
SSID bytes, BSSID, and raw auth type. `passport_wifi_scan_label` tries that
key first, then the label of a unique saved profile with matching SSID and
compatible security. Retained AP labels are listed by the catalog even
without a matching current scan. The Wi-Fi SDK exposes a terminated SSID
array without a separate on-air length.

The caller must validate decoded label bytes with `passport_names_text_valid`
using the original length and perform the HTTP operation's admission checks.
Submit a set/clear change with `passport_names_submit` using the catalog
revision. Setting a scanned AP label also takes the current completed
`scan_revision` and an exact unsaved key. A retained AP label can be cleared
without a scan. `PN_OK` from submit means accepted for owner processing.
Poll `passport_names_result` with the returned ticket for the durable outcome.
Each domain allows one pending change and retains its latest result. If a
ticket was superseded, read the current catalog to reconcile. The BLE and
Wi-Fi owners revalidate identity, Lock, and storage before applying a change.
The catalog reports storage uncertainty and rejects additional writes after
an unconfirmed write. The backend owns storage; the protocol adapter below exposes its results.

The separate `passport_names` NVS namespace stores the HID and Wi-Fi label
catalogs. Up to three HID bonds, four profiles and sixteen exact AP keys can
have labels. Committed forget removes only the attached bond or profile
label. Boot reconciliation removes orphan saved-identity labels after the
bond/profile store is known. An AP label stays with its original key. A
label failure is reported independently of the bond or profile operation.

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
Passport provider implements the exact input, BLE management, Wi-Fi scan and
display-label operations in the [compatibility plan](../docs/passport-compatibility-plan.md),
with `script` as its sole input producer. Operation metadata reports provider
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


## Board-hosted controller and management

`GET /`, `/controller.css` and `/controller.js` serve the embedded controller
on the same observed LAN/AP listener and exact Host/Origin boundary. Assets
need no session, external fonts, internet connection or PC relay. The server
reserves fourteen route slots: health, six setup routes, four API/WS routes,
and three controller assets. Registration failure stops the listener and
keeps the existing explicit unavailable state.

The browser uses the open bearer session and the common HTTP command/result
flow. Tokens stay in memory. Mouse, Keyboard and Connections are separate
views; theme selection works before login, follows the system by default,
and stores only an explicit browser preference. It does not change native
Appearance. Capability discovery disables unavailable controls. Ctrl/Command
shortcuts send `keyboard.key_tap` with modifiers 1/8 and usage 6 (copy), 25
(paste) or 29 (undo). Agent `keyboard.shortcut` retains the Passport Ctrl
semantics; agents targeting Apple hosts should send the explicit GUI modifier.

Movement samples expire after 150 ms locally and use protocol deadlines of
at most 200 ms. One browser command runs at a time, with no action replay.
Drag holds use a 1500 ms device deadman refreshed while the contact remains
active. Release ends the drag; cancellation while a command is in flight,
Stop during busy/unknown work, blur, page hide and offline close the session
best effort and discard local samples. Idle Stop releases control. The
three-second input lease remains the final boundary when the browser cannot
reach the board. A target/readiness change or lease loss requires explicit
control acquisition; uncertain replies require explicit reconnect and target
inspection. These are source and host-tested boundaries, not measured network
or host delivery guarantees.

Management uses the same v1 envelope, `management_generation` from state,
and `resource_revision` from the relevant list. No input lease is required.
The protocol remains the owner of duplicate requests and retained results.
Passport retains four protocol results, allows two active requests, and uses
two remote-input jobs with four recent owner results. `/api/v1/capabilities`
reports the result capacity. Exhaustion and eviction preserve the existing
busy and result-unknown outcomes; clients must consume results promptly.

| Read | Identity and revision |
| --- | --- |
| `/api/v1/bluetooth/peers` | `revision` is the bond-list revision; `generation` is the observed link generation. Up to three rows preserve `original`, optional `label`, `label_known`, 32-hex `id`, and `current`. Connection, encryption, subscriptions, readiness, pairing, handshake, selected ID and transition are separate fields. |
| `/api/v1/wifi/profiles` | `revision` is the profile-store revision. Up to four rows have an eight-hex stable profile `id`, raw `ssid_hex`, security, current marker and optional label. |
| `/api/v1/wifi/scans/current?cursor=0` | Four rows per page; `next_cursor: 0` ends paging. `generation`, `known`, `scanning` and `truncated` describe the scan. Rows preserve SSID hex, BSSID, raw auth, security, RSSI and index. A 64-hex `id` is SHA-256 of length, 16-bit big-endian auth, BSSID and exact SSID bytes, matching the shared AP identity encoding. A labelable row has no matching saved profile. |
| `/api/v1/display-names?cursor=0` | Eight entries per page, including absent APs. `revision` combines BLE and Wi-Fi catalog revisions as `ble * 4294967296 + wifi`, represented as a decimal string. Counts/capacities do not turn `known: false` into an empty-store success. |

`bluetooth.target.request` and `bluetooth.disconnect` require `peer_id` to be
the saved bond's 32-hex ID and `generation` to match the peers response, in
addition to the envelope's bond-list `resource_revision`. Pairing open,
close and cancel-handshake also require those current generation/revision
values. Open and target requests accept `window_seconds` from 1 to 300,
default 60. Target completion requires the exact selected host to become
input-ready; queue admission and advertising are intermediate observations.
Timeout, wrong-host rejection, local cancellation, SDK failure and uncertain
neutral release remain non-success results. Disconnect retains the bond.
Cancel-handshake retains the original pairing deadline; close ends admission.
One BLE management command is outstanding at a time. The native transition
path releases input before disconnecting or advertising.

`wifi.scan.start` and `wifi.scan.cancel` use the scan `generation` as
`resource_revision`. The Wi-Fi owner rechecks revision, Lock and availability
and publishes a command-specific receipt. `scan_started` means SDK scan
admission, not a completed result list; read `known` and `scanning` afterwards.
The existing six-second scan bound, sixteen retained rows and cancellation
drain remain in force. The adapter tracks one scan request at a time.

`display_name.set` takes `kind` (`hid_bond`, `wifi_profile`, or `wifi_ap`),
the ID above, `label` and the catalog `resource_revision`. Saved identities
also require `owner_revision` from their owner list. Exact APs require the
current integer `scan_generation` and `scan_index`; the adapter recomputes
and matches the complete key before submission. Clear takes kind, ID and
catalog revision, including for a retained AP absent from the latest scan.
Classic bonds are unavailable on C3. Two label jobs are bounded to one per
storage domain. Accepted writes are polled through the owners' tickets;
only confirmed persistence yields `applied`. Stale identity, Lock, capacity,
unsupported font glyphs, storage uncertainty and a lost result remain distinct
failures. No font set is expanded by this adapter.

Bond deletion and Wi-Fi credentials/join/forget remain outside the open
allowlist. The Connections view points to the native Forget/Connect and
physical AP setup flows. It never substitutes a label, row index or display
name for the original identity, and renders received text with `textContent`.
