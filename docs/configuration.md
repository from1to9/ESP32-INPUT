# Shared configuration contract

Status: planning, 2026-10-21. This extends the current six-field preference
record; it does not claim new runtime settings or network endpoints exist.
Complete the API/function inventory and this schema before implementing new
interfaces. Current executable defaults remain authoritative until migration.

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

The following values describe the current baseline where known. Additional
choices remain proposals until their owning algorithm/driver and tests exist.

| Group | Fields and current values | Apply and persistence plan |
| --- | --- | --- |
| Display | Theme Dark/Light, default Dark; brightness 10..100 percent in steps of 10, default 50; timeout Off/1/5/10/30 minutes, default 5 | Existing durable preferences; brightness requires driver acknowledgement; timeout starts a new interval |
| Input routing | Preferred Bluetooth/USB, default Bluetooth; cold boot Mouse with motion off | Transport preference saves only after safe switch; current link/readiness and motion-on are volatile |
| Keyboard | Ctrl/Command, default Ctrl; US ANSI layout; nine-cell groups; compiled preview capacity 64 | Modifier is durable today; a proposed effective preview limit may be 1..64 with default 64, distinct from the immutable compiled capacity. Layout/group profiles require capability/schema validation and release-before-apply; reducing the limit below the current draft rejects without truncation. Draft/send progress stays volatile |
| Pointer | Gain currently 4 HID units/degree; deadzone 0.5 degrees/second; filter constant 0.03 seconds; board axis transform | Proposed validated profiles; clear fractional/filter state and release/cancel before apply; no remote raw axis-register writes |
| Selection | Cell angle currently 6 degrees; hysteresis 1 degree; valid-cell layout and center rule | Proposed profile; require valid group membership and hysteresis strictly smaller than half the cell spacing; cancel pending confirmation before changing |
| Touch | Gain divisors currently 1, 4, 8; gesture/scroll behavior | Profile-supported choices; complete or cancel the active gesture before apply; never reinterpret an in-flight gesture |
| Buttons | Current AI single/double/hold semantics; 280 ms double-click window and 900 ms hold | Proposed allowlisted semantic mappings and timing profiles; shared wake/gesture timing; physical recovery/unlock must remain reachable |
| Haptics | Global enabled preference, default true; current bounded confirmation/mode/lock/error patterns | Existing enable is durable; proposed strength/duration/pattern profiles stay within board-qualified motor limits; off stops pending output immediately |
| Connections | Saved Wi-Fi profiles/order, BLE target preference/order, automatic reconnect policy, setup/pairing windows | New schema planned; stable object IDs/revisions, bounded counts/backoff; preserve current healthy link unless explicitly switching |
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

Expose schema and current effective/desired/saved state through read operations.
Use one bounded `config.stage` / `config.apply` / `config.cancel` transaction
family in the API operation registry, not a second path that bypasses ownership.
Staging is inert; it validates the full candidate against expected revision and
current capabilities. It does not save, stop input or change a connection.

Applying rechecks authorization, revision, capability and conflicts. Acquire
any required release/reconnect/maintenance barrier, then apply through the
manager. Report `accepted`, `applying`, `applied`, `saving`, `saved`, `failed`,
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

## Validation gate

Before implementation, reconcile names and types with the complete API function
matrix. Each configurable function needs a manager owner, default/bounds,
support status, apply/persistence semantics and test references. Validate schema
examples against a machine-readable definition; verify UI/API/MCP derive the
same choices and reasons. No permissive generic set-any-key endpoint.

Test boundaries, dependency conflicts, concurrent revisions, active gestures,
held keys, reconnect/capture/update conflicts, save coalescing, unchanged writes,
storage uncertainty, corrupted/old/newer records, interrupted migration and
power loss. Test secret redaction in state/events/export/errors. Hardware tests
must verify actual apply/reboot behavior, motor/audio limits and long powered
operation. Passing schema validation alone is not a device capability result.
