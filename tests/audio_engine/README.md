# Native audio checks

Run `python3 tests/audio_engine/run.py` after preparing the repository's pinned
ESP-IDF and CMake tools. It performs no device, USB, Bluetooth, serial or flash
access. Every synthetic file stays in the selected local evidence directory.

- `test_media.c`: exact format budgets, copied ring ownership, strict ordering,
  stale token rejection, overflow, erasure, integer limits and canonical WAV.
- `test_engine.c`: actual manager admission, indication, authority races,
  independent fan-out, duplex/mute/stop, bounded lifetime, off-state settings,
  playback receipts, recording ACL/lifecycle and uncertain-state reconciliation.
  Physical mute acknowledgement is separate from logical gating; playback
  unbinding cannot defer the hardware mute or starve independent capture.
- `test_store.c`: actual PSA AES-GCM/SHA256 store with an explicit flash boundary;
  synchronous/asynchronous callbacks, roundtrip, ciphertext privacy, recovery,
  torn writes, corruption, durable deletion and finalization/read cancellation.
- `test_recording_composition.c`: actual engine plus actual encrypted store,
  clock-paced capture, periodic checkpoints, finalization, content read, stored
  playback, delete acknowledgement and restart.
- `test_app_audio.c`: actual application worker on host threads, asynchronous
  codec completion notifications, 100 Hz fallback tick, sustained 10 ms PCM,
  live setting serialization, consumer binding, bounded ingress credits and
  safety shutdown with retained query payload invalidation.
- `test_app_audio_startup.c`: target-like four-byte generic heap alignment,
  explicit owner/engine alignment, allocation/task failures, rejected arena
  cleanup and retry, and valid pending/faulted codec initialization. Unknown
  snapshots remain unavailable; no poisoned stack view becomes known. A copied
  completion callback remains safe after owner teardown and task deletion,
  including replacement startup before and after task-handle publication.
- `test_app_recording_store.c`: actual firmware storage composition and real
  encrypted backend; explicit byte ownership, control-NVS custody, key/nonce
  atomicity, unknown commit handling and reboot. Only platform boundaries are
  modeled; no test implementation replaces production media/storage behavior.

The runner records exact source and binary hashes, strict build output and test
logs. UBSan aborts on the first finding. It independently reads the synthetic
WAV with Python's standard library. Host evidence does not establish hardware
qualification or real-time scheduling guarantees.
