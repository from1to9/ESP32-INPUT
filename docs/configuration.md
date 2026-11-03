# Shared configuration contract

Status: implemented schema-4 preference and typed configuration integration candidate, 2026-10-21. The production settings owner preserves valid legacy schemas and reports effective/requested/saved state separately. The [generated catalog](../api/control-v1.json) defines 77 typed metadata fields consumed by firmware/OpenAPI/MCP; individual manager availability, apply policy and persistence support remain explicit. Metadata does not make an unsupported field writable or qualify hardware. Existing durable data is never rewritten merely by loading it.

## One model, three clients

Device UI, authenticated Wi-Fi clients and the Mac MCP bridge consume the same
versioned configuration metadata. They submit typed desired values to the same
owning managers. No client writes peripheral registers, NVS keys or GPIOs.
Do not duplicate validation bounds, defaults or supported choices in UI code,
HTTP handlers and MCP tools. Compact generated firmware tables may be derived
from the checked-in schema; validate generated artifacts in CI.

Each field defines:

| Metadata | Meaning |
| --- | --- |
| `key`, `schema_version` | Stable namespaced identity; independent from API and firmware version |
| `type`, `unit`, `default` | Exact representation and documented default; no inferred unit |
| `minimum`, `maximum`, `step`, `enum` | Applicable constraints; reject rather than clamp silently |
| `capability`, `supported`, `reason` | Board/driver/profile dependencies and truthful availability |
| `scope`, `sensitive`, `readable` | Required authority; secrets are write-only and never exported |
| `apply_policy` | Immediate, release-required, reconnect-required or reboot-required |
| `persistence` | Volatile, explicit durable or installation profile; never implicit per-event writes |
| `dependencies`, `conflicts` | Cross-field validation and incompatible active operations |
| `label`, `help`, `group` | English product copy and UI grouping; no HTML or executable expressions |
| `effective`, `desired`, `default_source` | Actual applied value, requested value and provenance |
| `revision`, `applied_revision`, `storage_state` | Optimistic concurrency and separate persistence evidence |

Metadata itself is bounded and versioned. A device publishes only the fields it
understands, including inactive preferences with a reason. Unknown keys, enum
values, duplicate keys, nonfinite numbers, wrong types and oversized documents
are rejected. Missing fields in a patch mean unchanged, not reset to default.
An explicit reset names its field/group and follows the same validation path.

## Configuration inventory

The stored fields are theme, shortcut modifier, haptics enable, brightness,
idle timeout, preferred transport, tap-to-click, Bluetooth name, independent
pointer X/Y gains, deadzone, filter, freeze and X/Y inversion. Motion intent,
calibration, gravity/reference state, draft contents and active leases remain
volatile.

The current configuration transaction owner implements sixteen keys:
`display.theme`, `display.brightness_percent`, `display.idle_timeout_seconds`,
`haptics.enabled`, `keyboard.shortcut_modifier`, `input.preferred_transport`,
`touch.tap_to_click`, `bluetooth.device_name`, `pointer.gain_hid_per_degree`,
`pointer.deadzone_dps`, `pointer.filter_seconds`,
`pointer.gain_x_hid_per_degree`, `pointer.gain_y_hid_per_degree`,
`pointer.invert_x`, `pointer.invert_y` and `pointer.freeze_ms`. The scalar API
gain sets both axes. A candidate containing both scalar and per-axis gain is
rejected regardless of ordering. Each per-axis, inversion and freeze field can
be applied or reset independently through the same native preferences. When
axis gains differ, the configuration read reports the scalar as null and
includes the actual X/Y values. Registry entries alone do not make the
remaining fields writable; unsupported owner operations reject explicitly.

The following inventory includes implemented preferences and proposed profiles.
Additional choices need an owning algorithm/driver and validation before use.

| Group | Fields and current values | Apply and persistence plan |
| --- | --- | --- |
| Display | Theme Dark/Light, default Dark; brightness 10..100 percent in steps of 10, default 50; timeout Off/1/5/10/30 minutes, default 5 | Existing durable preferences; brightness requires driver acknowledgement; timeout starts a new interval |
| Input routing | Preferred Bluetooth/USB, default Bluetooth; cold boot Mouse with motion off | Transport preference saves only after safe switch; current link/readiness and motion-on are volatile |
| Keyboard | Ctrl/Command, default Ctrl; US ANSI layout; nine-cell groups; compiled preview capacity 64 | Modifier is durable today; a proposed effective preview limit may be 1..64 with default 64, distinct from the immutable compiled capacity. Layout/group profiles require capability/schema validation and release-before-apply; reducing the limit below the current draft rejects without truncation. Draft/send progress stays volatile |
| Pointer | Durable X/Y gains 0.25..100 HID units/degree in 0.01 steps, default 4 each; deadzone 0..10 degrees/second in 0.001 steps, default 0.5; filter/freeze 0..1000 ms, defaults 30/150; X/Y inversion default false | Implemented schema-4 preferences; all these native fields use the configuration API keys above. Cancel/release before motion policy changes and clear fractional/filter state; calibration and motion-on stay volatile. Board axis transforms remain port definitions |
| Selection | Cell angle currently 6 degrees; hysteresis 1 degree; valid-cell layout and center rule | Proposed profile; require valid group membership and hysteresis strictly smaller than half the cell spacing; cancel pending confirmation before changing |
| Touch | Gain divisors 1, 4, 8; durable `tap_to_click`, default true; portable tap/drag profile | Current defaults: tap 250 ms/8 px, second contact within 280 ms/16 px, contact timeout 1000 ms, drag ceiling 30000 ms. Valid bounds live in the portable header. Cancel/release before changing policy; Off keeps pointer motion and explicit buttons. Timing/profile API exposure remains planned |
| Bluetooth device name | Durable local UTF-8 name, default `ESP32-INPUT`, 1..18 bytes excluding NUL | Reject malformed UTF-8, C0/C1 controls and outer ASCII spaces. Persist a user change only after matching radio acknowledgement; keep effective, requested and saved status distinct. Preserve identity and bonds; host caches may retain an older name |
| Buttons | Current AI single/double/hold semantics; 280 ms double-click window and 900 ms hold | Proposed allowlisted semantic mappings and timing profiles; shared wake/gesture timing; physical recovery/unlock must remain reachable |
| Haptics | Global enabled preference, default true; current bounded confirmation/mode/lock/error patterns | Existing enable is durable; proposed strength/duration/pattern profiles stay within board-qualified motor limits; off stops pending output immediately |
| Connections | Saved Wi-Fi profiles/order, BLE target selection, setup/pairing windows | Separate typed connection operations and stores, rather than fields in the schema-4 preference record. Configuration transactions do not yet expose a general reconnect/order profile; preserve the current healthy link unless explicitly switching |
| Work policy | Canonical `work.policy` values `input_first` (default), `balanced`, `management_first`; only qualified values become writable | Bounded scan/retry/status/download budgets; idle-only admission is a separate job condition. No arbitrary RF percentage control or ability to demote key release/local stop |
| Audio | Route, volume, mic/speaker mute, sample profile, channel/mix/codec options | Driver-advertised choices only; separate saved volume from actual output/mute; no durable automatic capture/unmute |
| Recording | Format, sample profile, duration/byte ceilings, storage target, retention policy, streaming budget | New capability; bounded jobs, explicit authorization and indicator; file retention and storage writes are separate from settings writes |
| Remote access | Listener enable, enrolled principals/scopes, lease/deadman limits, observer/event limits | Separate privileged policy; credentials write-only; no remote self-enrollment or self-expansion of scope |
| Firmware update | Approved source policy, signing trust, channel/version policy, download budget, restart policy | Separate update authority and provisioning; no arbitrary bootloader/partition/eFuse mutation |
| Diagnostics | Log level, sample period, enabled counters, bounded capture | Rate-limited; secrets/typed text/audio excluded by default; diagnostic observations never alter control behavior |

Numerical bounds for new fields must come from algorithm, resource and board
qualification, and be specified before exposing that field. An unfinished field
is listed as planned, not offered as a writable fake control. A successful
schema write cannot manufacture a supported audio format, radio profile or
larger statically allocated buffer.

## Three kinds of values

1. **Runtime preferences** are user-facing tunables, validated and applied by
   their manager. They may be volatile or explicitly durable.
2. **Board/build profiles** describe pins, buses, supported chips, display and
   codec topology, fixed buffer ceilings, task resources and flash layout. Keep
   them in a separate port/profile registry derived from the BSP. Select only a
   positively identified compatible profile; another board does not inherit
   Mosaico qualification. Runtime preferences cannot overwrite this profile.
3. **Safety/protocol invariants** include release-before-switch, stale-generation
   rejection, bounded memory, physical unlock/recovery, no implicit text Enter,
   authenticated control and visible capture. They are documented constants or
   rules, not ordinary settings that a remote caller can disable.

Configuration replaces scattered policy literals, not every implementation
constant. Protocol report sizes and tested resource ceilings remain explicit.
Configurable requested limits must not exceed those ceilings.

## Transaction and durability

The implemented bounded `config.stage` / `config.apply` / `config.cancel` family
uses the API operation registry and common coordinator. Staging is inert; it
validates the full candidate against expected revision and current capabilities.
It does not save, stop input or change a connection. Applying rechecks mutable
gates and observes the owning driver and storage outcomes. Configuration reads
report current settings/revision; `durability: owner_status_required` means the
read itself is not a persistence receipt.

The staged `durable` option defaults to true. With `durable: false`, applying
changes effective runtime preferences without requesting a preference save.
With `durable: true`, the validated candidate is explicitly submitted to the
coalescing store; acceptance does not prove a commit. A same-effective-value
durable apply can save a previously temporary candidate. Native preference
edits merge only their changed fields into the separate durable baseline, so
an unrelated local edit cannot silently persist a remote temporary override.

The coordinator keeps effective preferences separate from its last admitted
durable intent. That baseline is not proof of storage: pending/error status and
the store's requested/saved revisions determine durable evidence. The UI can
report temporary preferences when effective values differ from that baseline.
After reboot, only a successfully stored record is restored; temporary changes
have no automatic restoration or background save.

Applying rechecks authorization, revision, capability and conflicts. Acquire
any required release/reconnect/maintenance barrier, then apply through the
manager. Keep `accepted`, `applying`, `applied`, `saving`, `saved`, `failed`,
`storage_uncertain` and `requires_restart` as distinct observations. A response
must identify which runtime values are effective and what durable evidence is
known. Group changes with different managers cannot claim atomic hardware
rollback unless the transaction actually verifies every affected manager.

Flash errors can occur after data has changed. Keep an independently known old
record as evidence, but do not assert it remains the current durable value after
an uncertain commit. Read-verify or perform an explicitly authorized recovery;
never erase the partition to make an error disappear. A failed apply preserves
or restores the last verified effective value only where the driver confirms it.

Coalesce changed preferences after a quiet interval and deduplicate identical
values. No automatic flash write on boot, reconnect, heartbeat, motion sample,
lease renewal or repeated same-value request. Preserve unknown/newer records
when migration is unsupported. Migration is versioned, tested and explicit;
secrets and unrelated namespaces survive. Cancellation cannot pretend to undo
a commit that has already begun. Factory-reset/forget operations are separately
scoped and cannot be hidden inside reset-to-default preferences.

`preferences.reset` supports `all`, `display`, `input`, `haptics` and `bluetooth`
groups through an immutable reviewed candidate. `device.factory_reset.request`
means **Reset preferences and connections**: device preferences, LE HID bonds,
Wi-Fi profiles and HID target preferences. Its required retention-manifest
digest identifies the displayed reset/retain scope. Control trust, Classic
bonds, firmware/OTA, models, recordings, unknown namespaces, external NAND and
factory backups remain preserved. Wi-Fi deletion runs last. No raw erase,
automatic restart, new pairing window or atomic rollback is implied. An absent
approval ID stages a local review; a supplied ID must match the original
identity, scope, action digest and revisions. Reset/restart confirmation requires
fresh physical input on that exact review, and cancellation remains available
when confirmation becomes stale or expired.

Input, all-preference and factory resets include the local Navigation preference,
restoring its default off value through the input ownership/release domain.
Display, haptic and Bluetooth-only preference resets preserve that choice.
This reset coverage does not add a remote Navigation patch key.

## Validation gate

Reconcile new fields and changes with the complete API function matrix and the
[canonical registry](../api/control-v1.json). Each configurable function needs a manager owner, default/bounds,
support status, apply/persistence semantics and test references. Validate schema
examples against a machine-readable definition; verify UI/API/MCP derive the
same choices and reasons. No permissive generic set-any-key endpoint.

Test boundaries, dependency conflicts, concurrent revisions, active gestures,
held keys, reconnect/capture/update conflicts, save coalescing, unchanged writes,
storage uncertainty, corrupted/old/newer records, interrupted migration and
power loss. Test secret redaction in state/events/export/errors. Hardware tests
must verify actual apply/reboot behavior, motor/audio limits and long powered
operation. Passing schema validation alone is not a device capability result.
