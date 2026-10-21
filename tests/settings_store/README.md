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

Cases check missing-record defaults without automatic writes; successful loads
and unchanged requests; preserved invalid lengths, version, magic and CRC; startup/open/read/task
failures; invalid request rejection; the 999999/1000000 microsecond quiet boundary;
latest-value coalescing; A-to-B-to-A deduplication; requests arriving during
successful/failed commit; saved versus requested revisions; explicit retry after
set/commit failure without periodic retry; pending saves across clock regression;
snapshot copies; revision exhaustion; and simulated restart against durable
storage. A thousand changes every 10 ms produce one quiet-period save, followed
by a thousand unchanged requests with no additional writes. Corruption tests
use the production codec, retain the original bytes and require an explicit
request before replacing the damaged record.

The 16 cases include schema-2 Bluetooth/USB preference round trips and invalid
transport rejection. A frozen schema-1 record loads with Bluetooth selected and
remains byte-for-byte unchanged through worker polls; an explicit USB change
writes schema 2 after the quiet interval and survives simulated restart. This
proves store migration/durability decisions, not the coordinator startup path.

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
