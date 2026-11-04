# BLE adapter regressions

Run `python3 tests/ble_hid/run.py` from the repository root. A native C11 compiler and AddressSanitizer/UndefinedBehaviorSanitizer are required; ESP-IDF and a device are not required.

The suite compiles production `ble_hid.c`, its internal `ble_hid_store.h` helper, the production portable HID engine and the shared device-settings validator. The only substitutes are Bluetooth/controller, NVS, PSA hashing, clock, task and queue boundaries in `stubs/`. The PSA substitute is a deterministic identity test double, not a SHA implementation. The target uses the SDK's actual PSA SHA-256 implementation. Native checks establish state behavior; the firmware build independently checks actual pinned SDK headers and libraries.

The production `ble_hid_store.c` preference serializer is also linked. Its
independent NVS namespace is tested for acknowledged commit/readback, reboot
replay and order, same-address key replacement, removed selected bonds, stale
revisions, copied inputs, admission during commit, all storage failure stages,
commit errors with/without persistence, malformed CRC and future versions.
No case treats missing/failed storage as an empty successful preference record.
Single-bond removal tests use the real worker/API and require exact fingerprint
and revision, one SDK address removal, preservation of unrelated bonds/CCCDs,
verified absence, pending cancellation, late/foreign completion isolation and
uncertainty after timeout/partial cleanup. The quiet forget-all variant never
opens pairing; an acknowledged empty preference record also prevents automatic
first-boot pairing behavior on the reset device's next boot.

## Advertising initialization

The structured Bluedroid advertising API in pinned `esp_gap_ble_api.c` accepts service UUID input in 128-bit entries, even for assigned 16-bit services. The adapter supplies the Bluetooth base UUID for HID 0x1812 in little-endian order; the SDK compresses it to a 16-bit service UUID on air. The advertising payload is 24 bytes including name, flags and appearance.

BTDM builds omit the BR/EDR-not-supported flag; BLE-only builds retain it.
The test asserts both configurations independently without changing name size.

The boundary substitute enforces the pinned API's `service_uuid_len % 16 == 0` contract. A production-initialization regression passes the old invalid two-byte length and requires `ESP_ERR_INVALID_ARG`, failed status and no app registration. A separate positive case checks all UUID bytes against an independent expected HID identity and drives registration, table creation, service start, advertisement configuration, scan-response configuration and advertising-start completion. Advertising cannot start before all prerequisites complete.

## Subscription persistence

GATT requires CCCDs to persist across bonded connections and to reset for non-bonded connections ([Core 6.2, section 3.3.3.3](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/host/generic-attribute-profile--gatt-.html)). The control worker owns one versioned `hid_cccd/subscriptions` NVS blob. It holds at most 32 records, each containing a 32-byte SHA-256 bond fingerprint and the two real notification bits. The maximum serialized size is 1060 bytes. No raw bond keys or addresses are logged or duplicated into this blob.

A fingerprint includes the stable SDK bond index/type, exported LTK, encryption identity fields, optional peer IRK/identity and HID schema version. A replacement LTK cannot inherit the old bond's subscriptions even when peer identity and IRK stay unchanged. Removed records can be reused only after a successful bond-list lookup proves their fingerprints absent; unavailable evidence is never an empty list. A full table with uncertain identities rejects the update.

This development adapter requires LE Secure Connections Just Works for bonded persistence. `ESP_LE_AUTH_REQ_SC_BOND` and `ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH` enforce the request; an existing legacy authentication is rejected without deleting its bond. In pinned SDK `383dda6c1`, `smp_save_secure_connections_long_term_key()` saves SC LTK as both PENC and LENC. `esp_ble_get_bond_device_list()` exports PENC. Peripheral legacy bonding can have only LENC, which this public API omits; the adapter does not guess a bond incarnation from IRK/address alone.

After authentication/encryption completion for the current connection, the worker matches the SDK-resolved peer to a current bond and restores only that fingerprint's actual saved bits. Encryption alone does not create subscriptions. Real writes received on the current connection supersede queued restoration. Readiness stays closed while any CCCD transaction is pending. Non-bonded subscriptions reset on every connection.

For a bonded CCCD Write Request, NVS commit precedes ATT success. Identical writes skip flash commits. Disable requests revoke input admission immediately. Writes that arrive before bond completion wait in a bounded queue for at most 3000 ms. NVS, hashing, identity or queue failures close the gate rather than silently acknowledging non-durable state. Queue exhaustion also faults on Write Commands, which cannot carry an ATT error response. A no-response disable can never be silently discarded while queued enables reopen readiness.

All storage and PSA work runs in a worker with a 6144-byte stack. BLE callbacks only validate, update admission state and post bounded jobs. A generation/authentication revision check prevents a stale commit, restore, or asynchronous failure from mutating a replacement link. A commit already in progress may persist the old peer's legitimate requested subscription, but cannot acknowledge an old transaction on the new connection.

## Notification lifetime and failures

One notification may be outstanding. Disconnect keeps reconnect advertising closed until the pinned SDK's notification completion drains. An independent timer checks deadlines every 20 ms. A missing completion after 1000 ms or subscription transaction after 3000 ms latches a fault even if the BLE control worker is stalled; the worker closes any active link. A late callback does not clear the fault or authorize connection ID reuse. Reboot is the explicit safe reset; bond storage is preserved.

## Pairing admission and saved hosts

The worker reads the actual SDK bond list before advertising. A successful zero-bond startup opens a 120-second pairing window once. Expiry never automatically reopens it. Explicit open requests accept 1 through 300000 ms and require a healthy, disconnected adapter with a known list below the configured SDK bond capacity and no rejection or forget operation pending. Opening a window adds admission without deleting or replacing existing bonds. The public cached list exposes that actual capacity (currently 15 in the target configuration, maximum 32); new-host admission stops at capacity so the SDK cannot evict an older bond to make room. Closing stops new admission; an already admitted pairing procedure has at most 30 seconds to complete. The coordinator can explicitly disconnect it sooner.

The regular link snapshot also exposes `bond_count_known`, `bond_count` and `bond_capacity` for a disabled Add device/full-capacity message. Capacity comes from the compiled SDK configuration even before the first bond read; an unavailable count must not be treated as zero. UI integration remains the coordinator's responsibility.

Saved peers may reconnect outside the window, including at capacity. An unknown peer outside it is rejected and never becomes the public active target. Its disconnect must complete within two seconds or the adapter faults. Rejection commands carry their own generation and address, so a queued rejection cannot close a replacement connection that reused its connection ID. The adapter has one active HID target even when several hosts are saved. Pairing-window controls never send HID reports; the coordinator owns release and transport selection. Bluetooth connection management continues independently of the coordinator's selected output transport. Only explicit send calls submit HID input reports.

Outside the pairing window the peripheral waits for host-initiated encryption with an existing LTK. It does not initiate a fresh SMP Security Request: pinned Bluedroid's `SMP_PAIR_FLAGS_WE_STARTED_DD` path can otherwise approve the following pairing request without an application security-request grant. During an admitted window, security replies are scoped to that connection and its 30-second deadline. Authentication consumes that admission, so later fresh security requests do not reuse it.

The repository's pinned SDK patch also preserves existing keys during bonded
re-encryption. In `383dda6c1`, `smp_link_encrypted()` routes a host-initiated
encryption completion through `smp_check_auth_req()`. Its upstream unconditional
key clear discards that bond before the next connection. The patch restricts
replacement to `SMP_PAIR_FLAG_ENC_AFTER_PAIR`, which the pairing request/response
path sets; successful fresh pairing still clears the old material before new
key distribution. Failed or unfinished pairing preserves the old keys.
Run `python3 tests/ble_hid/test_sdk_reconnect.py` after SDK setup. It compiles the
actual SMP state tables, event dispatcher, encryption actions and peripheral
LTK reply from that SDK, substituting record lookup, key clearing, HCI replies,
completion observation and unrelated actions. It covers restored encryption
in both roles, fresh SC/legacy replacement, failure and missing-key controls.
This is a focused native replay, not a full Bluetooth stack or radio test.

`ble_hid_bond_list()` returns a bounded cached value copy containing up to 32 actual SDK bond addresses and address types. The worker refreshes it after startup, bonded authentication and each forget step. A full-capacity read followed by a total-count check prevents an intervening addition from appearing as a truncated or empty list. The copy performs no Bluetooth or NVS access. It publishes availability, management-in-progress, error and completed-read/status revision. Unavailable results retain the last rows with `known=false`; they do not claim an empty list. Current-link flags can change without a read revision.

Every authentication completion immediately invalidates the durable bond cache and latches a worker refresh independently of active-link state. A successful new bonded authentication closes the pairing window in the callback. Disconnect cannot discard that refresh, and a late authentication event received after disconnect still invalidates the view. A mutation revision prevents a read overtaken by another authentication completion from publishing stale capacity or clearing the refresh latch. Stable read failure leaves the cached rows explicitly unavailable; it cannot authorize a new host. An existing host can trigger a later refresh through authentication.

A connection inside the window initially carries only pending pairing intent. Before initiating SMP or granting a security request, the worker performs a fresh bounded SDK bond read, rechecks the connection and authentication revision, and confirms a free configured slot. If the actual table is full despite a stale cached count, the physical link is closed without a new pairing grant or encryption request. A read failure also fails closed. This check protects existing bonds even when the SDK has changed storage before its completion reaches the application.

Only a successfully authenticated, resolved current bond with exactly one matching address and known SDK address type gets `is_current=true`. An unauthenticated link, unknown type, mismatched/private alias or ambiguous list produces no current-row claim. A disconnected link also clears the match immediately without a flash read. SDK indices may be private or pseudo addresses. They are not host names or OS identities, are not automatically logged, and do not imply that a peripheral can initiate a connection to a selected saved host. Product UI shows saved hosts and connection state; RSSI is not a product signal display.

## Confirmed forget operation

The coordinator owns user confirmation and cancel/release/disconnect. `ble_hid_forget_bonds(generation)` rejects an active link, stale generation or outstanding notification rather than bypassing that boundary. Acceptance closes pairing admission and starts one bounded worker operation. The worker waits for advertising to actually stop, requests one bond removal at a time, matches its callback address and checks that the actual SDK count decreased. A successful callback without deletion is insufficient.

After an actual zero-bond read, the worker commits an empty versioned `hid_cccd/subscriptions` blob and verifies zero bonds again. It never erases the NVS partition. Only completed success publishes `DONE`, a known empty list and a new 120-second pairing window. A submission error, callback error, unreadable list, lack of deletion progress, subscription commit error or ten-second deadline publishes `FAILED` with an error and unavailable/partial cache; it cannot silently claim success. Late callbacks cannot reopen pairing after terminal failure. Existing bonds that were not removed remain stored after a partial failure.

## Native saved-host management

Current-host selection covers both an already selected durable target and a
current authenticated bond without a selected preference. After input release,
the former skips persistence and the latter saves policy while preserving the
same physical link. Tests retain the actual CCCD readiness state and cover
unreserved/stale edits, provisional links, cancellation, NVS failures, link-down
and same-address key replacement during commit. Other-host selection retains
the existing release/disconnect and host-initiated reconnect checks below.
Live submission requires the exact Select reservation, captured host generation
and approved complete target. An Add reservation, public select/preferences or
reorder call cannot borrow it. Completion confirms the verified durable target
preference; input readiness remains a separate live observation, including when
a CCCD disable overtakes the manager's earlier connection snapshot.

The native `app_bluetooth_management` workflow uses the production worker in
this suite. Add waits for input release and actual link-down, saves a revisioned
unselected target policy, verifies its matching durable acknowledgement, then
opens the bounded pairing window and waits for advertising completion. It
preserves every existing bond. Forget all uses the separate no-pairing worker
entry point, verifies actual deletion and subscription cleanup, and saves an
empty durable target policy before completion. Only a later explicit Add opens
new admission. Single-host Forget matches the exact fingerprint, address type
and revisions, awaits its deletion receipt, preserves all other bonds and
subscriptions, and durably reconciles the remaining selected policy and order.

The composition covers selected-target Add and saved reconnect, deletion of a
selected or unselected host, simulated preference reboot, applied-on-error NVS
uncertainty, stale revisions/fingerprints, cancellation after SDK submission,
and the separate 30-second preparation deadline. Real connect/disconnect
callbacks verify that an admission-induced generation increment is not actual
link-down, a replacement before acceptance aborts, and replacement after
acceptance cannot trigger another disconnect or mutation while connected.
Timeout and cancellation cannot reopen pairing through a late callback. The
SDK/NVS boundary remains substituted; target radio and host interoperability
still require device qualification.

`manual_disconnect_cases.inc` reproduces scoped pause failures on Add, Select,
Forget and authenticated private-identity resolution. It covers last-host
Resume, Forget A followed by Select/Allow reconnect B while another pause stays
intact, provisional-link management with a pending neutral release, and repeated
failed authentication at zero bonds followed by a successfully paired new host.
Provisional links never become user-manageable hosts or input-ready. The tests
do not establish which physical host retried or why a radio timeout occurred.
Integrated-review regressions also cover provisional ACL loss during Add/Select,
both BLE/Classic reservation orders, cancellation/failure release, and rejection
of actual admitted-host or saved-identity changes before mutation.

`management_edges_cases.inc` injects link-down after the UI observation, at the
management reservation lock and at disconnect submission. It also replaces an
admitted host, changes the catalogue/policy and injects a real transport failure.
Use `BLE_MANAGEMENT_EDGES=all python3 tests/ble_hid/run.py` for these focused
cases across the three native configurations. Management authority is the
approved admitted-host generation (zero for none), plus catalogue/policy
revisions; SDK teardown still uses the actual physical generation.

## Internal RSSI diagnostics

The worker optionally requests actual asynchronous RSSI at two-second intervals for an active link. Samples are in dBm, valid only for SDK success with values -127 through 20, and expire after five seconds. Sentinel 127, failures and disconnect invalidate them. There is no percentage conversion or inferred quality. RSSI completion is scoped to its captured connection generation and address. A two-second timeout quarantines the pending request until its callback drains, including across reconnect to the same address; RSSI failure does not fault HID. These fields are diagnostic only and do not add a signal indicator to product UI.

## First-failure diagnostics

`ble_hid_failure()` returns an allocation-free, thread-safe value copy with `recorded`, a static `stage` name, `error`, `sdk_status_known`, `sdk_status` and `uptime_ms`. Before a failure the stage is `none`, `recorded=false`, error is `ESP_OK` and time/status are zero. `ble_hid_failure_stage_name()` is a convenience static label; use the copied record's own `stage` when formatting its other fields together. Stage strings have static lifetime and contain no peer address, bond material, report payload or caller-owned text.

The record captures the first observed setup or transport failure, including a recoverable authentication/report failure. It survives disconnect, successful reconnect, repeated initialization requests and later failures until reboot. It is historical evidence: `recorded=true` does not imply the current link is failed or not ready. There is no runtime clear, storage write, additional queue, task or timer. Recording uses the existing state lock and a monotonic `esp_timer_get_time()` read only for the first failure, so its 64-bit millisecond uptime does not wrap with the FreeRTOS tick counter. Reading diagnostics never samples time or changes control state.

Synchronous initialization records the failing step, such as controller initialization, worker allocation or advertising submission. GAP and GATTS failures preserve the raw callback status before existing generic error handling can replace its detail. Authentication preserves the SDK failure reason, notification completion preserves its GATT status, and a matching failed removal preserves its SDK status. Status is unknown when no callback status exists; its zero value must not be interpreted as SDK success unless `sdk_status_known=true`. A successful callback with invalid adapter metadata can have raw status zero alongside an adapter error.

Normal admission/API rejection, explicit disconnect, optional RSSI failures and ignored stale link jobs do not consume the record. A current SDK notification submission error is recorded even when existing recovery permits later successful reports. Deadline, storage, queue and actual setup faults retain their existing control behavior; diagnostics do not open/close gates, cancel actions, clear failures or retry work. This permits the parent health loop to print the original cause after USB CDC attaches without requiring an early serial capture.

Regressions cover NVS/controller/worker initialization failures, an asynchronous registration failure before initialization returns, deferred attribute-table failure after initialization, all advertising failure completions, raw authentication status, notification failure/recovery and later failure, retained deadline evidence beyond the 32-bit tick range, repeated initialization, simulated reboot clearing, ignored obsolete jobs and normal nonfault operations. Repeated accessor calls preserve the complete control state, and 100000 healthy lifecycles perform zero diagnostic clock reads. The clock substitute models the SDK monotonic boundary; target timing and actual memory exhaustion still require device evidence. No external-stack or Bluetooth memory-debug configuration is enabled by this change.

## Configurable local Bluetooth name

The adapter uses `device_settings_bluetooth_name_valid()` and the shared default/bound. A name is 1 through 18 UTF-8 bytes, excludes NUL/C0/C1 controls, malformed/overlong UTF-8 and surrogates, and has no outer ASCII space. Inputs have an explicit byte length and need not be terminated. The maximum complete primary advertisement is 31 bytes: flags 3, appearance 4, compressed HID UUID 4, name header 2 and name 18. No truncation or split UTF-8 sequence is used.

`ble_hid_name_request(utf8, length, &request_id)` copies one bounded request without calling Bluetooth or NVS. It accepts a pre-init request so initialization uses the stored name directly. The parent should submit its validated stored setting before `ble_hid_init()`. An accepted name cannot be replaced while pending; rejected input, partial initialization, forgetting, adapter fault and uncertain previous work leave the request and caller's output ID unchanged. A verified identical name completes without SDK or advertising churn.

`ble_hid_name_snapshot()` is a cached value copy exposing the request ID, requested name, verified active name/availability, state, primary error/raw SDK status and rollback result. The parent owns settings persistence and must save only the matching `APPLIED` request with `active_known=true`. `active_known` can describe the verified old value while an update is still pending; neither that flag nor a successful submission alone confirms the requested apply. No name is automatically logged or written to NVS by this adapter.

The worker serializes stop/read/set/read/advertising/scan-response/restart. Existing startup completions retain their owners. The pinned setter has no completion event, so the worker uses `esp_ble_gap_get_device_name()` and `ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT` to verify its actual result; `ESP_OK` from the setter is not treated as apply success. SDK callback data is copied/checked during its valid lifetime. Each advertising payload is configured and acknowledged before the next step. A disconnected update completes after advertising restart; a current HID host stays connected, with updated payloads prepared for its eventual normal reconnect. Connection events prove when legacy advertising already stopped, even if the explicit stop/start completion arrives later.

Reported failures restore the actual previously read bounded name, verify it, reconfigure both payloads and restore advertising when disconnected. `FAILED` with `rollback_confirmed=true` proves that restoration; it preserves the primary error. A failure before changing the name needs no rollback. Apply and rollback each have an independent five-second deadline. A missing callback or failed rollback becomes `UNCERTAIN`, retains truthful availability/error state, and blocks further name requests and advertising restarts until reboot. Its callback owner remains quarantined; a late callback can drain it but cannot complete another request. An existing HID connection remains usable through name failure/timeout. Normal module faults retain their existing teardown behavior.

Renaming never changes local address/identity, bonds, security policy, HID generations or subscription records, and it never sends a HID release or report. Host display caches are outside this adapter's control and may retain an old name until refreshed. Device tests are still required to observe the actual on-air payload and host cache behavior.

Regressions cover bounded/shared UTF-8 validation, caller-buffer ownership, pre-init stored names, startup callback isolation, request correlation/no-op, full-length UTF-8 advertisements, live HID input during apply, unchanged bond fingerprints/subscriptions/NVS counts, restoration of a previous non-default name, readback mismatch, stop/read/payload/restart submission and callback failures, rollback failure, timeout/late callbacks and connect/disconnect races during stop/restart. The native suite exercises both configured bond capacities with the real shared validator.

## Evidence and qualification

The production native manager/worker composition also keeps the next-host
`release_pending` barrier set throughout Add, Forget all, selected single-host
removal and unselected single-host removal. Each runs from an initially
disconnected state and after confirmed disconnect. The original manager stalls
in RELEASE; the repair completes without clearing the barrier. Connected
pending release/congestion still blocks disconnect, and removal verifies the
exact remaining bonds, subscription cleanup and durable target policy. These
SDK/storage substitutes do not establish physical pairing or deletion.

Regressions cover both CCCD gates, missed unsubscribe/resubscribe and suspend/resume edges, stale generations, malformed/prepared writes, LED padding, backpressure, disconnect during submission, asynchronous notification failure, bounded queue overflow, coordinator disconnect, missing completion, tick rollover, stale ATT transactions and 100000 connection lifecycles. Persistence cases include reconnect and simulated reboot without CCCD rewrites, disabled values, different peers, changed LTK with the same IRK, unchanged-write wear avoidance, pending initial bonding, restore/write/disconnect races, corruption/full/open/commit failure, explicit PSA initialization and BAD_STATE failures, stale lookup/commit failures after peer replacement, legacy bond rejection, missing key/list evidence, and bounded record capacity/reuse.

Management regressions cover first-boot zero-bond admission, expiry without reopening, manual open/close, saved reconnect outside the window, rejection and handshake deadlines, old rejection jobs, asynchronous RSSI validity/age/failure/timeout and same-address stale samples, confirmed forgetting with pending notifications/advertising/rejected peers, sequential removal of 32 bonds, partial failure and late callbacks. Three independently paired hosts preserve separate keyboard/mouse subscriptions across repeated reconnects without erasing other hosts. Cached-list tests include no SDK/storage calls during repeated copies, actual refresh after pairing, type/identity mismatch, duplicate and private/unmapped addresses, read errors, count races, known-empty clearing and capacity bounds. The complete adapter suite runs with both 15- and 32-bond SDK configurations, including full-capacity new-host rejection without deletion and saved-host reconnect. Native substitutes establish state and failure behavior, not radio interoperability.

The IR-09 regressions separate authentication callback delivery from worker execution: a final-slot bond is persisted, authenticated and disconnected before the worker; the durable list must still reach capacity and include that peer, close new admission, preserve every previous peer and permit its later saved reconnect. Additional cases cover authentication arriving after disconnect, failed refresh and recovery through a saved host, authentication overtaking an in-flight list read, and actual capacity becoming full while the published cache still has a free slot. Security/encryption call counters prove that stale capacity cannot authorize another new pairing. Both configured-capacity runs include these cases.

The peer mapping tests model the SDK's stable resolved address. Actual rotating-address resolution, both host-initiated and peripheral-requested encryption completion, bonding across power cycles, flash timing, stack headroom and iPhone/iPad/Windows/macOS compatibility require device qualification. The transport exposes no fabricated Battery or Device Information values and makes no complete HOGP profile qualification claim.
