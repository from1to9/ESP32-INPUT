# Native media product controls

Run `python3 tests/media_controls/run.py`. Twenty isolated cases compile the real
`app_media_controls.c` and audio format arithmetic with strict warnings,
AddressSanitizer and UndefinedBehaviorSanitizer. Cached codec, store, runtime and
Bluetooth interfaces are substituted at their public boundaries; no production
policy branch is replaced.

Coverage includes immutable recording review and actual chunk-overhead quota
arithmetic, stale revision rejection, uncertain outcomes without retry, exact
recording object/job identities, cursor termination, explicit storage preparation,
one-time speech preview/rejection, bounded live tasks, saved Classic peer and
bond revision, per-role mute acknowledgement, and retained microphone warning
through unavailable snapshots until acknowledged hardware stop.

Explicit Connect voice uses the transport's bounded saved-peer request queue.
Tests hold the first SLC request pending, complete its exact token and peer, and
then require a distinct OPEN_VOICE token before accepting actual negotiated SCO.
Existing SLC uses only OPEN_VOICE. Neither phase creates a live consumer or
unmutes the microphone. Failures, replaced tokens/peers, changed bond catalogues,
lock, timeout and unavailable snapshots stop sequencing without automatic retry.
The request seam records public queue calls; no transport implementation is
substituted inside the production controls algorithm.

These checks prove local request/admission behavior. They do not prove actual
audio capture, transport readiness, flash publication, physical mute or host
compatibility. Those remain the corresponding runtime/driver and device gates.

The coordinator owns a distinct fixed PSRAM polling workspace for incoming
runtime/store/Classic bond snapshots and query records. Old and new observations
remain separate until comparisons complete; no scratch escapes to a callback
or nested command. Temporary speech, recording and peer metadata is cleared
before return. The target poll frame is 656 bytes (previously 5,072); its new
4,488-byte workspace has no heap fallback. Bond lists use the copy-out API.

Regressions cover changed/unchanged revisions across repeated snapshots,
unavailable getters that leave their output untouched, capture-warning
retention, full bond capacity, and a failed record copy that wrote partial data.
Every poll path exercised by the suite verifies scratch cleanup; a failed query
cannot publish its temporary rows or automatically retry. Stack measurements
are compiler evidence, not device high-water measurements.
