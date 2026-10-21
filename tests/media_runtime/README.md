# Media runtime composition checks

Run `python3 tests/media_runtime/run.py` from the repository root. The suite
compiles the actual firmware composition with strict C11 warnings, ASan and
fail-fast UBSan. Platform, audio-worker and Bluetooth boundaries are explicit
test ports; the production runtime, admission, grant and query logic are used.

Coverage includes immutable local and authenticated remote grants, independent
result ownership, execution-time revisions, speech context cancellation without
stopping independent media, bounded four-frame playback catch-up, and exact
transport binding forwarding. Remote cancellation is serialized through the
same audio result owner and retains the original consumer identity during
recording finalization; an irreversible commit is not reported as cancelled.

Download tests exercise asynchronous metadata/read/metadata sequencing,
principal and object authority, revision changes, expiry, clock rollback and
session revocation immediately before publication. Denied content is cleared.
Authority callbacks assert that the composition lock is not held.

`test_network_composition.c` additionally links the actual threaded audio
worker and portable engine to the actual runtime and ACM1 codec. Its HTTP
queue, codec, renderer acknowledgement, RTOS and unused SDK services are
explicit boundaries. Initialization must register the real server media port.
The test checks microphone-to-ACM1 samples, ACM1-to-speaker samples, queue-full
retry without sequence advancement, duplicate rejection, owner scopes, grant
and session revocation, and independent duplex cancellation. Replacement
capture starts at sequence one even while playback remains active, and an old
send failure cannot revoke the replacement consumer.

Channel-lifetime coverage pins the actual server generation at admission and
checks close before START dispatch, close with no queued PCM, immediate
same-session replacement and delayed old close/failure callbacks. File-only
recording, HTTP metadata/content access and remote USB/HFP authority survive
WSS loss. The server generation cache is also rechecked continuously, so a
missed close notification cannot let an old job resume on a replacement.

Source and binary hashes and build/test logs are saved under
`.agent/local/media-runtime/`. These checks perform no device access and do not
establish physical timing or host interoperability.
