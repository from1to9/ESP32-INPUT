# Native media product controls

Run `python3 tests/media_controls/run.py`. Eighteen isolated cases compile the real
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
