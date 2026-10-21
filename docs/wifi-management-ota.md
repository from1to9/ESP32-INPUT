# Wi-Fi management, LAN services and firmware updates

Status: proposed contract, 2026-10-21. Design and source review only; no implementation, downloads, partition changes or device writes are authorized by this document. The published Wi-Fi foundation inspected at `d0f1f60706613ce7b7339e1cd14066b7e81f84ae` remains unchanged. This proposal extends [basic provisioning](wifi-provisioning.md), the [remote-control contract](remote-control.md), the [complete function matrix](api-function-matrix.md) and [shared configuration](configuration.md). Complete their functionality/schema/authority planning before implementing new interfaces; no proposed capability becomes functional through this document.

## 1. Product and module boundaries

Wi-Fi supplies background LAN management/control and update connectivity. Bluetooth and USB are HID target transports. They are independent settings:

| Action/state | Required relationship |
| --- | --- |
| Change Mouse/Keyboard/Audio mode or Bluetooth/USB selection | Does not select, disable or forget a Wi-Fi network |
| Scan, join, disconnect or forget Wi-Fi | Does not select/disconnect a Bluetooth/USB HID target or redirect its queued input |
| Lose Wi-Fi during local HID use | Local HID continues, subject to actual radio coexistence. No automatic HID transport fallback |
| Lose a remote-control channel | Revoke its input lease and release/cancel its input through the coordinator; do not physically disconnect the HID target merely because LAN access failed |
| Lock/screen off | Screen off leaves background network services running. On-board microphone capture instead requires a readable on-screen indicator and prevents blanking; qualified dimming is allowed. Lock rejects remote mutations and cancels cancelable privileged work; authenticated read-only status and saved-profile reconnect may remain available. Neither can unlock the device |

One network manager owns the Wi-Fi driver, station/AP interfaces, scans, profiles and join state. A separate authenticated LAN service posts typed management/input intents. A separate update manager owns one firmware job. A resource arbiter coordinates radio work and flash writes with the production input coordinator. Network/OTA workers never send HID or mutate LVGL. Callbacks carry job/generation identities and publish state.

Initial proposed build ceilings: **8 saved profiles, 32 scan records, one scan/join radio job, one firmware job, two authenticated LAN clients, and 16 recent job records**. Practical requested limits and scheduling preferences use shared configuration metadata within these ceilings; no duplicate hardcoded UI/API/MCP choices. Capacities and timing below require qualification. The current foundation supports one WPA2 network and temporary provisioning, not this expanded interface.

## 2. Pinned SDK feasibility

Inspected clean ESP-IDF checkout `383dda6c182e3dabf6355160166fe007df775629` (6.2.0, S31). Executable dependencies remain pinned; these links point to that source revision.

| Evidence | Supported mechanism / boundary |
| --- | --- |
| [Wi-Fi API](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_wifi/include/esp_wifi.h) | Nonblocking `esp_wifi_scan_start(..., false)`, completion event, stop, bounded result copy and explicit result-list release. Scan can reject while connecting; excessive dwell can disconnect STA |
| [Wi-Fi types](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_wifi/include/esp_wifi_types_generic.h), [configuration](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_wifi/Kconfig) | Scan records include BSSID, SSID, channel, RSSI, auth and cipher information. SDK auth enums/configuration are richer than the current product's WPA2 join form |
| [HTTP client](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_http_client/include/esp_http_client.h) | HTTPS certificate/name validation, explicit timeouts/buffers and disabled automatic redirects; supports a controlled firmware pull worker |
| [HTTPS OTA API](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_https_ota/include/esp_https_ota.h), [implementation](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/esp_https_ota/src/esp_https_ota.c) | Begin/perform/abort support interrupted downloads. **Finish selects the new boot partition** after validation; it is not a download-only finalizer |
| [Core OTA API](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/app_update/include/esp_ota_ops.h) | `esp_ota_begin/write/end/abort` separates staging from `esp_ota_set_boot_partition`; running/inactive partition and image-state queries support explicit activation |
| [Rollback configuration](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/bootloader/Kconfig.app_rollback) | Application-controlled confirmation is available. Default startup confirmation happens before `app_main`; anti-rollback is a different policy and can consume irreversible eFuse security-version bits |
| [NVS API](https://github.com/espressif/esp-idf/blob/383dda6c182e3dabf6355160166fe007df775629/components/nvs_flash/src/nvs_api.cpp) | Set/erase can change flash before commit. Errors cannot be represented as guaranteed rollback; retain the foundation's sticky storage uncertainty |

The inspected generated build enables SAE, OWE and enterprise SDK support, plus rollback with `CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_ON_STARTUP`. These are build observations, not qualified product features. No scan UI, profile manager, authenticated permanent LAN service, image-signing policy or product OTA health gate is established by them.

## 3. Scan, select and join

Start scans only on an explicit local action or a bounded reconnect decision. Use a job ID and generation; never run overlapping scan/connect operations. Initial list lifetime is 30 seconds, manual refresh cooldown 10 seconds, total scan deadline 15 seconds. Prefer incremental legal-channel scans with qualified short dwell/home-channel intervals; default connected scanning must preserve STA service as far as the SDK permits. Do not modify global scan parameters during an active connection job.

Copy at most 32 records into a fixed application buffer, record total found/returned/truncated, then release SDK scan memory on every completion/error/cancel path. `esp_wifi_scan_get_ap_records` frees the whole driver list, including records beyond the application's output capacity; error paths use `esp_wifi_clear_ap_list`. Cancel must finish driver cleanup before another scan starts. A late completion from an old scan cannot replace the current list.

The SDK internally allocates scan records dynamically; a 32-row UI cap does **not** prove an internal heap cap. No configurable driver-result cap was verified in this review. Establish a minimum free internal-heap/largest-block gate, abort/cleanup on pressure, test dense AP environments and expose `scan_unavailable:resource_budget` if the target cannot meet the safety budget. Do not claim strict memory qualification from a bounded copy alone.

Each row carries `scan_id`, `record_id`, raw SSID bytes/length, escaped display label, BSSID, channel/band when known, measured RSSI plus age, advertised auth/ciphers, saved-profile match and **join availability/reason**. SSIDs are at most 32 bytes and are untrusted data. Preserve raw bytes in the protocol; show an escaped byte label if they are not displayable UTF-8. Hidden SSID rows offer explicit manual entry. Do not merge different security configurations just because their names match. Never infer a supported band from another ESP32 variant or show Bluetooth signal bars from Wi-Fi RSSI.

| Advertised security | Initial product join policy |
| --- | --- |
| WPA2-Personal | Enabled after scan/join qualification; 8..63 printable ASCII passphrase initially. Raw 64-hex PSK is a separately advertised encoding |
| WPA/WPA2 transition | WPA2 negotiation only, with an explicit WPA2 minimum; never silently fall back to WPA1 |
| WPA2/WPA3 transition | Use the displayed supported policy. WPA2 initially; optional **Require WPA3** only after SAE/PMF qualification. Do not set a mixed threshold blindly: the SDK documents stronger-mode threshold semantics |
| WPA3-Personal only | Detected, initially unavailable until SAE/PWE/PMF and host/AP compatibility tests pass; compiled support alone is insufficient |
| Open / OWE | Detected distinctly. Initially unavailable; later explicit open-network policy or qualified OWE support, without a fake passphrase prompt |
| Enterprise/EAP, WAPI, DPP, unknown, WEP, WPA1-only | Display exact known category and unavailable reason. No generic PSK form pretending to support certificate/EAP onboarding; obsolete/unknown security is rejected |

Selection references the scan generation and row. Expired/replaced rows return `stale_scan`; the UI refreshes and asks the person to select again. A profile's stable identity is its random `profile_id`, raw SSID and security policy, not an RSSI value or guessed host name. BSSID hints may guide one attempt; persistent BSSID pinning is a separate explicit option, never a trust guarantee.

Join states are `queued -> waiting_for_radio -> associating -> obtaining_ip -> connected`, with `failed`, `cancelled` or `cleanup_pending` terminals/cleanup states. Association is not DHCP; IPv4 is not Internet/catalog reachability. Captive portals and blocked update servers are separate outcomes. One join attempt is bounded to 15 seconds. A cancelled manual switch may attempt the previous profile once under the same deadline, then report its actual result rather than claim restoration.

## 4. Saved profiles and priority

Store at most eight profiles: ID, raw SSID/length, explicit auth/minimum-security policy, enabled flag, unique priority order, credential presence and protected secret material. Metadata APIs never return passwords, credential lengths or secret-bearing exports. Editing a profile preserves an existing secret only when the request explicitly says so; a masked display value is not a new password.

The UI offers **Join once**, **Remember and join**, and **Save without joining**. Join once changes RAM only. Remember-and-join reports runtime connection and persistent save independently; a connected network may still have a save error. Save without joining cannot claim connection. Priority edits send the complete ordered ID permutation with an expected profiles revision; reject omissions, duplicates, unknown IDs and over-capacity requests without eviction.

On boot/reconnect, try enabled profiles in saved order, using compatible fresh scan evidence where available. Limit each profile to two attempts per round, each at most 15 seconds; delay subsequent rounds with capped 1/2/4/8/16/30-second backoff. Authentication failures quarantine that profile for five minutes or until an explicit credential edit/retry. Do not repeatedly disconnect a healthy connection merely because a stronger or higher-priority AP appears. No automatic fallback to a weaker security policy or disabled profile.

Use a versioned, bounded profile codec, proposed maximum 4 KiB per bank, with two NVS keys containing generation, length and checksum. Write the inactive bank and verify it by readback; select the highest valid generation on boot. This tolerates an incomplete bank but does not turn NVS into a rollback transaction: an errored write can already be present and selected at reboot. Expose runtime selection, saved revision, known/present record state, stored SSID metadata and sticky `storage_uncertain/storage_error` separately. Only an explicit successful save/forget/read verification resolves uncertainty. Never erase the whole NVS partition to recover space.

Migrate the legacy `WIF1` single-network record only after validating it, retaining its original bytes until the new format and rollback compatibility are verified. Unreadable records remain forgettable without inventing an SSID. Keep old-schema data readable by the previous working firmware during OTA probation; no destructive migration before confirmation. Measure NVS capacity alongside bonds/settings before enabling eight profiles; reject a full store without evicting another namespace. RSSI, reconnect counters and heartbeats never cause per-frame/per-event persistence.

## 5. Persistent authenticated LAN interface

The temporary WPA2 setup hotspot remains a local, expiring provisioning service. It does not grant LAN credentials, activate remote HID, or inherit administrator scopes. A separately enrolled, TLS-authenticated service binds the **station interface** and may persist its locally approved enable preference across boots. Every boot starts without a remote input owner or resumed privileged job.

Reuse the enrollment, exact Origin/Host, CSRF, host-only session-cookie, native-client credential, authenticated subscription and certificate-trust rules in [remote-control.md](remote-control.md#4-enrollment-secure-transport-and-sessions). Browser certificate installation remains a real deployment requirement; the native Mac bridge can pin the device identity. Do not copy the hotspot's HTTP/token model onto the normal LAN. Provisioning tokens/cookies and LAN sessions have distinct audiences and are not interchangeable.

Scopes follow the [matrix](api-function-matrix.md), including view, wifi.configure, input.control, update.check/stage/activate and separate admin.policy authority. Device lock blocks mutations except cancellation/safety operations; administrator/AI scope cannot unlock. Reconnection invalidates an input lease, while ordinary Wi-Fi service state remains independent of HID mode/selection. MCP initially lives on the Mac using typed wrappers over this API. Read/check tools can be separately granted; download/activation tools require the same device-enforced scopes and explicit or policy-delegated authority as human clients. An enrolled authorized admin can enable a constrained updater policy without forcing a new physical confirmation for every matching update. Ordinary input tools receive no update/admin scope implicitly.

Except session/upgrade transport bootstrap, all domain mutations use POST /api/v1/commands and one typed operation registry, optionally with the same envelope over WSS. All job-producing operations return 202 with job_id, not a success claim. Common mutation fields are `v`, `boot_id`, `request_id`, authenticated session binding, `management_generation`, expected resource revision and monotonic expiry. Management requests target the device, not a BLE host. HID operations retain their separate input epoch/target/lease guards. Bounded duplicate suppression returns the original result; reconnect does not replay requests or jobs. A missing result is `unknown`, not permission to repeat an activation.

| Read resource or registry operation | Contract |
| --- | --- |
| GET /api/v1/state, /capabilities, /config/schema, /config | Bounded scoped status/metadata; no active scan or probe as a GET side effect |
| wifi.scan.start/cancel; GET /api/v1/wifi/scans/{id} | One generation-bound job, at most eight rows/page from an immutable 32-row snapshot; count/truncation/age/cursor |
| config.stage/apply/cancel on wifi.profiles/profile_order/reconnect fields | Common typed metadata/validation, expected revisions, write-only secrets. No duplicate PUT/POST per-field setter or hidden destructive forget |
| wifi.profile.forget; wifi.storage.verify | Separately scoped explicit deletion/read reconciliation; unreadable/unknown records remain recoverable without a guessed SSID |
| wifi.join/disconnect, wifi.setup.start/stop | Runtime actions, current profile/scan/security/deadline; temporary hotspot and persistent LAN have separate audiences |
| GET /api/v1/firmware/releases; firmware.check | Cached read or explicit check of an approved catalog/channel; no arbitrary URL from a model |
| firmware.download/discard | Release/digest/state revision and explicit or delegated job authority; inactive-slot staging only |
| firmware.activate | Verified image/slot/state plus fresh one-use job authority issued under current admin policy or explicit approval; selection/restart, not a download |
| config.stage/apply/cancel on update.policy fields; update.policy.revoke | Enrolled admin.policy can enable, bound or revoke background update delegation remotely; no per-update forced physical prompt within that policy |
| job.cancel; GET /api/v1/jobs/{id}; authenticated /api/v1/events | Scoped job/cancel/cleanup/terminal evidence; lost response is unknown, never automatic replay |

Local manager APIs mirror those registry operations through bounded workers. UI and MCP bindings are generated from the same operation/configuration metadata. The published app_wifi API is not modified until the coordinated complete contract is reviewed; no new interface implementation is part of this phase.

## 6. Firmware source, identity and authorization

Configure one approved HTTPS catalog origin/channel initially. The device fetches only release objects named by that catalog. A private LAN firmware server is allowed only through explicit local enrollment with its exact endpoint and TLS identity; it is not an arbitrary-URL fetch feature. Disable automatic redirects, credentials in URLs, custom proxy transports and redirects to HTTP. Reject unapproved host/port/path or DNS destination changes; never forward a server credential to a different origin. Loopback, link-local and multicast targets are forbidden; approved private LAN origins require explicit policy rather than a blanket private-address bypass.

Require verified server identity/name and a supported trust/time policy. If certificate validity cannot be evaluated, expose `time_or_trust_unavailable`; do not disable verification. Trusted-time provisioning and certificate rotation need an explicit tested deployment path. HTTPS alone does not authorize a release.

Proposed signed manifest envelope: at most 8 KiB, exact signed payload bytes encoded as `payload_b64`, a fixed permitted key ID and detached ECDSA-P256/SHA256 signature. Pin the algorithm/key mapping in firmware; no caller-selected algorithm or `none`. Verify before parsing the payload into release policy, then reject duplicate/unknown mandatory fields and invalid ranges. Target crypto/key-rotation qualification remains an implementation gate. Image signing requires the separately enabled and tested SDK secure-OTA verification configuration; a SHA-256 value by itself is not publisher authentication.

The signed payload binds: product/board family and revisions, chip/revision constraints, release ID and monotonic release index, display version, secure version, project identity, total image bytes including signatures/padding, SHA-256, exact HTTPS image object, required partition-layout ID, settings read/write compatibility, minimum updater version and expiry. Compare it with the ESP image/app descriptor and actual device/partition information. Reject wrong board/chip/layout, oversized/truncated images, version conflicts and ordinary downgrades. A same version with a different digest is not silently equivalent. Physical recovery is a separately authorized path; ordinary API scopes cannot bypass version policy.

Preflight requires a confirmed running image, trustworthy layout/slot identity, sufficient memory/flash budget, resolved storage uncertainty and valid maintenance authority. That authority may be a fresh explicit approval or an existing scoped administrator policy. Policy metadata binds principal, source/channel, compatible release/version/size, finite download deadline, idle/maintenance/restart budgets, expiry and revocation. Default auto-activation is off; an authorized admin may explicitly enable it within those limits without a new physical prompt per update. No policy can waive lock/local stop, image verification or protected-region restrictions.

Do not advertise independent external-power detection on this board. Show telemetry as known/unknown; a policy may carry an explicitly operator-declared stable-supply assurance and expiry, labeled as a declaration rather than a measurement. A policy requiring unavailable measured power blocks/defer-starts rather than inventing it. Battery percentage is a gauge estimate, not proof of safe supply. Hardware power-loss qualification remains mandatory.

## 7. Stage, activate, confirm and cancel

Use a dedicated HTTPS client plus core OTA APIs for the initial **download without activation** contract. `esp_https_ota_finish()` changes boot selection in this SDK, so the high-level wrapper cannot be used as a supposedly passive download finalizer.

```text
idle -> checking -> release_available
     -> waiting_for_authority -> waiting_for_input_release
     -> downloading_to_inactive -> verifying -> staged
staged -> waiting_for_activation_authority -> selecting_boot -> restart_pending
new boot -> pending_health_check -> confirmed | rollback
cancelable states -> cancelling -> cancelled | cleanup_failed
```

1. Under fresh explicit or policy-delegated authority, enter maintenance through the production coordinator: stop queued input, mute, disable motion intent and complete the selected transport's neutral-release barrier. A stalled connected transport must recover/disconnect or the operation fails; no fake release acknowledgement. Record the preserved mode/transport and never resume old input afterward.
2. Select an inactive OTA app slot using the actual partition table. It must differ from the running image, the protected factory image and any already-selected next-boot image. Refuse staging while current-image probation or unresolved boot selection exists.
3. Stream through fixed buffers, proposed 4 KiB application chunks, two-second socket timeout, ten-second no-progress deadline and 30-minute total job deadline. No full-image RAM buffer. Use sequential sector erase/write where the SDK permits; no bulk erase by default. Initial version disables reboot/range resumption. At most two network retries may restart the same authorized image from byte zero in the inactive slot; flash/validation errors require a new explicit decision.
4. Validate actual received length, complete-body status, streamed SHA-256, flash readback hash, image header/descriptor and configured signature verification. Complete `esp_ota_end` successfully. Free HTTP/TLS/OTA resources and report `staged` only after verification. **Do not call boot selection here.** Unverified or partial bytes remain ineligible for activation, not an installed release.
5. A staged image may persist, but a reboot requires rediscovery and full verification before any activation; job/approval tokens never resume. Staged metadata is an untrusted hint after an uncertain NVS write. An explicit discard invalidates metadata without pretending that flash bytes were securely erased.
6. Activation requires fresh one-use job authority bound to image digest, slot, device/boot state and current policy revision. Manual approval defaults to a 120-second lifetime; a current background policy can issue matching bounded job authority without physical confirmation for every image. Revoked/expired/out-of-policy work is denied. Re-enter the release/maintenance barrier if input resumed after staging. Call `esp_ota_set_boot_partition`, inspect its result and query boot selection. A selection error can be `boot_selection_uncertain`; do not assume rollback or immediately retry/restart. Once selection is committed, cancellation cannot promise to undo it. Show `restart_pending`, then restart only under the approved activation contract.
7. New firmware must use `CONFIG_BOOTLOADER_APP_ROLLBACK=y` and `CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP=y`, with startup auto-confirm disabled. Within a proposed 45-second first-boot health window, verify board identity, required driver/resource initialization, settings compatibility, coordinator/release gates and worker/watchdog liveness. A connected HID host, USB enumeration or reachable Internet is not required for a powered device to boot correctly. These checks do not substitute for release-level host qualification.
8. Confirm using `esp_ota_mark_app_valid_cancel_rollback()` only after that gate. Reject critical failure with the supported rollback path; a crash/reset before confirmation must roll back. Read back image state after uncertain confirmation results rather than invent success. Keep previous-version settings readable and avoid destructive schema/key changes during probation. A startup error loop must not feed watchdogs forever instead of confirming or rolling back.

Cancellation is cooperative and observable: `accepted_cancel` is not `cancelled`. Latch it outside ordinary queues, stop at the next safe network/flash boundary, abort/close all resources and keep input gated while cleanup is uncertain. A socket or sector operation can delay cancellation; measure the bound. Global lock/principal or policy revocation cancels work before boot selection, and no background job can unlock. A finite explicitly policy-bound job may outlive its initiating browser session only within delegated lifetime; ordinary session-bound jobs cancel when that session expires. After selection, return `too_late`/actual boot state and require a deliberate recovery decision. Progress distinguishes bytes received, bytes written this attempt, verification, boot selected, new image running and health confirmed.

Hardware anti-rollback is deliberately separate: the pinned SDK requires an OTA-only layout without factory for that configuration, and eFuse changes can make the previous image unrecoverable. This factory-preserving proposal does not enable it or burn fuses. Signed release-index policy provides software downgrade control, not physical rollback protection on unencrypted development flash.

## 8. Input priority, powered operation and resource limits

Use shared work.policy metadata: **Input first** (default), **Balanced** and **Management first** only when their resource profiles are implemented/qualified. No policy outranks emergency lock, key/button release or watchdog servicing. Input first defers scans/profile writes under active pointer/key traffic, lowers LAN state refresh and stops optional diagnostics before consuming input reserves. The proposed idle background profile uses a 500 ms quiet interval and pauses/defer-starts when input returns; budget values come from the same schema, not separate UI/MCP literals. A queued scan/save must expose `deferred:input_busy` and a finite deadline rather than wait invisibly forever. An explicit **Run now** request may show a radio-impact warning, but cannot waive safety gates.

Priorities alone cannot make flash writes harmless: flash erase/write can stall cached execution and affect USB/Bluetooth timing. Therefore the first OTA download-to-flash implementation requires explicit maintenance and pauses HID producers. Firmware metadata checking can remain background work. Concurrent input plus OTA flash writing is a later capability requiring real interrupt/cache/radio measurements; do not advertise it from low task priority alone.

One resource arbiter serializes scan/join, profile/settings persistence and OTA flash ownership. Disallow pairing/schema migration and competing first-party flash writers during update maintenance; verify SDK-owned persistence behavior separately. Budget application job/result/scan/profile buffers statically; admit TLS only above measured internal-heap/fragmentation reserves. Do not enable a capability until SDK-internal allocations and worst-case dense scans fit the measured budget. Never increase a watchdog timeout just to hide a blocked worker.

For extended powered use: keep services alive when the screen sleeps; snapshots/status must not reset inactivity. Bound counters, retries, logs, DNS/TLS failures, result history and event subscriptions. No periodic flash heartbeat, unbounded offline queue or unconditional update/restart on reconnect. A deliberately enabled finite administrator policy may schedule approved checks/staging/activation under its configured bounds; it cannot replay an old job or approval. On reboot all executable jobs stop, then policy scheduling needs a new evaluated authorized job, never a resumed flash operation. Proposed qualification includes repeated job/cancel cycles, 8-hour mixed-input/network soak and 24-hour idle/reconnect soak, with heap/stack/radio/error trends; these durations are plans, not completed evidence.

## 9. Current partition limitation and migration proposal

The inspected [partition table](../firmware/partitions.csv) has `nvs` at `0x9000/0x6000`, `phy_init` at `0xf000/0x1000`, and only `factory` at `0x10000/0x400000`. It was a working-tree candidate at the original `d0f1f6` review and is tracked at the subsequent `41d6b42` integration checkpoint. The inspected local generated build uses 16 MiB flash, bootloader at `0x2000`, table at `0x8000`, and a 2,415,232-byte candidate application. These are source/build observations; the actual device's flash capacity, installed layout and original contents were not read in this task.

The current layout has no OTA data partition or inactive OTA app slot. A rollback configuration flag cannot create them. Proposed layout **only if actual 16 MiB hardware and the existing regions are verified to match**:

```csv
# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, 0x9000, 0x6000,
phy_init, data, phy, 0xf000, 0x1000,
factory, app, factory, 0x10000, 0x400000,
ota_0, app, ota_0, 0x410000, 0x400000,
ota_1, app, ota_1, 0x810000, 0x400000,
otadata, data, ota, 0xc10000, 0x2000,
```

This preserves the existing NVS/PHY/factory offsets and sizes, uses aligned four-MiB application slots, and leaves the remainder unallocated pending a reviewed storage plan. The CSV passed the pinned ESP-IDF partition generator with a 16 MiB target; that proves syntax/alignment/non-overlap under the supplied capacity, not the installed device layout or recovery. The current candidate's size is not proof that every signed future image fits. Validate complete signed-image size and actual flash capacity before migration. Do not silently shrink NVS to make room for conventional low-address OTA metadata.

Migration is a separate cabled maintenance operation, not ordinary app OTA:

1. Identify the actual device, flash size, installed partition table, bootloader/security state and readable recovery interfaces. If the installed layout differs, derive a new preservation plan; never apply these offsets by assumption.
2. Before any write, acquire a **full original SPI-flash backup** of the actual capacity, plus separate original external NAND/EEPROM/assets where present. Record hashes, parsed partitions, chip identity/security metadata and tools. Store credential-bearing backups privately/encrypted outside Git and retain two verified copies. A flash dump does not contain eFuses or nonexportable keys; encrypted/read-protected devices require a demonstrated vendor-supported restore path. Missing backup/recovery evidence blocks migration.
3. Prove the proposed new slot/metadata areas contain no needed original data; unallocated in a CSV does not mean disposable. Build and validate a signed, compatible bootstrap image and table offline. Keep the original factory bytes and all NVS/bonds/settings untouched. Verify bootloader compatibility with both old factory and new OTA images; replace it only if necessary under a separately reviewed artifact manifest.
4. Under explicitly authorized stable-power USB maintenance, write/verify the bootstrap in a future inactive slot before changing boot metadata. Install the reviewed table/required bootloader and SDK-generated OTA metadata through a tested recovery procedure. Qualify first-boot NEW/PENDING_VERIFY handling; do not hand-write guessed metadata state/CRC values. If that cannot be qualified, retain factory boot selection and stop for a revised bootstrap procedure.
5. Read back and compare preserved regions byte-for-byte, then qualify bootstrap confirmation, failed-boot rollback and full restoration from backup. Never use whole-chip erase as an implicit step. Do not delete the original backup after a successful boot.

Application A/B OTA protects an inactive app write; it does not make bootloader/partition-table migration power-loss atomic. The critical migration window requires a working cabled recovery path and original backup. No partition, bootloader, factory, NVS, key or eFuse update is exposed by the ordinary firmware API.

## 10. Required UI/state contract

Keep Connectivity -> **Input transport**, **Bluetooth peers**, **Wi-Fi**, and **LAN/API** separate. Updates are a separate System page. Preserve Mouse/Keyboard/Audio navigation and the existing wake/lock rules.

| Page | Required fields and actions |
| --- | --- |
| Wi-Fi | Enabled/initializing/connecting/DHCP/backoff/connected, actual SSID/IP/RSSI+age, runtime versus remembered state, storage uncertainty; Scan/Refresh/Cancel, choose row, manual hidden SSID, explicit security policy, protected password entry, Join once/Remember and join |
| Saved networks | Up to eight entries with priority/enabled/current/saved/error indicators; reorder, edit credential, join, disable and confirmed forget. Unknown/unreadable store is recoverable without a guessed SSID |
| Setup hotspot | Separate local start, fresh local password, IP, expiry, clients if measured, stop; cleanup-pending/retry/error instead of usable credentials after revocation |
| LAN/API | Enabled versus reachable, TLS identity/trust, enrolled client/scopes, authenticated viewers and input owner shown separately; enroll/revoke/disable. Setup access is not LAN authentication |
| Background work | Policy, active/deferred job, reason, age/deadline, attempt, received/written bytes, phase, cancel/cancelling/cleanup failure; never a permanent spinning icon without state |
| Updates | Installed version/hash/board/layout, check result, release source/trust/version compatibility, inactive-slot size, approval/maintenance consequences, Download/Cancel, verified Staged state, explicit approval or current delegated activation authority/restart budget, rollback/health result |

Unavailable operations show the actual reason (`not_implemented`, `unqualified`, `security_unsupported`, `resource_budget`, `layout_not_ota_capable`, `storage_uncertain`, `release_pending`, `locked`, `trust_unavailable`). No zero-as-unknown RSSI, generic green Connected, download-equals-installed message or successful-HTTP-equals-target-effect assertion. Update completion means the expected image is running and health-confirmed; a lost response is unknown until authenticated state is reconciled.

## 11. Edge-case acceptance matrix

All cases below are **planned, not run**. H = production host/fault tests; D = actual device/browser/radio; P = controlled power-loss/recovery testing with preserved backups. Record exact firmware/SDK/board/host, initial generations/revisions, events, runtime versus durable state and measured bounds.

| ID | Fault/scenario | Required result | Evidence |
| --- | --- | --- | --- |
| W01 | Wi-Fi changes during BLE/USB input; HID transport/mode changes during LAN use | Independent selections, no redirected/replayed HID; real coexistence faults remain visible | H/D |
| W02 | Wi-Fi loss with local input versus remote lease | Local path retained; remote input cancelled/released, no automatic reacquire | H/D |
| W03 | Scan while connecting, duplicate starts, cancellation/error/late completion | One job; bounded rejection/defer; SDK list freed; stale result cannot replace current | H/D |
| W04 | Dense APs, >32 results, low heap, slow scan | Truncation/count/age truthful, pressure cleanup, no input starvation or hidden driver-memory guarantee | H/D |
| W05 | Hidden/empty/non-UTF-8/HTML SSID, duplicate names with different auth/BSSID | Lossless bounded identity, escaped labels, correct security and selection | H/D |
| W06 | Expired scan/cursor/row, channel/regulatory/band mismatch | Stale/unsupported error; no guessed connection | H/D |
| W07 | Every advertised auth category; transition downgrade; missing PMF/SAE/EAP | Only qualified combinations join; no weaker silent fallback or inappropriate password form | H/D |
| W08 | Wrong secret, AP disappears, DHCP timeout, captive portal, no Internet | Distinct association/IP/catalog outcomes, bounded retries/quarantine; no auto setup AP | H/D |
| W09 | Eight profiles/full store, reorder conflict/duplicates, disabled/high-priority AP | No eviction, atomic intent validation, stable ordered policy, healthy link not needlessly dropped | H/D |
| W10 | Join once/save only/remember-and-join; cancel active switch; forget current/unreadable record | Runtime, persistence and restoration outcomes separate; no fabricated SSID | H/D |
| W11 | NVS write/erase error unchanged/already-applied/partial bank; corrupt legacy/new record | Sticky uncertainty, valid-bank recovery, explicit read reconciliation, no whole-NVS erase | H/D/P |
| W12 | Setup shutdown HTTP/AP failures and repeated stop | Immediate credential/admission revocation; bounded independent cleanup retry, no hot loop | H/D |
| L01 | No/expired/revoked/wrong-scope session; observer mutation; AP token on LAN | Denied before disclosure/admission; no HID/admin privilege crossover | H/D |
| L02 | Hostile Origin/Host/CSRF, rebinding, bad certificate/name/time, leaked redirect target | Fail closed, no secret forwarding/downgrade; accurate unavailable trust state | H/D |
| L03 | Malformed/oversized/duplicate/stale/expired command; reconnect/lost response | Bounds, generations and duplicate policy hold; no automatic replay or invented completion | H/D |
| L04 | Lock/local takeover/principal or policy revocation/client death while MCP/admin job runs | Device policy wins; session-bound versus explicitly finite policy-bound lifetime enforced; cancel before commit, no unlock or hidden renewal | H/D |
| O01 | No OTA slots/metadata, wrong capacity/layout, running/unconfirmed/next-boot slot selected | Update unavailable/rejected before erase; no protected-region write | H/D |
| O02 | Unsigned/tampered manifest/image, wrong key/algorithm, expired release, bad byte count/hash | Reject without boot selection; no digest-only authentication | H/D |
| O03 | Wrong product/board/chip revision/project/settings range; downgrade/version collision | Precise incompatibility; no generic success or forced version bypass | H/D |
| O04 | HTTPS/DNS redirect/credential-origin change, partial/truncated/chunked body, timeout | Fixed origin/trust/size policy, bounded abort/retry, no bytes beyond slot | H/D |
| O05 | Cancel/disconnect/revoke at each sector/read/hash/end boundary | Cooperative cancellation/cleanup truthful; current image bootable; no input before cleanup barrier clears | H/D/P |
| O06 | Completed staging, reboot before activation, corrupt staged metadata | Boot choice unchanged; full revalidation and new explicit or policy-matching job authority required | H/D/P |
| O07 | Input stuck/release fails; scan/profile/bond/OTA work competes | Maintenance admission fails or recovers explicitly; release priority never waived | H/D |
| O08 | Activation retry/lost acknowledgement/power cut during OTA metadata change | Boot selection reconciled or uncertain; never duplicate activation blindly; only verified images eligible | H/D/P |
| O09 | New image crash/hang/required-driver failure, early auto-confirm accidentally enabled | Bounded probation and rollback; configuration gate rejects startup confirmation | H/D/P |
| O10 | No HID host/USB enumeration/Internet during healthy first boot | Health logic does not invent an external dependency or roll back a valid offline input device | H/D |
| O11 | Confirmation API error, unreadable state, old settings after rollback | No false confirmed state; backward-compatible data; uncertainty/recovery visible | H/D/P |
| O12 | Repeated updates, nearly full slot, signing padding, key rotation, secure-version conflict | Exact size/key/version compatibility; previous working image preserved until confirmation | H/D/P |
| O13 | Enrolled admin enables background update policy remotely; matching/out-of-policy images, expiry/revoke/restart budget | No forced per-update physical prompt for authorized matches; wrong scope/source/version/budget denied, new jobs evaluated and no lock bypass | H/D |
| O14 | Schema-driven limits changed through UI/API/MCP, active-job policy revision changes | Same bounds/effective values everywhere; immutable job snapshot or explicit cancellation, no per-frame writes or hardcoded divergent client defaults | H/D |
| M01 | Actual factory layout differs, backup incomplete/protected or new space contains data | Migration blocked; no guessed offsets or destructive cleanup | H/D |
| M02 | Migration interruption at each bootstrap/table/bootloader/metadata write | Demonstrated cabled original restore; preserved NVS/factory hashes; no claim of migration atomicity | D/P |
| B01 | Sustained input, scan/save deferral, TLS/flash pressure, stalled result reader | Finite jobs/queues, accurate busy/unknown state, measured input reserves | H/D |
| B02 | 8-hour mixed activity and 24-hour idle/reconnect with screen off/no host | Bounded memory/storage/logs/retries, watchdogs alive, no hidden auto update/restart | D |

## 12. Implementation sequence and unresolved gates

First finish and reconcile the complete function/configuration/audio/network/update plans before implementing any new interface. Coordinate additive API/UI snapshots and ownership against the published foundation, using the matrix and one metadata registry. Then implement scan/security discovery and profile storage/recovery; next qualify reconnect ordering and scheduling; then add separately enrolled read-only LAN status and scoped management/MCP operations. OTA starts only after a reviewed migration/recovery package, signing/trust policy and application-controlled health confirmation are ready. Remote HID remains subject to its own coordinator/lease tests.

Open evidence: actual installed device layout/security/backup and restore; NVS capacity with bonds; driver scan heap bounds and supported bands; authentication combinations on real APs; browser certificate/trusted-time onboarding; flash/cache/USB/BLE interference; crypto and signed-image compatibility; probation/rollback/power-loss behavior. This document establishes concrete proposals and source-supported mechanisms, not a passed runtime gate.
