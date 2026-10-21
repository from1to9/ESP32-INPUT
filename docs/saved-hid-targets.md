# Saved HID target preferences

`ble_hid_target_select` selects a saved BLE HID host's reconnect-admission key.
It does not make this BLE peripheral initiate a connection: the host still
connects. There is no nearby-host scan, name/OS inference, signal-strength
ranking, automatic host rotation or simultaneous second HID link. The
coordinator releases input and disconnects before switching a live host.
All bonds and configurable-name behavior remain preserved.

Callers use the exact revision from a fresh `ble_hid_bond_list` plus an address
and known SDK type. A supplied bond fingerprint must match that row; otherwise
the worker binds the request to its actual fingerprint before admission.
Selection is available only for stable public/static keys whose exported
identity is absent or matches the stored key, with an actual SC bond
fingerprint. Offline private/RPA/pseudo keys remain visible but return
NOT_SUPPORTED for selection. The pseudo-address build mode conservatively
disables exact selection. A connection whose key/type/fingerprint is not the
selected, still-verified bond is rejected before HID readiness/notification.
Authentication rechecks the complete bond fingerprint, including its LTK: a
same-address re-pair cannot inherit a selection. Clearing the selection restores
ordinary saved-host admission; an unknown host still requires explicit pairing.

`ble_hid_target_snapshot` distinguishes an unknown bond list, a removed key and
unsupported privacy from an available selection. A removed/changed selected
key keeps admission blocked until explicit clear/replacement against a fresh
list, including after reboot. Missing evidence never becomes an empty list or
guessed identity. Unknown/unselectable legacy bonds remain listed and preserved;
their address/type can retain display order without pretending a fingerprint
was available or granting selection authority.

`ble_hid_target_order` accepts a complete permutation of current known keys.
Duplicates, omissions, stale revisions and unknown address types are rejected.
The saved order filters removed identities and appends newly observed keys on
refresh. It changes presentation/preferences only; it never evicts a bond or
starts a connection. Selection/order require a healthy disconnected state with
no outstanding report, rejection, name operation or bond mutation.

## Durable asynchronous application

`ble_hid_target_preferences_request` atomically submits selection and complete
order against both the bond-list revision and the last applied preference
revision. The existing select/order APIs are submission wrappers. ESP_OK means
bounded copied admission only. Poll `request_id`, `request_status` and
`request_error`; APPLIED requires an actual NVS commit and exact valid readback.
`revision` changes only after verified durable application and survives reboot.
A replay with the old expected revision is rejected without another write.
Request IDs/status are boot-local; a reboot reports the actual loaded policy.

The existing BLE control worker loads/writes the `hid_targets` namespace and
`preferences` key in the original default NVS partition. `ble_hid_store.c`
serializes version 1 explicitly: magic/version, flags/count, a little-endian
64-bit preference revision, selected key, up to 32 ordered keys, and CRC32.
Each key contains the actual SDK address/type, a known-fingerprint flag, and
its 32-byte fingerprint when available. No raw structs, keys, host names,
callback storage, dynamic allocation or automatic NVS erasure are used. The
maximum record is 1340 bytes. The two serialization/readback buffers and one
verification record have fixed single-worker ownership.

Pending/APPLYING blocks new connections and pairing. A reported failure before
any NVS mutation retains the previous active policy and reports FAILED. Once a
write may have occurred, unverified commit/readback reports UNCERTAIN, closes
admission and rejects new preference requests until reboot. Reboot parses the
actual persisted record; it never assumes that a failed commit wrote nothing.
Malformed, truncated, unknown-version or unreadable records fail closed and
remain preserved. No startup default overwrites them. A removed selection is
retained blocked rather than silently choosing another saved host.

## Validation

Run `python3 tests/ble_hid/run.py`. Sanitized production regressions run with
15/32 bond limits and Classic BTDM, including name/rollback coverage and 100000
lifecycle cycles per build. Durable cases cover replay/reboot/order, replaced
and removed identities, unknown legacy fingerprints, stale requests, copied
request storage, connections arriving during commit, open/set/commit/readback
failures, commit failure with and without actual persistence, old-record
recovery, CRC corruption and future schema rejection. No real device or NVS
partition is touched. Radio/private-host behavior still needs later authorized
physical qualification.
