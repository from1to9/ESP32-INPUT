# Remote media adapter checks

Run `python3 tests/remote_media/run.py` from the repository root. The suite uses
the actual adapter, canonical operation registry, argument schemas, JSON parser
and media helpers with explicit coordinator/runtime boundary ports. Compilation
uses strict C11 warnings, ASan and fail-fast UBSan.

Coverage includes authenticated identities and scopes, immutable retries,
management/audio revisions, reviewed capture and background grants, bounded
profiles, first-frame evidence, physical mute acknowledgement and completion
backpressure. Input lease release, expiry, override and conflict preserve
independent recording; device authorization revocation cancels its owner.

Private reads enforce operation scope and exact owner session. Speech candidates
remain private, require the shared input context for confirmation and never
send HID. Metadata delegates to the owner-scoped asynchronous runtime; binary
content uses the separate authenticated HTTP download port rather than JSON.
Live local-route capture and playback use the bound WebSocket media port.
Private session reads expose the media job ID, audio and consumer generations,
and exact PCM profile needed to construct ACM1 frames.

Source and binary hashes and logs are saved under `.agent/local/remote-media/`.
The tests perform no device writes, pairing, serial or hardware access.
