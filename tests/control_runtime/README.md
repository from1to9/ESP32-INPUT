# Control runtime boundary tests

Run `python3 tests/control_runtime/run.py`, or select `--case trust`, `update`, `clock`, `server`, `work` or `stack`. Strict C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer apply. The actual production adapters are compiled with explicitly named SDK/NVS/crypto/HTTP/clock/owner substitutes. The substitutes do not establish real TLS/PSA cryptography, flash power-loss behavior or device compatibility.

- Trust tests exercise local-window/token/digest approval, exact proposal binding, expiry/single use, two-bank uncertainty/recovery, known installed digest, grants and public disclosure of changed listener/clock/update authority.
- Update tests exercise actual layout/running-image metadata, approved HTTPS source/DNS/name/CA configuration, signed-catalog verification boundary, inactive-slot stream/hash/readback, cancellation, uncertain boot selection and health probation. Publisher signing uses real cryptography separately in the companion suite.
- Clock tests require an actual synchronization callback from the SDK boundary and cover unknown boot/date, stale/regressed time, disconnect, late callbacks and reconfiguration.
- Server tests compile the actual WSS/binary include with the actual portable protocol. They cover authentication before 101, CSRF-issued single-use/expiry tickets, scoped input loss, same-session replacement and stale channel generations, copied bounded PCM queues, owner-thread socket sends, queue/socket failure, exact live-channel loss with zero queued PCM, binary Range/If-Match and post-read authority/deadline checks. HTTP response close is independent of logical logout.
- Work tests compile exact production mailbox/admission/safety/state-publication function sections. Only native work, current identity, clock, completion and hardware snapshots are substituted. They inject sequential and identity-lookup interleaved lock/session/generation loss before the atomic native-admission claim, verify no effects or retained secrets, preserve input-only loss independence, and report already-started manager effects truthfully. Extraction fails if the production function boundaries change.

Network allocation regressions use a heap model that returns generic allocations
at a four-byte boundary that does not satisfy eight-byte alignment. Explicit
aligned requests honor the requested type/opaque-storage contract. Trust,
update, server/media and runtime tests exercise real typed object access,
capability fallback, rejected allocation, parser failure and cleanup. Work tests
also compile the exact runtime initialization function and native profile reorder
branch, substituting only its Wi-Fi mutation port. Sanitizer findings stop the
test process. The separate protocol suite rejects misaligned or undersized
caller storage before modifying it.

The related [Wi-Fi suite](../wifi/README.md) tests the actual native manager and local/remote ticket ownership. The [coordinator suite](../coordinator/README.md) exercises the actual update binding's neutral/transport/cleanup/continuous-health gates. The pinned target object check is documented in [the protocol suite](../control_protocol/README.md). Full firmware linkage and independent source review remain separate evidence; no test here opens USB/serial, pairs, flashes, downloads to a device or changes the live Keychain.


Selective network stack coverage compiles the actual clock and media adapter,
and exact server initialization/worker/start functions, with the real protocol.
The matrix covers PSRAM plus external-stack support, no PSRAM, and PSRAM without
external-stack support. Supported builds request PSRAM-only stack caps at the
unchanged 4-KiB clock/server, 12-KiB HTTPS and 8-KiB download capacities. Creation
failure never retries with internal caps. Tests preserve clock failure/retry and
reconfiguration, download admission cleanup, server startup failure, retained
resources during a failed stop, secret clearing and matching WithCaps deletion.
Listener/task ports are named substitutes; they do not execute SDK cleanup tasks.

The current SDK WithCaps self-delete path allocates an internal cleanup task
with a 1536-byte stack plus internal TCB. One server/HTTPS/download shutdown wave
can therefore require 4608 cleanup-stack bytes plus three TCBs. This is a planning
allowance, not a global bound or reservation: new admission can precede earlier
cleanup completion. The SDK aborts if cleanup-task creation fails. Teardown
pressure and scheduling overlap remain runtime qualification requirements. Main,
native Wi-Fi/NVS, management runtime and OTA task stacks remain internal.
