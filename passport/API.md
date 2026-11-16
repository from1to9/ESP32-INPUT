# Passport local HID batch interface

This file describes the implemented subset, not the full
[compatibility plan](../docs/passport-compatibility-plan.md).

## External interfaces

| Interface | Available behavior |
| --- | --- |
| BLE HID | Report-only keyboard input (ID 1, eight bytes), keyboard LED output (ID 1, one byte), relative mouse input (ID 2, four bytes), report map, HID information and control point |
| BLE management | One connection, three saved bonds, explicit 60-second pairing or selected-bond advertising, disconnect, individual forget |
| Local input | 12-unit directional steps, bounded direction repeats, left click, Esc/Enter/Tab/Backspace/arrows; all through the shared engine |
| HTTP | `GET /health` only; bootstrap diagnostics, no credentials or HID readiness assertion |

The BLE report payloads exclude the report ID; characteristics and Report
Reference descriptors select it. Both input characteristics must be subscribed
and the connection encrypted and bonded before the initial neutral keyboard
and mouse reports can complete. Boot protocol, text entry, drag, wheel,
right/middle click, sound, and remote input are not
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
lease. No remote lease or takeover path exists in this batch.

The snapshot separately exposes synchronization, storage availability,
connection, encryption, bonded state, each input subscription, lock,
advertising, pairing, release uncertainty and input readiness. Readiness
also requires exact selected identity, a nonsuspended transport and a
completed two-report release barrier. Input submission cannot bypass the
shared engine's local owner, epoch, mode, lock or release gates.

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

Wi-Fi cards expose startup observations only and mark live state unknown.
They do not imply a usable station, current AP association or DHCP service.
The Web interface remains the read-only health route; browser input and
matching browser themes belong to a later batch.

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
