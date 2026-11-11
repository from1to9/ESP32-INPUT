# Audio and recording API

The [operation catalog](../api/control-v1.json) defines the available request
shapes, scopes and configuration metadata. Read `/api/v1/audio/capabilities`
and `/api/v1/audio/state` to check the actual route, format, grant and job
before requesting audio.

A route selection does not start capture. A microphone job requires current
permission, a usable input route and the applicable local capture indication.
Mute and stop have their own acknowledgements. Follow the job state through
preparation, first frame, running and terminal result. Codec or socket
submission alone does not establish audible playback or delivered capture.

Live PCM uses the `ACM1` frame format on an authenticated channel. Its
header identifies the job, generations, sample offset and format. Channel
closure revokes its network consumer. See the
[media transport contract](../bridge/README.md#http-and-media) for frame bounds
and timing.

Local recordings use bounded onboard storage. Use the recording metadata to
check ownership, completion, retention and export eligibility. The content
route requires read and export scope, the owner and an exportable completed
recording. Range requests use the exact strong ETag in `If-Match`; see the
[companion guide](../bridge/README.md#http-and-media) for supported range forms.
Keep captured audio and trust material out of Git.

See [Bluetooth audio](bt-audio.md) for Classic profile and stream readiness,
and [validation](validation.md) for device evidence.
