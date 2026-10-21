# Audio and recording ownership

`audio_engine.c` owns the codec resource and all capture/playback jobs.
`audio_media.c` supplies exact PCM formats, copied bounded rings and canonical
PCM16 WAV headers. `audio_recording.c` owns recording admission, immutable bounds,
ACLs and lifecycle. `audio_store.c` is the actual encrypted object store, using
the pinned SDK's PSA AES-256-GCM and incremental SHA-256 implementations.

The firmware boundary is `app_audio.c`: one worker, one copied ordinary command
and retained result, a separate atomic safety latch, four copied transport ingress
frames and PSRAM-only media state. It registers codec/storage completion wakeups
and performs at most four progress passes per wake with a 4 ms cooperative budget.
Each underlying I/O callback must still satisfy its 20 ms contract. This budget
cannot interrupt a callback; physical timing remains a qualification item.

`app_recording_store.c` composes the named internal recordings partition, explicit
byte-volume ownership, encrypted-store mount/recovery, trusted time and software
key custody in the dedicated `control` NVS partition. Initialization allocates a
stable endpoint; it does not provision or format. Its service callback executes
in the audio worker, separate from the UI and input coordinator. No production
path substitutes a test store or reports success for absent storage.

## Media invariants

- All starts have immutable request, principal, owner-session and consent
  identities. Request retries cannot extend duration or bytes. A microphone
  start requires explicit activation and a current grant.
- Default state is off and muted. A fresh readable indicator acknowledgement for
  the exact audio generation precedes unmute. Lock, privacy, maintenance,
  stale indication and revoked authority close software egress before cleanup.
- First complete frame sequence is **1**, sample offset **0**. Every subsequent
  frame must match exact audio/consumer generation, actual peer/stream/link
  token, format, sequence and sample offset. There is no resampling or frame
  relabeling. Full buffers fail the affected consumer; they never overwrite.
- Same-format duplex uses a single prepared codec session only when the actual
  codec and route support both directions. Codec mailbox requests are serialized.
  Microphone and speaker stop/mute independently; unmute cannot revive a cancelled
  job. Explicit muted preparation can be reused by a later authorized start.
- Remote duplex requires `route.bind_playback`: resource preparation does not
  authorize a playback consumer. Binding flushes old input, installs the exact
  consumer generation and starts its sequence at 1. Old callback generations
  remain old. The opposite capture direction is preserved. Ports without this
  callback use the legacy zero-generation ingress contract; they must not claim
  safe independent replacement of a running remote playback consumer.
- Live capture and stream playback admit explicitly bounded sessions up to
  28,800 seconds and 8 GiB. Recognition is limited to 8 seconds; recording to
  1,800 seconds; stored-file playback to 3,600 seconds. Actual grants may be
  stricter. There is no automatic resume.
- Capture fan-out is opt-in and bounded to live, recording and recognition
  consumers. A pending physical read does not prevent draining their independent
  queues. File finalization does not pause another active capture consumer.
- Playback consumption receipts occur only after an exact full codec write,
  with current authority and link identity. Enqueueing is not consumption.

## Persistent format and failure behavior

The owned data region has 4096-byte pages. Two authenticated superblocks precede
an index of two alternating metadata pages per object. Each object reserves its
entire bounded extent before capture. Up to 64 index slots are supported, subject
to the actual geometry/quota reserve; the internal partition normally uses 8.
Each terminal or live object remains charged for its reserved extent until
explicit deletion or authorized retention expiry. No unrelated object is evicted.

Each page contains a 64-byte authenticated header, a 12-byte nonce, 4000 encrypted
payload bytes, a 16-byte GCM tag and four zero pad bytes. The header binds page
kind, slot, sequence, object ID, key ID, logical PCM offset and payload length.
Serialization is explicit little endian, never a native C structure. Data pages
are immutable once acknowledged. A checkpoint flushes an incomplete data page,
then commits the exact synced prefix in an alternate metadata slot. This may use
space earlier than the maximum PCM estimate; actual extent exhaustion returns
quota/partial status rather than exceeding the reservation.

Completed metadata is published only after canonical WAV validation, authenticated
page reads, exact extent verification, actual plaintext WAV SHA-256, synchronization
and readback. Partial/error objects are metadata-only. Downloads require the
original owner and `owner_download`; local playback applies the same ownership,
expiry, exact-length and integrity checks. Downloads and playback recheck authority
after source I/O and before exposure or codec submission.

Deletion first durably commits access revocation. Logical extent reclamation then
completes; this is not a claim of physical secure erasure. Retention expiry uses
trusted absolute UTC and bounded background journal work. Metadata records a
last-observed UTC watermark so a restart cannot accept time earlier than known
durable evidence. Unknown/backwards time keeps content and new writes closed.

Mount authenticates metadata, rejects overlapping extents, verifies committed
page chains and hashes, and recovers unfinished objects as partial/error. Capture
never resumes. Unknown or torn metadata is quarantined, preserving bytes for
reconciliation; the backend never falls back to an older successful deletion.
Uncheckpointed data is not promoted to durable content. Explicit recovery can
reconcile an actual terminal object or authoritative absence after a timeout,
without opening the microphone. Read cancellation retires the outstanding byte
request before clearing its private buffer, so it cannot strand the store mailbox.

A finalization cancellation suppresses publication before submission of the
terminal page program. Any in-flight source read/erase is retired first. Once the
terminal page program has been submitted, cancellation returns `AE_TOO_LATE`;
it never acknowledges cancellation of an uncertain atomic commit. Discard also
requires delete authority and uses durable access revocation.

## Key custody and provisioning

The firmware stores the volume identity, 256-bit key and next nonce in one atomic
NVS value in `control/record_audio`. The volume-specific key is generated only
after an explicit provision request and a complete all-erased scan of an already
owned data region. Keys and their counters are never reset independently.

The backend durably leases 32,768 nonces at a time before use. It does not write
NVS per frame or per progress update. Ambiguous lease commits close writes; the
range is not used and reboot/recovery reads the durable counter. A failed data
write is not retried as a different successful object. Temporary plaintext/key
buffers are cleared at their owning boundaries.

This is **software custody**, with no product API for key export. It does not
claim physical tamper resistance, and does not provision eFuses or transparent
flash encryption. Unknown partitions, keys or ownership markers are never erased
or silently adopted. Internal records and owned NAND are explicit storage
profiles; neither is an automatic fallback for the other.

## Bounds and evidence

Mute state distinguishes the immediate logical gate from the last physically
applied value, a matching confirmed acknowledgement and a pending operation.
Until a physical stop is confirmed, privacy indication remains required.
Stopping playback closes the hardware role before waiting for transport
unbinding; independent capture continues while that acknowledgement is pending.

Transport playback binds the exact route token, audio generation and consumer
generation. A replacement consumer starts at sequence one and sample offset
zero; queued frames from the old binding are never relabelled. The application
runtime observes four-slot ingress capacity before removing transport frames.
Codec and storage completion callbacks wake the same bounded audio worker and
must be invoked outside driver locks.

The runtime registers the control server's live media callbacks during
initialization. A remote live job on the local codec route uses ACM1 WebSocket
frames: 64 bytes of big-endian metadata followed by at most 1920 bytes of
interleaved signed PCM16 little-endian samples. The protected audio-session
read supplies `media_job_id`, `audio_generation`, `consumer_generation`,
`sample_rate_hz`, `channels` and `frame_ms`. Each consumer begins at sequence
one and sample offset zero, including replacement capture during duplex audio.
The actual WSS channel generation is fixed at admission and carried through
ingress, egress and cancellation. Closing that channel cancels its pending and
active live jobs even without queued PCM; a same-session replacement requires
new admission. HTTP management, file recording and BT/USB consumers retain
their independent lifetime.

Capture copies into the server's two-packet queue. Acceptance is a queue
receipt; the HTTP owner rechecks current identity, job, generation and capture
grant before the actual send. A failed send revokes only that consumer. A full
queue retains the exact frame for retry within the bounded capture ring.
Playback validates the same binding and copies into the four-frame audio
ingress. Full ingress returns retryable backpressure without advancing sequence
or triggering an independent consumer's safety stop. All physical consumption
remains on the single audio worker.

Network framing uses fixed PSRAM scratch (one 1984-byte capture packet and one
native frame); HTTP callbacks never borrow the coordinator's memory. The real
TLS/WebSocket server owns sockets, queue deadlines and final buffer erasure.

The firmware composition retains authenticated remote identity and immutable
grants through the common audio owner. Independent media does not inherit an
input-lease release. Owner-scoped download queries recheck object revision,
session, expiry and trusted time before publishing copied bytes. See the
[runtime checks](../../tests/media_runtime/README.md) and
[remote adapter checks](../../tests/remote_media/README.md) for their evidence.

| Allocation or limit | Bound |
| --- | --- |
| PCM frame payload | 1920 bytes |
| Native 64-bit frame structure | 2008 bytes |
| Capture queues | 3 x 8 frames |
| Playback queue | 16 frames |
| Cross-thread transport ingress | 4 frames |
| File read result | 8192 bytes |
| Metadata list result | 16 objects plus continuation cursor |
| Internal storage quota | 64 to 512 KiB; 4096-byte aligned |
| Owned NAND quota | 1 to 32 MiB; 4096-byte aligned |
| Retention | 3600 to 2592000 seconds |
| Engine memory | `ae_engine_bytes()`, caller-provided PSRAM |
| Store memory | `ae_store_bytes()`, caller-provided PSRAM |
| Audio worker stack | 8192 bytes, separate from PCM state |

The native runner reports current structure/allocation sizes. PSA owns one AES
key and one incremental hash operation; its target heap overhead and the board's
DMA/driver/task resources must be included in the combined firmware ledger.
Native structure sizes are evidence for that ABI, not measurements of target RAM.
No internal-memory fallback is allowed for the engine, store or app media arena.

Run `python3 tests/audio_engine/run.py` from the repository root. The runner uses
strict warnings, ASan and fail-fast UBSan; builds real crypto from the pinned SDK;
checks manager/media/store/runtime composition with explicit hardware, RTOS,
partition and NVS boundary models; and writes source/binary hashes and synthetic
WAV evidence under `.agent/local/audio-engine/`. The application worker test uses
real host threads and a modeled 100 Hz fallback tick, with completion notifications.

Physical acoustic quality, actual stop latency, flash power-cut behavior, long
run thermal/CPU/RAM behavior and host interoperability remain unqualified. This
is a documented device-testing deferral, not a reason to disable implemented
routes or a source-publication prerequisite.
