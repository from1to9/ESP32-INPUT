# Bluetooth peer observations and single removal

Coordinator/VIEW adapters read `ble_hid_bond_list()`,
`ble_hid_target_snapshot()`, `ble_hid_snapshot()` and `bt_audio_bond_list()`.
These bounded cached getters do not perform SDK I/O or expose exported link
keys, credentials or fabricated host names. Unknown/updating/error states are
unavailable evidence, never an empty successful list. A combined JSON encoder
must reject an oversized complete response instead of truncating saved rows.

The BLE array is in the actual preference-derived order. Use its same-lock
`preferences_revision` to reconcile a separate preference snapshot. Fingerprints
are non-secret 32-byte bond IDs only when `bond_id_known` is true. A connected
row requires all of `bluetooth.connected`, `bonds.current_match_known`, and
`row.is_current`; a stale row flag alone is insufficient. Unknown address type
must remain unknown. Classic rows use actual saved BR/EDR addresses, separate
HFP/A2DP current matches and explicitly unavailable names; HID identity cannot
stand in for an audio peer.

`ble_hid_forget_bond_request` accepts the current HID generation, exact bond-list
revision, and complete matching fingerprint key. It requires a disconnected,
released, healthy manager with no conflicting pairing/name/preference/bond
operation. ESP_OK acknowledges copied admission only. Poll the exact request ID
with `ble_hid_bond_forget_snapshot`. The existing worker closes admission,
stops advertising, rereads the actual SDK identity, submits exactly one LE
address removal, verifies SDK completion and absence, and checks all remaining
saved identities against its baseline. Only the removed fingerprint's CCCD
record is changed, with commit/readback verification. No operation aliases to
forget-all or silently selects another host.

Pending removal can be cancelled by exact request ID; CANCELLED proves no
removal was submitted. Once submission starts, cancellation returns
ESP_ERR_NOT_SUPPORTED (too late). Missing callbacks, unexpected readback or
partial cleanup are UNCERTAIN, never a fabricated rollback/success, and block
admission until recovery. Stale/foreign callbacks cannot acknowledge a different
request. Successful removal does not open a pairing window. A removed selected
preference remains durably blocked until explicit clear/replacement.

Factory/remote forget-all uses `ble_hid_forget_bonds_without_pairing`, sharing
the existing worker and completion state while keeping the window closed.
After verified zero bonds, submit empty order plus `selected=false` through
`ble_hid_target_preferences_request` against the fresh bond/preference revisions.
Wait for APPLIED before declaring reset complete. That durable empty record
clears orphan preferences and prevents automatic initial pairing on the next
boot. A later explicit local pairing action remains available. Classic bonds
are preserved unless separately authorized.

## Local Audio Forget

Audio Forget uses the selected Classic address and an opaque key fingerprint
captured by the local confirmation. The Classic scan coordinator requires an
inactive matching BR/EDR record and prevents new Classic pages during the
request. BTC checks the same persisted identity and saves only the removal of
its Classic fields. BTU then clears that exact RAM key and its Classic security
flags. The LE handle, keys and shared bond state remain intact even at the same
address. The Audio manager reports success after the refreshed list omits the
selected address.

This operation requires the pinned S31 controller without a link-key store
and an existing single-block Bluedroid config whose current serialization fits
one block. The existing reader treats a first blob shorter than 1536 bytes as
complete, so one checked NVS update carries all effective data and no separate
continuation index. A full-sized first block, a larger cache, or an unknown
storage layout fails before key deletion. A save or post-commit verification
failure leaves removal unconfirmed and blocks new Classic commands until
restart. No input disconnect, LE deletion, global clear or pairing window is
part of the operation.
