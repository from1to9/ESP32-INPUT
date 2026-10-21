# Audio and recording API plan

Status: proposed v1, planning only, 2026-10-21. No audio API, recording driver or
recording authorization is implemented by this document. This module belongs
to the complete device API plan and shares the [remote protocol](remote-control.md),
[product requirements](requirements.md) and [coordinator architecture](architecture.md).
The [function matrix](api-function-matrix.md) owns operation IDs, field names,
scopes and units; [shared configuration](configuration.md) owns transactions.
This document supplies their audio resource, privacy and lifecycle semantics.
Wi-Fi management is independent of the selected Bluetooth/USB input transport.
The existing provisioning HTTP form must never acquire these capabilities.

## 1. Evidence and capability gates

| Inspected source | What exists | What it does not prove |
| --- | --- | --- |
| BSP [audio header](../third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/include/bsp/audio.h) and [implementation](../third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/onboard/audio.c) | ES8311 speaker/microphone handles share I2S0 and one data interface. Default bus framing is 48 kHz, 16-bit, two slots. Initialization enables RX/TX; subsequent `bsp_audio_init()` calls reuse the interface | Application audio ownership, independently configurable simultaneous endpoints, measured microphone/speaker operation or audio quality |
| Pinned `esp_codec_dev` interfaces/ES8311 implementation | Open/close, read/write, output volume/mute and input gain/mute. ES8311 input gain selects 0, 6, 12, 18, 24, 30, 36 or 42 dB | Bounded stop latency: the current I2S data backend uses a 1,000 ms default I/O wait. Its single-consumer input mirror can overwrite old data and is not a recording fan-out queue |
| BSP [NAND interface](../third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/include/bsp/nand_flash.h) and [implementation](../third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/onboard/nand_flash.c) | SPI NAND initialization, handle and sync/power-save hooks; defaults include 40 MHz, queue 10 and 4 KiB transfers. The managed driver supplies Dhara-backed logical pages, geometry, sync and GC APIs | Actual installed capacity, unused ownership, a mounted application filesystem, power-loss durability or endurance |
| [Board contract](../firmware/main/board_port.h), [partition table](../firmware/partitions.csv), firmware configuration | No audio/storage capabilities are currently exported. Application flash contains NVS, PHY and one factory image; no recording partition. S31 SDK declares Classic hardware support, while this image sets `CONFIG_BT_CLASSIC_ENABLED=n` | HFP/A2DP availability in this build; USB UAC; any recording store. Other ESP32 variants may lack Classic or USB hardware entirely |

Until implemented and qualified, microphone, speaker, recording, recognition,
HFP, A2DP and UAC operations advertise `implementation=absent`,
`available=false`, `reason=not_implemented`. Later report hardware probe,
driver state, implementation, qualification and current resource availability
separately. Compiled examples or a successful handle allocation cannot set
qualification to passed. Capability schemas list supported formats and exact
board/firmware/host/profile combinations; clients cannot set qualification flags.

## 2. Ownership, authorization and privacy

One audio manager owns codec handles, I2S, routes, capture/playback and mute.
One storage manager owns recording objects and NAND. Network/UI/MCP submit the
same typed coordinator intents; no callback opens another codec or writes files.
Keep the BSP's two handles coordinated: they address one physical codec, and a
second initialization/format change can affect the first. Establish bounded
I/O and cancellation before advertising capture readiness; the existing
1,000 ms backend wait cannot be presented as a 100 ms stop guarantee.

Baseline TLS, Origin/Host, CSRF, logical-session sequence and cross-transport
request-deduplication rules apply to POST and optional command WSS alike. Audio
and recording use management generation, audio generation and expected resource
revision. They require neither a HID lease nor a selected HID target, input
epoch or `input.control`. Losing input ownership alone does not stop an
independent media job. Mode selection remains the separate `mode.select`
operation with its input guards; opening Audio never activates capture.

| Canonical authority | Audio use |
| --- | --- |
| `audio.control` | Route, role power/mute and corresponding configuration; enabling prepares a muted route and cannot grant capture consent |
| `audio.capture` | Explicit capture/recognition and microphone operations in the matrix; each capture consumer also needs P consent |
| `audio.playback` | Playback and speaker operations; source recordings additionally require `recording.read` and the object's ACL |
| `recording.control` | Start/stop/cancel an owned recording and its internal capture consumer, with P consent; no extra HID authority or standalone live-capture scope is inferred |
| `recording.configure` | Recording defaults through the common configuration family |
| `recording.read`, `recording.export`, `recording.delete` | Separate artifact read, future destination export and explicit deletion authority, each constrained by the object's ACL and policy |
| `capture.consent`, `admin.privacy` | Separately enrolled bounded capture authority; protected privacy-policy administration. Ordinary input/audio/admin authority does not supply consent |

There are no `audio.configure`, `audio.stream` or `recordings.*` scope aliases.
Ordinary `view` grants only redacted state, not samples, recordings, transcripts,
filenames, other principals' session details or private level meters. Opening a
capture media channel needs the source job's scope, consent and destination
permission; an opaque ID or stream ticket never expands them.

**Standing recording authorization:** an owner may explicitly enroll a scoped
`capture.consent` grant covering principal, local-microphone source, purpose,
live/file destinations, finite duration/bytes, retention/export policy and expiry.
Authenticated starts within that grant need no repeated local confirmation.
Fresh local approval remains an alternative for one exact bounded job/session.
Remote grant updates may narrow/revoke rights within the enrolled ceiling;
expanding it or adding another principal requires the owner's enrollment flow.
The common `capture.consent_policy` and protected principal/policy records own
grant administration; there is no parallel audio-authorization setter. A saved
grant is permission, never a saved active/unmuted state. The accepted immutable
job copies consent identity/revision and its source/destination/retention/bounds;
later preference changes cannot widen it. Recheck mutable revocation/expiry at
activation and while running; every consumer has its own applicable consent.

Every start still requires an unlocked device, no physical privacy latch,
available drivers, quota and a visible capture indicator with local Stop.
Microphone capture keeps a readable on-screen indicator active and prevents idle
blanking; only a qualified readable dim level is allowed. Initial v1 has no
screen-off recording mode or substitute LED. Missing/stalled indication rejects
or stops capture. Local mute, physical
lock/Stop, grant revocation, maintenance and faults close the capture/egress gate
immediately, invalidate the audio generation and stop every microphone consumer.
Hardware mute/off remains `stopping` or `unknown` until acknowledged; a failed
stop must not extinguish the privacy warning or claim microphone-off.

Default `disconnect_policy=stop` binds media to its authenticated owner session
and active media/control channel, with a finite session liveness deadline. A
single completed POST does not terminate that logical session. Channel/session
loss closes live egress immediately on detection and stops session-bound capture,
recording and playback; detection never claims knowledge before the common
bounded liveness timeout. Changing POST/WSS transport cannot renew the job or
replay it.

Optional `continue_local` is file-only. It needs a **distinct enrolled background
grant**, named by `background_grant_id`, in addition to the source consent. That
grant is a separate `capture.consent` delegation with purpose
`background_recording`, not another HID lease or an implicit background scope.
It binds principal, local-file destination, source, duration/bytes, retention,
expiry and local-stop policy. The device accepts ownership only after successful
bounded recording start and reports that transfer explicitly. The job may then
finish after owner channel/session or Wi-Fi loss, within both grants. Its
original input lease has no bearing on that lifetime. Live egress always stops
on owner channel/session loss; `continue_local` never preserves it. Record plus
live fan-out is unavailable initially and, if later qualified, requires separate
live authority and gates.

Lock, privacy Stop, grant/principal revocation, maintenance, indicator failure,
faults and duration/byte ceilings stop either policy. Narrowing/revoking a grant
that no longer covers a running job stops it; no reconnect or preference save
resumes it. The original retained artifact policy remains enforceable after job
termination; withdrawing capture consent never silently grants artifact access.
Boot never resumes capture, playback, a call or recording. API stop/mute safety
commands require the relevant authenticated audio/recording authority and job
ownership or separately enrolled safety authority; no active HID lease is
required and viewers cannot invoke them. The original principal may stop its
background job from a fresh authenticated session without resuming live media.
Local controls always retain priority.

## 3. Shared configuration schema

Use schema version 1 inside the common settings schema and the matrix's exact
names below. All preference changes use `config.stage/apply/cancel`; there is no
audio-specific patch/volume endpoint. Expand matrix slash shorthand into the
listed namespaced fields, not alternative wire names.
Reject unknown keys, non-finite values, invalid enums and a candidate that
exceeds capabilities or its grant; validate the complete patch before saving.
Expose desired, saved and applied revisions, pending keys and per-key errors.
Do not silently round codec gain, clamp volume or substitute an unsupported
format. Runtime active/mute state, sessions and queued media are never persisted.

| Canonical key | Initial/default plan | Type/range and application rule |
| --- | --- | --- |
| `audio.route`, `audio.profile` | Off; no active sample format | Qualified route/profile IDs. Candidate local capture profile is 16 kHz mono PCM16LE; local output is 48 kHz mono. No activation from saving a preference; `audio.control` |
| `audio.enabled` | false for each microphone/speaker role | Boolean role state, volatile; true prepares muted resources, false closes affected consumers. Runtime operations and configuration share one owner; `audio.control` |
| `audio.output_volume_percent` | Candidate local-profile default 30 | Integer 0..100 intersect qualified safe mapping; proposed initial safe ceiling 80 is profile metadata, not a new writable limit. Volume zero does not close the route; `audio.control` |
| `audio.microphone_gain_profile` | Candidate `fixed_18db` | Advertised IDs mapping exactly to ES8311 `{0,6,12,18,24,30,36,42}` dB. No silent rounding or assumed AGC; stopped/resource-transition apply; `audio.capture` |
| `audio.microphone_muted`, `audio.speaker_muted` | true / true | Volatile Boolean state. Unmute requires current driver acknowledgement and applicable consent; changing saved volume/gain never unmutes. `audio.control`; safety mute needs no HID lease |
| `audio.sample`, `audio.channels`, `audio.codec`, `audio.mix`, `audio.aec_profile` | No active format; discrete qualified profile choices | Profile metadata contains rate/encoding and compatible role/channel tuple. Candidate capture rates 8/16/48 kHz, one actual microphone channel, PCM16LE; output 16/44.1/48 kHz with explicit mono downmix for two-channel input. Half duplex initially; AEC/background coexistence remains unavailable until qualified. `audio.control` |
| `recording.format`, `recording.sample_profile` | No automatically selected recording format | Explicit supported IDs on start; candidate `wav_pcm_s16le` and `pcm16_mono_16k`. Saved defaults never activate a writer; `recording.configure` |
| `recording.duration_seconds` | Explicit start value; candidate profile default 300 only when chosen | Positive integer seconds, candidate range 1..1800, bounded by consent/profile. Wall-clock runtime ceiling, never zero/unlimited; `recording.configure` |
| `recording.byte_limit` | Explicit start value; candidate 16 MiB only when chosen | Integer bytes, candidate 64 KiB..32 MiB and within effective quota/consent. Counts stored media, framing, encryption and per-object metadata; positive and finite. `recording.configure` |
| `recording.storage` | `volatile`; persistent NAND requires explicit selection | Advertised IDs only. `volatile` permits bounded live capture but creates no persistent recording object; `recording.start` initially requires the provisioned `nand` store and rejects unavailable/volatile storage. No RAM-to-NAND fallback; `recording.configure` |
| `recording.retention` | No persistent retention until explicitly approved | Object `{policy:"expire_after",duration_seconds:N}`; candidate 3600..2592000 seconds, e.g. 604800 for seven days. No indefinite/manual retention in initial v1. The start and consent must approve the exact policy; `recording.configure` |
| `recording.export_policy` | `none` | Enum `none|owner_download` initially. Owner download is explicit consent for ACL-bound authenticated pull; arbitrary destination export remains unsupported. `recording.configure` |
| `capture.consent_policy` | Deny without explicit bounded authority | Protected enrolled grant/expiry/revocation metadata under `admin.privacy`; not a Boolean that removes consent, indicator or local-stop requirements |

Wire job limits use positive integer `duration_seconds` and `byte_limit`, with
SI seconds and bytes; no alternate `max_duration_ms`/`max_file_bytes` setters.
Candidate standalone capture is 1..300 seconds (profile default 60 if explicitly
selected), playback 1..3600 seconds (candidate default 600). Recording uses the
limits above. Timers convert seconds to checked monotonic deadlines once at
admission; sample offsets/counts are separate exact media positions.
Candidate standalone capture accepts an explicit 1 KiB..32 MiB `byte_limit`,
playback 1 KiB..64 MiB, narrowed by the negotiated profile and authority. They
bound total accepted media payload, not allocated memory or a recording store;
recording's stored-byte limit includes its separate finalization overhead.

Resource-profile metadata, not additional ordinary writable settings, bounds
frames at 10 or 20 ms and rings at 20..500 ms, frame-aligned and at most 32 KiB
per ring. Candidate store quota is min(32 MiB, verified usable bytes minus
reserve), rounded down to 4 KiB; reserve is max(4 MiB, 10% of usable capacity).
Less than 1 MiB effective quota disables recording. A later qualified quota
profile may select 1..128 MiB without exceeding verified space. Candidate object
limit is 32 with a compiled ceiling 64; sync interval is 1000 ms within a
qualified 250..5000 ms profile. No per-frame NVS commits, automatic quota-driven
oldest-file deletion or unsupported expansion of these resource ceilings.

Disabled/unavailable audio does not apply saved preferences to nonexistent
hardware. Rate/route/encoding changes require stopping affected sessions and
publishing a new audio generation; volume/mute must not restart capture.
Coalesce durable preference saves; transient slider changes remain RAM-only
until explicit save. Report uncertain storage rather than assuming rollback.

The proposed internal bus remains 48 kHz/16-bit/two-slot initially; selecting
16 kHz mono requires a tested extraction/resampler in the manager, not relabeling
bus bytes. An unsupported resampler/format is unavailable. These profile candidates
and bounds are design choices, not measured quality, loudness or latency claims.
Until qualification and shared schema generation, they remain unavailable
metadata, not writable settings.

## 4. Operation inventory and payloads

All mutations use **`POST /api/v1/commands`** or the identical authenticated
envelope over optional `/api/v1/ws`. Both enter one registry dispatcher and share
logical-session sequence, deduplication and results. Include boot/management
generation, `audio_generation` and expected resource revision; omit/null HID
lease/target/epoch fields. Starts finish admission within the common 2 s horizon,
then expose a separately bounded session/job. Duplicate envelopes, including a
POST/WSS retry of the same logical command, return the same job/object IDs and
never restart, extend duration, increase byte limits or renew consent.

| Operation / read route | Required payload and result |
| --- | --- |
| `mode.select` (input owner) | Foreground `mouse|keyboard|audio` under its input guards. This is independent of audio job authority and cannot satisfy consent or activate audio |
| `audio.route.select`, `audio.link.connect/disconnect` | Qualified route/peer/profile IDs; candidate routes `local|hfp_hf|a2dp_sink|usb_uac`. `audio.control`, resource revision and conflict handling. Route selection never changes HID mode/target, unmutes or fabricates a link; connect/disconnect reports actual link/cleanup independently |
| `audio.enable.set`, `audio.microphone.enable`, `audio.speaker.enable` | Explicit `enabled` Boolean; general form also names `role:microphone|speaker`. Role-specific matrix forms normalize to the same manager transition, with their exact registered `audio.capture`/`audio.playback` authority; the general form uses `audio.control`. Enable prepares muted resources; disable closes the role and its consumers. No hidden capture from enable |
| `audio.mute.set`, `audio.microphone.mute`, `audio.speaker.mute` | Explicit `muted` Boolean and role in the general form. One mute owner and revision across the matrix's general/role-specific forms; general scope `audio.control`, specific microphone/speaker scope `audio.capture`/`audio.playback`. Mute is an authorized safety action; unmute needs an explicitly bounded consumer and P consent for microphone capture. A driver failure never reports applied mute |
| `config.stage/apply/cancel` | Only preference write family, with canonical keys, field scopes and expected settings revision. Host OS/AVRCP volume is a separate future capability; output-volume preference is not an OS volume acknowledgement |
| `audio.capture.start/stop` | `audio.capture` and P. Start: source `local_microphone`, qualified `sample_profile`, positive `duration_seconds` and `byte_limit`, `consent_id`, `activate_microphone:false` by default. Explicit true may enable/unmute within that consent. Result: audio-session/job ID, actual profile, expiry and scoped media ticket. Stop names owned session/generation; no HID lease |
| `audio.playback.start/stop` | `audio.playback`; source `live_stream` or ACL-owned `recording_id`, qualified sample/output profile and finite duration/byte bounds. No arbitrary URL fetch. Start returns session/media channel; stop discards pending data and acknowledges output mute/drain separately |
| `audio.playback.pause/resume/seek` | **Unsupported in initial v1**, with operation metadata `supported=false`, `reason=not_implemented`. No alias to stop/start and no seek-position claim. A later implementation must define position, buffer ownership, deadlines and exact seekable assets before advertising these operations |
| `recording.start/stop` | `recording.control` and P; explicit source/profile/store, limits, retention/export/disconnect policy and consent. Return recording object ID, job ID and audio-session ID. Stop closes this consumer first, then reports bounded finalization; another separately qualified consumer remains governed by its own consent |
| `recording.cancel` | Same owned job authority, expected job revision and mandatory `disposition:keep_partial|discard`. Close capture first. Keep retains only an actual verified partial prefix; discard additionally requires `recording.delete` and reports cleanup separately. No success alias to stop/delete; section 5 defines races and terminal evidence |
| `recording.pause/resume` | **Unsupported in initial v1**; no paused recording state or dormant writer/consent reservation is advertised. No stop/start alias. Future support must define indicator, wall/sample time, quotas and resume consent before enabling the matrix's planned paused state |
| `job.get/cancel`, protected privacy configuration | Common job cancel retains a recording's verified partial prefix by default; it cannot request discard without explicit deletion authority. Grant administration is the common protected policy/principal flow; no `audio.authorization.*` endpoint |
| `GET /api/v1/audio/state`, `/audio/sessions/{id}` | Authorized state, requested/applied mic/speaker/mute/volume, route, format, consumers, generation, expiry, counters and reason codes; redact other principals' private metadata |
| `recordings.list/get`: `GET /api/v1/recordings?cursor=&limit=` and `/{id}` | `recording.read` plus ACL; default 16, maximum 32 entries and 4 KiB response; opaque bounded cursor. No raw filesystem listing |
| `GET /api/v1/recordings/{id}` | Authorized metadata: state, actual bytes/samples/duration, format, integrity, checksum availability, generation/revision and redacted failure reason |
| `recording.read`: `GET /api/v1/recordings/{id}/content` | `recording.read`, ACL and approved `owner_download` policy; finalized bytes only initially. Partial/recovery content reads are unsupported initially, not silently treated as complete. TLS, no-store, safe generated attachment name, no URL secret, immutable ETag, bounded Range and verified integrity |
| `recording.export` | **Unsupported in initial v1**. Future destination-owned export needs `recording.export`, consent/policy naming that destination and a distinct bounded job with source/destination integrity evidence. A successful pull download is `recording.read`, never a falsely completed export job |
| `recording.delete` | `{recording_id,expected_object_revision}` plus `recording.delete` scope/ACL. Reject active writer/reader; cancel or close it explicitly first. Return deletion job and actual access-revocation/cleanup completion, never an erase promise |
| `audio.test_tone/loopback`, `audio.diagnostics` | Qualified duration/level/source profile and `diagnostics.control`; loopback additionally needs P and capture authority. Remain unavailable until real driver and acoustic/resource evidence exists; status inspection cannot run a tone or open the microphone |

Example `recording.start` arguments, excluding the common envelope:

```json
{
  "source": "local_microphone",
  "format": "wav_pcm_s16le",
  "sample_profile": "pcm16_mono_16k",
  "storage": "nand",
  "duration_seconds": 300,
  "byte_limit": 16777216,
  "retention": {"policy": "expire_after", "duration_seconds": 604800},
  "export_policy": "owner_download",
  "disconnect_policy": "stop",
  "activate_microphone": true,
  "consent_id": "8a634ac34ce84cf1ae15935a4fcd98be"
}
```

This request is admissible only if the actual qualified profiles/store and the
consent cover every field; the example does not assert they exist. A
`continue_local` request also requires the distinct `background_grant_id`.
Former companion names `mode.set`, `audio.route.set`, `audio.mic.set`,
`audio.speaker.set`, `audio.volume.set`, `audio.config.patch`, `capture.*`,
`playback.*` and `recordings.start/stop/delete` are removed wire aliases. Use the
matrix operations above; plural `recordings` remains only for list/get resources.
Priority safety handling belongs to the registered per-job stop/cancel and
role disable/mute operations, not an unregistered `audio.stop` command.

Grant IDs/object IDs are not credentials. Generate independent CSPRNG 128-bit
audio-session, recording-object and stream IDs; never reuse a deleted ID or
derive a path from client text. Bind every callback to boot, session, job,
principal and audio generation. Recording metadata survives reboot; active
session identity and ownership do not. A caller losing the result reconciles
status; expired result history returns `result_unknown`, without replay.

## 5. Lifecycle, media channel and resource bounds

Audio: `off -> preparing -> ready_muted -> running -> stopping -> off|fault`.
Recording: `starting -> recording -> stopping -> finalizing -> completed`,
with `partial|error` alternatives. These are the matrix's artifact states;
the common job independently reports `cancelled`, `failed` or `cleanup_pending`.
`paused` is reserved for a later qualified operation and is not an initial-v1
state. `accepted`, first valid samples, hardware stopped, bytes written,
verified bytes, sync acknowledged and `completed` are distinct
events. First-frame/start timeout is 2 s; proposed stop initiation budget is
50 ms and bounded backend I/O at most 20 ms. Hardware stop must acknowledge
within a proposed 100 ms gate before qualification; otherwise report
`stop_timeout`, quarantine resources and keep the privacy fault visible.
Finalization has a 30 s deadline, independently of the fast capture stop.

Stop invalidates the affected consumer's queued buffers and egress permissions
before waiting for drivers. Its consumer generation changes independently; a
recording stop cannot revoke a separately qualified live consumer. Shared route,
privacy and hardware faults invalidate the shared audio generation and every
affected consumer. Late reads/callbacks cannot append, transmit or restart under a new
session. Preserve only verified pre-stop data and report its actual end sample.
Reject configuration/route changes that conflict with an active job instead of
reinterpreting its format. The caller must explicitly stop it or use an
authorized manager transition that declares affected jobs and waits for cleanup.

### Recording limits, stop and cancel

An immutable job records principal, source/format/profile, consent/background
grant revisions, start time, wall-clock deadline, byte/quota reservation,
retention/export policy and disconnect policy. `duration_seconds` is a maximum
elapsed runtime beginning at first valid frame; a separate 2 s startup deadline
prevents indefinite preparation. It is not inferred from file length. Track
sample frames and exact sample-derived duration separately from elapsed time.
There is no pause/resume in initial v1, so no pause can extend a deadline or hold
consent/storage indefinitely. Live capture/playback use the same finite-wall-time
rule and their own advertised duration/byte profile ceilings.

Reserve container/footer/integrity/encryption/manifest overhead before accepting
frames. `byte_limit` bounds total per-object storage; report media payload bytes,
stored bytes, verified bytes, last synced bytes, sample count and elapsed time
separately. Stop before the next complete frame plus required finalization
overhead would exceed the bound; never split/drop a frame to squeeze it in.
The earliest job, grant, byte, quota or resource limit wins. The initial recorder
profile accepts 1..1800 seconds and 64 KiB..32 MiB only when the actual store,
source format and consent support them. Standalone media profile byte ceilings
are separately declared before exposure, not borrowed silently from file quota.

| Trigger/operation | Capture and artifact outcome |
| --- | --- |
| `recording.stop` before a fault/limit | Close this consumer, then finalize the exact accepted prefix; `completed` only after verification and sync. Actual duration may be shorter than the ceiling; never claim a requested minimum duration was achieved |
| Duration/byte/consent expiry, overflow, write fault, owner loss under `stop`, local privacy Stop/lock or power loss | Close capture/egress; retain a recoverable prefix as `partial`, with exact reason and verified/synced extent. If no valid samples or recoverable container exist, report `error`; no fabricated empty completed WAV |
| `recording.cancel` with `keep_partial` | Stop without a completion claim. Common job becomes `cancelled` only after bounded cleanup; actual prefix remains a `partial` object under its original ACL/retention. No valid prefix means `error`/cleanup evidence, not a playable empty file |
| `recording.cancel` with `discard` | Requires deletion scope/ACL as well as job authority. Close capture and revoke object access, then perform bounded logical deletion. Report `cleanup_pending` or `storage_uncertain` until actual result is known. It never reports a retained completed file or physical secure erasure |
| Common `job.cancel`, tool timeout or transport cancellation | Best-effort stop with `keep_partial`; it cannot silently choose discard. A valid `continue_local` transfer is not cancelled merely by its initiating connection closing; explicit authorized cancel still applies |
| Cancel races with publication | Serialize on job revision at the recorder. Before immutable manifest publication, admitted cancellation owns the terminal path. After publication/terminal completion, return `too_late`/`already_terminal` without deleting; explicit `recording.delete` is a separate request. Retries return the same known/unknown outcome, never another writer |

Finalization errors retain `partial` only when the recovered prefix is verified;
otherwise `error` plus truthful recovery/cleanup evidence. Initial v1 lists
partial metadata but does not serve its content; recovery download must be a
separately advertised later capability. None of these paths silently inserts
silence, drops samples or labels a faulted continuous recording complete.
Playback underrun may emit silence with an explicit underrun counter; 500 ms
without valid data stops the session. No capture, playback or download backlog
is replayed after reconnect.

### Media channel and bounded resources

Live media uses a separate authenticated WSS endpoint
`/api/v1/audio/streams/{id}/ws`, not the JSON control channel. Bind a single-use
10 s ticket to the authenticated principal/session, stream, direction and
format, with source scope, consent revision and approved destination. Neither
the ticket nor a HID lease preserves capture authority after revocation. Follow
the common browser subprotocol-ticket/Origin rules, without URL
credentials. One media socket supports the permitted direction(s); duplex is
unavailable until qualified. A binary frame has a fixed 40-byte network-order
header: version u8, direction u8, format ID u16, stream ID 16 bytes, sequence u32,
sample offset u64, payload bytes u32, flags u32; maximum payload 1,920 bytes.
Negotiation rejects a profile whose complete frame exceeds that payload bound;
for example, PCM16 stereo at 48 kHz can use 10 ms, but not 20 ms, wire frames.
No implicit fragmentation, channel dropping or unadvertised downmix occurs.
Offsets/sequences must advance exactly; reject reuse, oversize and format changes.
Never send samples through ordinary state subscriptions. A slow reader beyond
the negotiated ring budget stops that stream, rather than retaining old audio.

Initial compiled budgets, to qualify together with HID/Wi-Fi:

| Resource | Bound / behavior |
| --- | --- |
| Audio/media jobs | One capture source, one recording writer, one playback source; sharing requires the qualified matrix below. One pending manager operation, fixed queues; safety stop bypasses them |
| Media memory | Ceiling 192 KiB: I2S DMA 16, capture frame pool 32, playback ring 32, recording spool 32, file transfer 8, DSP workspace 32, task stacks 24, metadata 16 KiB. DMA uses suitable internal memory; other pools use proven-capable memory |
| Network | At most one media WSS and one file download, within the common four-socket total, two control-WSS and one-handshake limits. No new unbounded connection pool; reserve control/stop resources |
| File transfer | One 8 KiB I/O buffer; Range at most 256 KiB/request, default rate ceiling 128 KiB/s, 5 s inactivity closes the transfer; resume only immutable finalized bytes after reauthorization |
| Persistence | One writer, full bounded-job quota reservation before activation, effective free-space reserve and at most 64 indexed objects. No recording samples, progress ticks or lease renewals in NVS |

The memory ceiling excludes TLS, filesystem/FTL and other firmware budgets;
reserve and measure their combined internal/PSRAM minima before start. No codec,
network, NAND or TLS work runs in HID/IMU callbacks. Reduce optional recognition/
UI load first; recording stops truthfully rather than starving key release.

## 6. Storage, failure and concurrency

Persistent recordings require positively identified NAND geometry, inventory
of existing contents, an explicitly provisioned owned store, wear management,
authenticated encryption and protected keys. Missing storage/keystore disables
file recording; live capture may still qualify independently. Never autoformat
unknown NAND, use application/NVS flash as a fallback or infer capacity from a
marketing label. A filesystem/helper integration is new work; the BSP handle
alone is insufficient. Enforce nonce uniqueness and authenticated metadata in
the storage design; no client-selected encryption nonce or key.

Append bounded chunks and integrity records; sync at configured intervals and
finalization. Publish a complete immutable object only after exact length,
container header, integrity/hash and manifest are verified and synced. Do not
assume FATFS rename or a successful return alone proves power-loss atomicity:
use a recoverable manifest/journal with generation and checksums. Recovery marks
unfinished objects `partial|error` and never resumes capture. Downloads decrypt in
bounded buffers; SHA-256 is unavailable until the final plaintext file hash is
actually computed. Logical deletion revokes API access and reclaims space; it
does not prove physical secure erasure of NAND/FTL remnants. Lowering quota never
silently removes existing files; reject new reservations until within budget.

### Retention and authorized data access

Every persistent job explicitly approves a finite `expire_after` retention
duration, bounded by its consent/profile; `manual`/indefinite retention is
unsupported initially. Candidate bounds are 3600..2592000 seconds, and the
example's seven days is a request, not a default applied without consent.
The deadline starts with object reservation, covers partial/error recovery
data as well as completed media and is recorded in authenticated metadata.
Runtime stop/cancel cannot silently reset it. No capacity-based eviction or
automatic upload substitutes for the chosen policy.

Persistent recording additionally requires a qualified trusted expiry timebase
that survives restart, plus monotonic enforcement during a boot. Backward clock
changes/restarts cannot extend retention or grant expiry. If remaining lifetime
is unknown, deny content access/new persistent recording and report
`retention_time_unknown`; do not invent a fresh retention interval or claim
deletion. Reconciliation uses the recorded policy and reliable time before
declaring objects available or expired. This clock/storage integration is a
new qualification gate, not a capability inferred from the BSP.

At known expiry, revoke access/tickets and stop an active reader, then run
bounded deletion under the exact retention authorization. Keep cleanup/uncertain
space-reclamation evidence until established; failure never resurrects API
access or claims secure physical erasure. Explicit `recording.delete` may remove
an owned object earlier under its separate scope and revision rules.

`export_policy=none` permits no network content read or destination export.
`owner_download` permits the named ACL principal's bounded authenticated pull
with `recording.read`; it never creates a new recipient ACL or unattended push.
Read failure/timeout records actual byte progress, not successful export.
Pull authorization is rechecked on every range and during a transfer. A later
`recording.export` implementation must name a separately approved destination,
own a finite transfer job and distinguish local read, transmitted bytes and
verified destination receipt. Until then it rejects `not_implemented` without
opening a destination or returning success. Pause/resume/seek/export rejections
have no start/stop/delete side effects.

| Combination | Planned admission |
| --- | --- |
| Local microphone to live stream or file | First independent codec path to qualify; mono PCM/WAV and bounded resources |
| Recording + live stream / local recognition | Same capture producer with bounded consumer queues and separate source/data grants; disabled until fan-out and overload tests pass. The codec mirror API is not a substitute; a live stream is not a recording-export job |
| Capture + speaker | Half duplex by default; reject conflicting start. Full duplex requires actual AEC/acoustic and resource qualification, not a boolean preference alone |
| HFP HF + recording/recognition | Unavailable now. Later explicit source permission and qualified SCO format/fan-out; no implicit call/downlink recording |
| HFP + A2DP | Exclusive initial routes; no automatic role/profile switch. Profile implementations and host combinations need independent tests |
| BLE/USB HID + Wi-Fi audio/recording | Only a measured coexistence tuple may enable background operation; release/local privacy retains priority |
| USB UAC + HID/CDC | Future descriptor/endpoint/codec integration under the single USB owner; current USB input readiness says nothing about UAC |

S31 Classic support does not enable HFP/A2DP in a BLE-only image. A different
ESP32 board may have no suitable controller or codec. Capability errors include
`not_implemented`, `hardware_unsupported`, `profile_disabled`, `not_qualified`,
`resource_conflict`, `grant_required`, `privacy_locked`, `storage_unavailable`,
`quota_exceeded`, `backpressure`, `stale_generation`, `stop_timeout` and
`storage_uncertain`. Rejection never changes the prior desired configuration;
an asynchronous hardware/storage failure reports actual partial state.

## 7. Acceptance matrix before enabling capabilities

| Gate | Required evidence |
| --- | --- |
| Schema/protocol | Every field default/range, whole-patch/revision validation, idempotent duplicate start/delete, unknown outcomes, stale boot/session/generation, invalid/oversize media and bounded list/Range |
| Authorization/privacy | Standing-grant starts repeatedly without new local prompts; ceiling escalation denied; ACL isolation; revocation/expiry, lock/local Stop, failed/readability-stalled indicator, session/channel loss and explicit distinct background-grant transfer. No HID lease dependency, screen-off capture or live egress surviving owner loss; no samples/secrets in logs/status |
| Audio lifecycle | Actual gain/volume/mute acknowledgements, rates/channel mapping, blocked-read stop, partial startup/cleanup, underrun/overrun, resampler/AEC qualification and no stale callbacks. Pause/resume/seek/export reject without side effects until their later lifecycle is qualified |
| Recording lifecycle | Whole-frame duration/byte/overhead boundaries, wall versus sample time, every stop/cancel disposition, missing discard scope, common cancellation retaining partial, publication/cancel races, exact verified/synced prefix, zero-frame failure and no false completed artifact |
| Storage/data | Geometry/ownership, full/quota/bad-block/GC pauses, write/sync failure, power loss at every publish/delete phase, partial recovery, checksum/encryption failure and wear/endurance. Finite retention, restart/backward/unknown clock, expiry during reads, access revocation versus cleanup, export-policy/ACL/range isolation and no unqualified partial content download |
| System/host | Simultaneous HID releases plus TLS/media/storage stress, no-host/slow clients, disconnect/reconnect without replay, physical indication/local recovery, measured stack/heap/latency/thermal behavior; HFP/A2DP/UAC each separately on actual hosts |

Implement only after the complete API/configuration inventory and these shared
semantics are reviewed together. First gate secure control/scoped grants, then
one local codec path, live media, qualified storage/recording, and finally
recognition/profile/concurrency combinations. Until each gate passes, advertise
the precise unavailable reason; do not substitute timers or empty audio files.
