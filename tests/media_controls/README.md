# Native media product controls

Run `python3 tests/media_controls/run.py`. Twenty-seven isolated cases compile the real
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

USB/Bluetooth selection is admitted from the Audio workspace and connection
panel only, after all jobs, cleanup and acknowledged hardware stop permit it.
Bluetooth preserves an already usable selected profile, otherwise selecting a
negotiated HFP or a playable A2DP profile before an unconnected supported
profile. Mic requires actual HFP capture capability; Speaker may use A2DP.
A live role may select an already negotiated profile for the same observed
Bluetooth peer only after the previous owner is stopped; it never auto-connects
or implicitly changes peers. These controls submit to
the existing audio owner; they do not alter HID selection. Applied runtime
route observations establish the displayed connection, independently of host
stream/link readiness. Tests cover pending and uncertain outcomes, stale and
invalid requests, absent capabilities, unavailable snapshots, privacy and stop
barriers, and an unconnected selected transport that cannot start either role.

One owning admission function returns each role's semantic `start_block`;
`can_start` is derived only from `ALLOWED`. The reason cannot claim readiness
when a separate boolean rejects it. Gate tests cover unknown snapshots, clock,
lock/screen, pending results, quarantine, connection selection, actual route
support/availability/link, per-role host streaming, codec availability/format,
immutable transport identity, same-peer profile changes, cleanup and occupancy.
Codec format and cleanup checks fail closed before a live command is queued.
Mic and Speaker retain independent host-stream reasons; enabling a role,
unmuting it or changing volume does not manufacture an authorized live job.

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
