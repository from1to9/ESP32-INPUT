# Production settings-store regressions

Run `python3 tests/settings_store/run.py` from the repository root. A named case
can be isolated with `--case quiet_deadline_and_latest_value_coalescing`; the
default runs every case. The runner uses temporary outputs, strict C11 and
conversion/shadow diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer.

The harness includes unchanged production `firmware/main/settings_store.c` and
links the real `device_settings` codec/policy. Only NVS, task creation/delay,
critical sections and monotonic time are substituted. It executes one actual
worker iteration up to its delay boundary; it does not reproduce the store's
admission, coalescing or revision decisions. All storage and clock operations
assert that they occur outside critical sections. No device, network, actual
NVS partition, firmware build or serial port is accessed.

Cases check the 50% missing-record brightness fallback without automatic writes;
successful loads retaining chosen 10% and 70% brightness with no rewrite for
unchanged requests; preserved invalid lengths, version, magic and CRC; startup/open/read/task
failures; invalid request rejection; the 999999/1000000 microsecond quiet boundary;
latest-value coalescing; A-to-B-to-A deduplication; requests arriving during
successful/failed commit; saved versus requested revisions; explicit retry after
set/commit failure without periodic retry; pending saves across clock regression;
snapshot copies; revision exhaustion; and simulated restart against durable
storage. A thousand changes every 10 ms produce one quiet-period save, followed
by a thousand unchanged requests with no additional writes. Corruption tests
use the production codec, retain the original bytes and require an explicit
request before replacing the damaged record.

The 17 cases include schema-3 Bluetooth/USB, tap-to-click and Bluetooth-name
round trips and invalid preference rejection. Frozen 16-byte schema-1/2 records
retain every old field (including 70% or custom brightness), default only new
tap/name values, and remain byte-for-byte unchanged through worker polls and
unchanged requests. An explicit changed preference writes the 56-byte schema 4
after the quiet interval and survives simulated restart. The store caches the
validated logical value canonically for deduplication; this memory-only cache
is not evidence that the on-flash record was upgraded. These cases prove store
migration/durability decisions, not coordinator startup or BLE name application.

The storage double supports both an unchanged durable record on failure and an
uncertain commit result where the new value is already durable. A failure must
not be treated as rollback: the pinned SDK's simple NVS handle currently writes
before `nvs_commit`, whose implementation is a no-op. Consequently a retry back
to the previously saved value must not deduplicate against a stale cache. This
fault model does not prove physical power-loss atomicity or flash endurance.

Requests injected inside the NVS commit boundary provide deterministic
interleavings, not a FreeRTOS race/scheduling proof. The caller must distinguish
accepted/pending from saved; `loaded` describes startup load evidence. General
setting coalescing does not weaken BLE subscription commit-before-success rules.
The integration owner maintains `tools/check.py` and fixes production defects;
this suite retains failures until the owning implementation is corrected.

Schema-3 tap-off migration is additionally checked with a frozen independent CRC fixture: equal requests and changed-then-restored values preserve the original 40-byte record without a boot or format-only write. Explicit motion changes persist schema 4 and round-trip across restart.
