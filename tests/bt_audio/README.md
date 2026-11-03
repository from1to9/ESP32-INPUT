# Bluetooth audio native regressions

Run `python3 tests/bt_audio/run.py` from the repository root. The suite compiles
the production transport against deterministic SDK boundary substitutes with
strict C11 diagnostics, address sanitizer and undefined behavior sanitizer.

It checks actual asynchronous initialization versus request acceptance,
partial-init/allocation/event failure, missing callbacks, cancellation and
quarantine of in-flight commands, submission failure, CVSD/mSBC/unsupported
LC3, bounded PCM/age/mute gates, queue overflow and underflow, full-queue data
notifications, little-endian signed samples, stereo downmix, variable HFP chunk
assembly, actual route tokens, same-format reconnect/suspend invalidation,
old-stop isolation, foreign handles and the unsupported BLE-only build.
Classic saved-host cases cover actual bounded addresses with unavailable names,
stale list revisions, absent peers, retained rows after read failure, queued
refresh and cancellation of a saved-host command overtaken before submission.
It also covers bounded Classic pairing, fresh shared-capacity exhaustion,
cross-profile admission denial, window expiry, legacy PIN/passkey rejection,
retention of an uncertain handshake guard, discovery-verification timeout and
the unavailable unpatched-SDK build.
The engine/transport composition uses both production owners and SDK/codec
boundary models. Pairing before start or before preparation must end the start
as busy without codec activation or quarantine, and a fresh request after
closure must run. It also checks prepared-audio rejection of pairing and the
capacity-query/preparation interleaving.
Session loss and privacy revocation during the same rejection preserve their
actual job errors, leave hardware off and permit a fresh authorized request.
`playback-binding` additionally verifies immutable consumer stamping, partial
and queued RX flush, sequence/offset reset, idempotent binding, old-close and
closed-consumer rejection, capture preservation and a callback interrupted
after entry but before commit. The clock-boundary hook models a concurrent
manager rebind; production callbacks never invoke the manager.

After SDK setup, run `python3 tests/bt_audio/test_sdk_admission.py` to exercise
the actual patched SDK dispatch function: Just Works and ordinary confirmation
must reach admission; allocation/dispatch failure must never auto-accept.
`python3 tests/toolchain/test_idf_patches.py` separately validates reproducible
patch provenance and preservation of unrelated SDK/BSP work.

Substitutes model only the hardware/API boundary. They do not model radio
encoding, SDK queue scheduling, privacy resolution or actual physical output.
The pinned SDK target compile checks real API names/types; the combined image
and later authorized hardware qualification remain separate checks.
