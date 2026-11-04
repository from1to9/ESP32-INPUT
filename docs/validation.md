# Compatibility and acceptance

## Current combined delivery boundary

The current combined source includes HID, motion/touch, spoken letters, audio,
recording, authenticated LAN control/MCP, native connection management and signed
OTA staging. The source checkpoint follows the
[combined checklist](delivery-checklist.md). Its separately authorized device
attempt is recorded below. Historical observations belong to their named images
and must not be used as evidence for a different combined image.

The latest reviewed source incorporates the repairs recorded below. Source
synchronization is authorized separately from device acceptance. Earlier startup
faults remain in their image-specific records; the latest capture observed fresh
IMU samples but still failed the error-log and physical Motion gates.
This checkpoint is not a qualified release. Reusable findings are indexed in
the [engineering lessons](../.agent/shared/ENGINEERING_LESSONS.md).

### Wi-Fi browser input and Bluetooth reconnection on Windows (2026-11-04)

Clean `1e5e7b5dae0d7ec5dc043d16fc1533ea214fbd5b` was written to the
inactive app slot and read back. The previous confirmed application and its
selector were preserved. The running firmware reported the expected ELF identity
and confirmed its first boot. After confirmation, the peer API reported one saved
BLE HID record and one saved Classic audio record.

- **Local navigation.** The operator reported that the menu responded after a few
  seconds. The first accepted tap was not timed. The log recorded an LVGL lock
  acquisition failure at 4.1 seconds and first-boot confirmation at about 50 seconds.
  Their relationship to the reported delay is unknown. Re-entering Keyboard
  from Mouse opened the Touch grid. Audio → Manage devices opened the Audio tab.
- **Wi-Fi mouse and keyboard.** The device served its HTML, CSS and JavaScript
  over station HTTPS. The test Chromium profile pinned the enrolled public key
  and verified the served certificate against the enrollment. System-wide trust
  was unchanged. The operator approved keyboard/mouse, management and update
  access for 24 hours on Mosaico. The browser reconnected using its saved
  identity and acquired control explicitly. The Windows operator saw the pointer
  move and click, received the complete text `Mosaico WiFi 123` in Notepad, and
  saw `a` appear after an on-screen letter tap. The browser then released
  control and disconnected. The bench exercised input only.
- **Saved Bluetooth devices.** In the Audio tab, the saved Windows row
  disappeared during a connection attempt while the page showed
  `Pairing closed`. The operator needed `Add device` to reconnect. A later
  snapshot showed the saved Classic record with both HFP and A2DP connected.
  There is no synchronized peer snapshot from the instant the row vanished.
  On the input side, selecting a saved mouse target repeatedly showed waiting,
  pairing-closed and authentication states. The captured BLE link lost readiness
  and later regained it. Individual screen taps were not timestamped.
- **Speaker playback.** The operator heard audio and could restart the
  speaker after it stopped. Three playback jobs ran for observed intervals
  of 42.6, 17.0 and 29.9 seconds and ended in `AE_STOP_UNDERRUN`. At each
  exit the same A2DP stream remained connected and streaming, the sample
  binding was cleared, and the received overrun count increased by one.
  The one-shot ACL capture observed seven replacements of incomplete packets
  at both ingress and dequeue. Both encoded and decoder-input observations saw
  17 forward sequence gaps spanning 28 skipped IDs. No allocation or enqueue
  failure was recorded in that capture. These counters do not locate loss
  upstream of the host ingress. Continuous speaker playback remains open.

### Classic connection display DMA failure and chunk repair: 2026-11-04

Clean `cf2c8daff4bfb529e5f90ed8c4c44a7699afde64` booted with a reported
4095-byte display transaction limit. After the first successful Classic audio
pairing/connection, the user observed frozen screen pixels and unusable touch.
BLE keyboard/mouse, LAN replies and periodic health logs continued. The first
retained `Draw bitmap failed: ESP_ERR_NO_MEM` was at uptime 120639 ms, with
184 such errors in that capture. The retained failed request was 4095 internal
DMA bytes; later samples reported about 5463 free DMA bytes and a 3072-byte
largest block, compared with 6912 before connection. Those samples do not
measure the free heap at the exact allocation attempt or prove a leak.

The user disconnected audio on the host, retained pairing and did not restart.
Menu/touch recovered. The subsequent capture had no new bitmap errors; health
samples at 263/273 seconds reported 7643 free DMA bytes, a 2048-byte largest
block and HID ready. This is a connected/disconnected control observation,
not evidence that a 4095-byte free block had returned. A periodic sample can
follow a successful submission while its transfer buffer is still retained.

The preceding bus-request cap cannot guarantee a smaller color allocation:
the pinned LCD IO factory caches the SPI driver's descriptor-rounded maximum.
The follow-up source repair caps that query to 2048 bytes on the dedicated
panel bus, so the unchanged LCD driver splits full frames at the actual owning
boundary. It preserves native errors, smaller limits, other buses, TE and final
completion. It adds no reserved buffer or startup allocation. The diagnostic
log now identifies the effective color-chunk limit rather than hardware capacity.

The native SDK transport regression first failed at a 3072-byte contiguous
limit, then passed full-frame, tail, queue-depth and alignment cases with the
repair. Real allocation failure still returns an error without final completion.
These are software checks; the repaired image has not been device-tested in
this record. Acceptance still requires visible Menu/touch transitions during
Classic connection, disconnection and reconnection with pairing retained,
plus concurrent BLE HID and authorized HFP/A2DP/USB audio workloads without
new display allocation errors. Indicator confirmation and update self-test
requirements and deadlines are unchanged.

At the default queue ceiling, retained color payload falls from 4095 to 2048
bytes, a 2047-byte reduction; depth two can retain 4096 payload bytes. Allocator
metadata and other live buffers remain additional costs. This is a payload bound,
not a guarantee from the sampled free totals. Both Classic profiles can be
connected, but the application renders one selected audio route. Their SDK
connection/codec buffers and I2S internal descriptors remain outside this bound.
I2S payloads use PSRAM under the existing supported configuration; no audio or
radio capacities are reduced by this repair. Simultaneous profile connections
and active voice/music/USB transitions still need measured device headroom.

### USB transport deployment and capture startup investigation: 2026-10-26

The separately authorized application-only deployment used clean source
`f0ec1bfb582cb832df8b3f99e53b97e41ed1fd61`. Its 4,702,544-byte application
SHA-256 is `e9f7b4a3d6ced8f04cfc362b39fb6e01cb5157b4d904117a94436d87d0d2b720`.
Compilation and independent package review passed. Flash verification passed;
the installed partition table, NVS and PHY data remained identical. No new full
backup was taken, as requested. Runtime identity matched the delivered source.

The user confirmed normal boot, touch and page switching. A 90-second startup
capture observed connected/ready Bluetooth, LAN connectivity and advancing IMU
samples. It remained failed because two duplicate GPIO ISR installation errors
were logged; the same errors exist in the preceding image's startup evidence.
There was no observed panic or reboot within this bounded capture. This does
not qualify long-duration operation or Bluetooth host interoperability.

USB audio controller mode was observed valid. Across two bounded output probes,
OUT recorded 242,498 successful completions and 23,279,808 successful bytes,
with no failed OUT completions. This is physical transport progress beyond the
previous stalled two-packet observation. Acoustic success was not confirmed:
the user still reported timeout/error. One playback job delivered 2,402,880
bytes to the codec interface, then overflowed immediately after Mic admission.
The next playback job delivered 5,685,120 bytes (59.22 seconds of mono 48 kHz
16-bit PCM) without overflow, then ended on a USB stream/connection-generation
change. Codec-write accounting is not proof of audible playback.

A subsequent input-only host probe counted 4,183,040 client-format float frames,
with no finite samples above its amplitude threshold and no callback errors. Two device Mic jobs started with the USB input stream
selected, but the codec remained prepared without capture running; both reached
the unchanged two-second startup deadline with no first frame and zero capture
bytes. A missing host input consumer does not explain those attempts. The
existing logs do not expose the display permission observation, so the exact
pre-capture gate remains unproven. Raw logs and host/device details stay local.

The follow-up source repair keeps an already active speaker progressing while
a new Mic awaits its indicator, provided no codec command is pending. It also
retains manager ingress frames while the engine ring is full. Native regressions
first reproduced both failures, then checked ordered recovery, unchanged Mic
permission/deadline checks and safety cancellation. Copied coordinator context
diagnostics expose the next capture gate without rereading hardware or changing
policy. These follow-up changes are not part of the deployed image above;
their physical behavior remains unqualified until a separate verified update.

### Capture indicator confirmation failure: 2026-10-26

The follow-up application from clean source
`31fe54ada3cdc58ab965df41671419b62ff53db6` was independently reviewed and
deployed. Its 4,703,760-byte application SHA-256 is
`5ede5644cda1d641a1d385da774ee1263b994486753c37d91124986793260d6e`.
Application verification and unchanged partition-table/NVS/PHY checks passed;
no full backup was taken. Runtime identity matched. The 90-second startup
capture again failed on the two existing duplicate GPIO ISR installation
errors, without an observed panic or reboot. This is not a passing health gate.

An isolated USB Mic request still timed out. Its capture generation matched an
armed recording indicator; the screen was visible, the display owner was alive,
the clock was valid, and neither lock nor maintenance denied capture. However,
display transfer confirmation and indicator usability remained false until the
unchanged two-second deadline. The codec was prepared but capture never ran or
read a first frame. The input-only host probe counted 4,204,544 client-format
float frames, no finite samples above amplitude 0.00001, and no callback errors.
This narrows the observed block to indicator transfer confirmation; it does not
prove why that confirmation was absent or establish usable microphone audio.
Raw captures remain local. A software receipt-retention regression and its
repair must be distinguished from physical causal confirmation on a later image.

The receipt-retention repair was subsequently deployed from clean source
`8d04401ebb5a5d6302d821b24e17b375783e8aaa`. Its 4,704,016-byte application
SHA-256 is `d641557a20101beb474eebddf6d41923255db52523c13669768b58edb3c1271e`.
Application verification passed with partition table, NVS and PHY preserved,
and runtime identity matched. Nineteen focused presenter regressions, native
LVGL/UI/presenter/Board composition, target compilation and one independent
source review passed before deployment.

On this image, both initial USB Mic start and restart after the host consumer
closed obtained a confirmed display transfer and entered capture. The user
confirmed that Mic could enable in both attempts. The device delivered
2,664,960 and 1,766,400 bytes of mono 48 kHz 16-bit PCM respectively, equivalent
to 27.76 and 18.4 seconds. Both input-only host probes observed finite samples
above amplitude 0.00001 and zero callback errors; no PCM was retained. The
bounded probes ran for 60 and 90 seconds, but those durations include time
before the user started Mic and must not be reported as active-capture duration.
Each job ended when the host consumer closed, USB selection cleared and the
connection generation advanced, with `AE_STOP_OWNER_LOST` / `AE_STALE` rather
than the previous startup timeout. The user then saw unavailable controls.
The generic `Unavailable` / `State changed` wording does not explain host demand.

A separate output-only attempt delivered 10,673,280 bytes to the codec interface
(111.18 seconds equivalent), with playback running, speaker unmuted and no
reported SDK error. The user could start Speaker but heard no sound. Acoustic
playback therefore remains unqualified. The test used sparse, low-amplitude
tones; the default 30-percent device level maps to -35 dB, which makes stimulus
level a candidate explanation, not an established cause. These observations
do not qualify sound quality, duplex, Bluetooth audio or endurance. Health
captures retain the existing GPIO errors and job-failure flags for host-owner
loss; no complete health pass is claimed.

A further attempt at the user's reported device volume of about 80 percent
still produced no audible sound. It delivered 11,741,760 bytes to the codec
interface (122.31 seconds equivalent); playback remained running and unmuted
without a reported SDK error until the bounded host probe stopped. The actual
volume register and nonzero sample content were not exposed in that image's
logs. Low default volume is therefore insufficient to explain the result.
The subsequent local-tone discriminator is recorded below. Nonzero playback
data at USB ingress and I2S submission remains unobserved. Neither software
counters nor GPIO command success prove electrical output.

### Local speaker discriminator: 2026-10-26

The default-off `APP_LOCAL_SPEAKER_TEST` diagnostic option supplies a finite
1 kHz mono 48 kHz PCM16LE tone directly to the production board codec adapter
before the normal media manager starts. It bypasses USB payloads and the media
engine, opens no microphone and writes no user preferences. Physical button or
lock input cancels the test. Normal startup can continue only after confirmed
codec shutdown; software transfer counts do not establish audible output.
This option does not implement or qualify the public `audio.test_tone` API.
The enabled image from clean source `4e837cdbf8443b94d5cf322afa7cfc4e3327d27e`
was written and verified without a full backup; the partition table, NVS and PHY
data were preserved. Its 4,706,288-byte application SHA-256 is
`1e962225e20bd865b0b32d5a9396cbadfdaa31392932456c9feb616f0f777e5f`.
The user heard the local tone. The fixed three-second output deadline returned
`AE_TIMEOUT` after 283,200 driver-reported bytes, with 284,160 bytes produced
and submitted. Mute and stop returned `AE_OK`, shutdown was confirmed and the
normal media owner took over. Audible local output is established for this
profile; complete 300-frame delivery and USB sound are not established.

The diagnostic module caused a separate internal-memory regression. Exact
linked objects added 1,021 bytes of internal static payload and moved the heap
start by 1,040 bytes. Boot entry measurements corroborate that loss before
codec prepare/teardown. The diagnostic image later had 12,167 free DMA bytes,
but its largest DMA block was only 7,680 bytes; display transfers needed an
8,190-byte allocation. Display `ESP_ERR_NO_MEM` appeared during initial startup
and recurred during the USB comparison. More total free memory did not imply
sufficient contiguous space. A prepare/teardown leak is not established.

The USB comparison ended after 15.45 seconds with a serial IO error and no
admitted Speaker job. The user reported a frozen display and then manually
restarted/reconnected the device; this is not evidence of an autonomous crash.
A read-only host query observed mono 48 kHz signed 16-bit physical output and
mono 48 kHz float client format. No host hardware volume/mute property was
exposed, consistent with the descriptor's absence of a Feature Unit. These
facts do not reveal whether accepted USB playback frames contain nonzero PCM.

The same clean source was subsequently rebuilt with the test option disabled.
The generated configuration, compile database and linked symbols all confirm
that the diagnostic module is absent. Its 4,704,016-byte application SHA-256 is
`63e69644576b5173701ad07395dad6637eff5e6f7c5e2caf5e277536fe5b1931`.
That normal image was restored and verified, preserving settings and the
partition table. The runtime USB interface reappeared. At the user's request,
no functional device test or new log capture followed restoration; subsequent
changes are restricted to local analysis, validation and Git publication.

The source repair moves only the diagnostic frame and result storage to
supported external BSS; callback synchronization and cancellation remain in
internal memory. Placement and tests are checked locally. Contiguous DMA
recovery and behavior of that repaired diagnostic image remain untested on
hardware. The normal restored image does not contain this optional module.

### Remaining live-control timeout after USB readiness repair: 2026-10-26

Source `bb3b075b0a52b441c1b1c9fed45c876f8826aaff` was application-only
flashed and byte-verified. The 4,706,816-byte application SHA-256 is
`772349ce1a70c07f955971576df94c16f8a5c0db1cadb6c3c9c6a74aa354cccd`;
settings and partitions were preserved without a full backup.

The user initially confirmed stable startup controls and audible stop/restart,
then reported that both Mic and Speaker buttons became grey and unusable while
audio continued. A persistent `Operation completed` message survived page
navigation. The 180-second capture contained 18 health samples but failed the
audio-job gate; this attempt is not a complete pass.

The log observed media `busy=1` with active playback at 53.961, 54.821, 64.890
and 74.921 seconds. At 83.979 seconds the owner became OFF, and the partial
playback job reported `AE_STOP_IO` / `AE_PRIVACY`; cached board SDK errors were
zero. Later manual starts produced sound, with explicit completed stops at
102.476 and 108.740 seconds. The host stimulus ran for 150 seconds, separately
from that earlier timeout. These observations distinguish the pending control
failure from the planned end of the host test.

A native fixture using the production manager and engine with an asynchronous
codec reproduced the same failure under continuous PCM ingress: volume timed
out after 30,001 simulated milliseconds while 30,030 writes delivered
28,828,800 bytes, ending with the same partial-job stop/error pair. A control
retry saw pending I/O before reserving its turn; the progress pump immediately
queued another PCM request. Successful transfers therefore starved control
completion. This deterministic source reproduction supports the device-log
attribution; its timing is simulated, not measured device latency.

The repair reserves the control turn, retires the exact pending I/O or unmute,
and resumes PCM after the control completes. The same fixture completes volume
and gain in three simulated milliseconds, with ordered PCM content and no
overrun/discard. Safety-stop and privacy cancellation remain separate checks.
Accepted commands display pending until their actual result arrives. Successful
notices expire after three seconds and clear on navigation, including the
coordinator's global status. The repaired image still needs physical live-volume,
stop/restart and page-navigation validation. Bluetooth playback and microphone
tests remain deferred until this USB round is complete, as requested.

Focused sanitizer checks passed 13 engine/worker/startup variants, 32 media
control cases and 163 production coordinator cases. The normal-profile
coordinator harness now supplies its previously missing configuration header;
this does not enable the optional startup tone. The pinned target build and
482 declared task stack paths passed, including live control dispatch and
pending-I/O retirement. Internal heap start remained unchanged. Interrupt stack
coverage and full release qualification remain unproven; these focused results
do not imply that every repository suite was rerun.

### Live-control device recovery and failure-only notices: 2026-10-26

Source `6d549402e217ce17c85bc20e50352c8654a48d1b` was application-only
flashed and verified. The 4,707,200-byte application SHA-256 is
`e2f016e3c7c1b889e54a22ee76e37ca820a1e30ad21eab5ca188f60717b8c9bb`.
Partitions and settings were preserved; no full backup was performed.

The first 180-second capture passed its parser with 18 health observations.
During playback, observed volume changed from 30 to 92, 47 and 91; a pending
control observation was followed by completion without the previous timeout.
The user confirmed audible playback and working controls, then reported grey,
silent controls near the end of the bounded host stimulus. The first log
contains a requested completed stop. A subsequent retained job reports owner
loss/stale state after the host stimulus ended; the intervening transition was
not captured, so the reported screen click is not established as its cause.

A second host stimulus restored playback. The user confirmed that it continued
without stopping. The later job delivered 12,622,080 bytes of mono 48 kHz PCM
(131.48 seconds) with no observed job error or discarded frames while running;
volume and requested stop/restart transitions were also observed. The second
180-second capture retains the earlier partial owner-loss job and therefore
still fails its overall audio-job gate. This record distinguishes that retained
failure from the later running job rather than weakening the parser.

The user confirmed successful notices disappeared after three seconds, then
revised the interaction: success should be silent and only failure should
notify. The follow-up source removes both success and pending footer notices;
existing button/job states continue to show progress and errors remain visible.
This notice-only follow-up needs its own deployment check. Bluetooth audio,
long-duration endurance and complete release qualification remain pending.

### Failure-only notice deployment and USB duplex failure: 2026-10-26

The notice-only image `c7b0f96eea185ccd245460b68c31d894d4107d9e` was
application-only flashed and verified, preserving settings and partitions with
no full backup. Its 4,706,944-byte application SHA-256 is
`bd33ee977b8206d517efbd635eec7b5e03a24b751c87ec734dde9345ae38cc72`.
The source passed 32 media-control sanitizer cases, the pinned build and the
same 482 declared task paths. Physical notice acceptance was interrupted by a
new simultaneous-audio report and remains distinct from those source results.

During the output-only host stimulus, both codec capture and playback were
observed running. Mic delivered 404,160 bytes, then ended partial with
`AE_STOP_OVERFLOW` / `AE_OVERFLOW`; the speaker remained active. USB IN reported
404,120 bytes, and the capture-admission rejection counter reached 5,463. The
user reported Mic exiting and crackling playback. The 90-second capture has
nine health observations and fails the audio-job gate. This is not evidence of
stable duplex operation or proof that a host input consumer would remove the
failure.

A subsequent host probe enabled simultaneous input/output without retaining
PCM. It received frames but no nonzero input samples; no fresh device audio
activation appeared during that window, only the earlier retained failures.
That attempt is unexercised and cannot qualify or disprove a repair. The
packet-sized TX write per worker wake was subsequently reproduced as a native
throughput defect; crackling playback still requires physical confirmation.

The actual pinned class/FIFO test supplies one capture frame every 10 ms,
completes IN and OUT every millisecond, and skips one worker wake in four. The
old adapter rejects capture at 130 ms with its four-frame queue full. The repair
admits at most four existing 98-byte chunks per pass, stops on short/zero writes
and rechecks flush, readiness and age between chunks. Scratch allocation, queue
capacity, descriptors and deadlines are unchanged.

Both full-speed and high-speed native scenarios pass ten simulated seconds:
960,000 produced bytes equal admitted bytes, with 959,806 completed bytes plus
96 in flight and 98 in the finite FIFO. Per-byte order, application/FIFO
conservation and zero admission rejection or playback drop are checked. Seven
candidate class/FIFO scenarios pass, and five historical negative oracles remain
failing as intended. Existing low-fill and finite-tail assertions are preserved;
the resumed-capture stimulus now includes a four-millisecond worker gap to keep
exercising the 94/96/98-byte branches after batching improves FIFO occupancy.
This is source/host evidence, not a device duplex pass.

The user then reported a frozen interface and manually entered BOOT. The last
90-second observation still contained advancing health records, no logged
panic/watchdog/allocation failure, and a largest DMA block of 8,192 bytes. That
capture does not identify the freeze's onset or cause. The ROM download
interface observed afterward reflects the confirmed manual BOOT action, not
proof of an automatic firmware crash.

### USB startup readiness interaction repair: 2026-10-26

Local Audio start admission now requires the same valid USB stream identity,
format and requested role to remain observed for the named 1,000 ms settling
interval. Loss, replacement, unknown runtime or invalid timing revokes that
readiness immediately. Timer maturity does not advance the control revision
and cannot invalidate an unrelated held Stop/Mute or replay a prior Start.
USB selection remains possible without a host stream. The current preparing
hint takes priority over retained command text; current faults and historical
job results remain intact.

Thirty control-layer sanitizer cases passed, including churn, exact threshold,
fresh explicit consent, per-role readiness, disconnect/replacement, missing
runtime, clock overflow/regression and independent Stop/Mute. The focused
native Audio UI suite passed 118 renders across both themes and pointer tests
that reject a press begun while Start was disabled. New preparing and retained
error scenes were visually checked for alignment and clipping. This was the
Audio subset, not a rerun of every UI suite. The pinned target build and expanded
task stack paths passed; physical startup/recovery verification remains pending.

### Audible USB playback and startup control churn: 2026-10-26

Published source `acac8e6df130793db3041c20e27639ff251a1f8f` was built
with the optional boot tone disabled. The 4,706,400-byte application has SHA-256
`57f995e733b9a3479072fd2ade73d883ae11727527422ef3a08c72cf08fd791e`.
Application-only write and independent flash comparison passed; the partition
table, NVS and PHY bytes were unchanged. No full backup was performed.

The user heard the host's alternating one-second 1 kHz USB tone after starting
Speaker with Mic off and settling the volume near 80 percent. The three-minute
capture contained 18 complete health observations and no parser failure codes.
The running job was observed across 167.669 seconds. At the final paired
summary, USB delivered 8,052,480 samples and the board reported 8,050,080 samples;
both had 3,861,539 nonzero samples and peak 4,096. These cumulative snapshots
are sequential, not atomic. All 16,771 recorded board writes had valid reported
prefixes and SDK success. Cached DAC volume and mute reads matched their
requests; settled volume was observed at 79 and later 77 percent. The host
rendered 8,656,896 frames with zero callback errors or incoming silence flags.
No raw PCM was saved.

This establishes audible output for this bounded USB playback attempt. It does
not prove the cause of earlier silence, uninterrupted sample timing, other
hosts, duplex operation or endurance. Earlier captures lack equivalent sample
content and actual-volume evidence.

Startup remains defective: the user reported buttons rapidly alternating
between usable and grey, with early attempts ineffective, then stable controls
and playback after several seconds. During that interval the device remained
USB-mounted while host alternate-interface selection repeatedly closed and
reopened streams; observed link generation advanced from 7 to 79. This
correlates the symptom with host stream negotiation and motivates an explicit
local readiness-settling state, without retaining or replaying Start consent.

A separate 90-second post-playback capture retained eight health observations
but failed the audio-job gate: host stream closure left a partial playback job
with `AE_STALE` / `AE_STOP_OWNER_LOST` and five discarded frames. Hardware
returned OFF with mute confirmed and cleanup complete. This is retained
terminal evidence, not an unreported successful stop or a passing recovery
qualification. A new-start recovery and the control interaction repair remain
pending.

### USB playback content observation candidate: 2026-10-26

Following renewed device-test authorization, the candidate adds task-only
nonzero-sample counts and absolute peaks at complete USB frame delivery and
the board's post-conversion, SDK-reported I2S write prefix. It retains no raw
audio. Cached existing DAC volume/mute reads and engine volume are logged;
no extra bus transactions or control-policy changes are introduced. Stream
and consumer generations distinguish retained USB observations from current
ones. Board observations remain historical acknowledgements, not DMA or
acoustic completion.

Fifteen USB application cases, twelve observer sanitizer cases and ten board
adapter/ES8311 sanitizer cases passed. The pinned USB class/FIFO and DWC2
comparison checks also passed with their expected baseline failures retained.
The pinned ESP32-S31 target build and declared task stack budget passed;
interrupt safety remains unqualified. Both new aggregates and the enlarged
observer are in external BSS; the internal heap boundary remains identical to
the previously restored normal image. The optional boot speaker test stays off.

The broader portable check stopped at the coordinator harness because it
cannot find `sdkconfig.h`, included by unchanged `main.c`. Neither the harness
nor that include changed in this candidate; the broader gate is not claimed
as passing. Focused checks above cover the changed behavior. Physical USB
sample content, actual volume, audibility and stability remain pending the
new deployment; these source checks alone do not resolve the silence defect.

### USB silence offline follow-up: 2026-10-26

This investigation used clean source
`8662b22120f78507be0689f328ae9f7bc36d15ba` and retained logs. It did not
flash firmware, open a device, start host audio or collect new device logs.
No deterministic firmware cause of the reported USB silence was reproduced.

The original host renderer and the later comparison renderer were called
offline with varying callback lengths, then passed through the host's actual
float-to-signed-PCM16 converter. Each produced 96,000 samples. The original
440 Hz stimulus contained 4,792 nonzero samples with peak 1,311; the comparison
1 kHz stimulus contained 46,000 nonzero samples with peak 4,096. Conversion
error stayed below half a PCM16 unit and the intended silent tails remained
zero. This rejects an unconditional zero-output or float/PCM conversion defect
in those isolated paths. It does not observe the live host mixer, output
callback flags, USB wire or device input.

Six isolated sanitizer cases composed the production USB application adapter,
audio engine, board adapter and pinned ES8311 driver. Cases covered levels
80/30/0, Mic stop before Speaker start, local playback stop before USB playback,
and an all-zero input control. Each passed six 480-sample frames through the
actual application queues and asynchronous board mailbox. Varied signed samples,
including both signed 16-bit extrema, reached both I2S slots byte-for-byte even
after the submitting buffer was erased. DAC volume/mute registers and PA state
matched the modeled requests. These cases substitute FIFO, SDK and authority
boundaries and do not run the application audio worker, TinyUSB controller,
physical DMA/cache or analog output. All 41 recorded inputs stayed unchanged.

The shared board path uses the same format, pins and I2S copy operation for
local and USB playback. USB preparation also sets ADC gain; source and the
composition cases show no additional DAC mute or USB-only PCM scaling in that
step. The normal engine starts at 30 percent and does not inherit the boot
diagnostic's transient 80 percent. A settled UI value reflects the engine's
acknowledged setting; a slider position during a drag does not prove that
setting was applied in an earlier device attempt.

Reanalysis of the previous volume-80 attempt found twelve intervals of at least
five seconds with codec-interface accounting between 95,914 and 96,133 bytes
per second, close to the nominal 96,000. Pending received-minus-delivered bytes
were 2,880 to 3,840 at those observations. This argues against a sustained
application submission stall during that attempt, but silence can have exactly
the same byte rate. It is not a DMA completion or electrical-output measurement.

The next missing discriminator is sample content on the device: compare bounded
nonzero-sample counts and absolute peaks at complete USB frame admission and at
the board's converted/reported-written samples, tied to the same stream and
consumer generation. Also retain the engine volume and the already performed
DAC volume/mute readbacks. These observations are not implemented or collected
by this documentation update. They must not retain raw PCM, add bus reads in
getters, scan payloads in an ISR, or consume unbudgeted internal DMA memory.
Zero USB input would direct investigation toward host/USB ingress; nonzero
input lost before board submission would implicate the software handoff;
nonzero board submission with confirmed gain would leave runtime DMA/clock,
power and analog output to distinguish. Existing evidence cannot select among
those branches, so USB playback remains unqualified.

### Combined source checkpoint: 2026-10-21

| Check | Current evidence |
| --- | --- |
| Complete host regression | `python3 tools/check.py` passed, including 146 actual coordinator cases, protocol/generated-artifact checks, threaded media composition, server/authentication, connection ownership, settings/storage and updater failure paths |
| Companion/MCP | Five tests passed using the actual companion, stdio MCP and signing code with named external boundaries; independent review also passed four offline real-TLS cases and 54 C/Python format comparisons |
| Production UI | All native suites passed on stable source: 748 renders, all 174 legacy scenario names retained, 100 settled-resource cases and repeated navigation across both themes; teardown retained zero tracked allocations |
| Independent review | Component and integration findings were repaired and independently retested. Publication requires the exact whole-tree review inventory to match the final files; detailed private reports remain local |
| Hygiene | First-party text/local-link checks passed for 484 files; diff whitespace passed. Publication excludes private recovery images, credentials, recordings, generated binaries and local evidence |
| Target build | Full ESP32-S31 firmware build and size analysis passed with the pinned SDK, BSP and resolved dependencies; this did not enumerate or access a device |

The built application is **4,655,312 bytes** (`0x4708d0`), leaving 26 percent of
its six-MiB slot free. Its SHA-256 is
`601827c21e672e5dcba965ee4e8ea7d65a895564f73e4dfa83d8441f18344b24`.
The generated configuration SHA-256 is
`c55cc206f4a9e1e86f91934b4d120faca948d042e849c1f79ac161fa47c6c80c`.
SDK commit `383dda6c182e3dabf6355160166fe007df775629`, BSP commit
`5f10831ddf8697680121e72ba0fd3dce7f7f1799`, the exact reviewed pairing patch and
`firmware/dependencies.lock` identify the dependencies. The generated SDK and
actual X.509 compilation enable certificate date validation. Rollback uses
application-controlled confirmation, with startup auto-confirm disabled.

Static DIRAM use is 301,378 of 503,744 bytes, leaving 202,366 bytes before runtime
allocations. This is linker evidence, not a measured free-heap or endurance
result. The bounded native review pages and presentation buffers use external
RAM; their successful host allocation checks do not qualify physical PSRAM.

The operation catalog has 147 entries and 77 configuration metadata fields;
16 native preference keys are currently writable through the shared path.
Catalog presence does not imply runtime support. Initial-v1 event subscriptions,
playback pause/resume/seek, recording pause/resume, destination export and target
screen observation remain explicitly unsupported. Finalized recording download
uses the authenticated binary HTTP resource. Spoken input is the confirmed
letter-candidate workflow, not general dictation. Actual recording uses explicitly
prepared encrypted NOR storage; unknown external NAND remains preserved.

No flashing, USB/serial enumeration, pairing, recording on hardware, radio test
or device test was performed for this checkpoint. Host interoperability, acoustic
quality, physical motion/touch behavior, partition migration, power-loss recovery
and long powered operation remain separate qualification gates. The v3 layout
needs a separately authorized migration on an earlier-layout device; ordinary
OTA cannot perform that migration.

### Cabled v3 migration and failed startup: 2026-10-22

The user authorized flashing and explicitly waived a new backup. The existing
recovery image was retained. The connected target identified as ESP32-S31 rev0
with 16 MiB NOR, secure boot disabled and flash encryption disabled. Its installed
table contained NVS at `0x9000`, PHY at `0xf000` and a four-MiB factory slot at
`0x10000`. The entire new 64-KiB control region was verified erased before boot.

The five frozen artifacts from source commit
`1b36bc0811cee117612952998f871427b2ac4cb6` were written and individually verified,
with the partition table last. Readback confirmed NVS/PHY and control unchanged
before reset. No whole-chip erase, external NAND access or eFuse write occurred.
The first bootstrap marks OTA_0 valid directly; this is not application health
probation or proof of a working fallback image.

On macOS 27.0.1 (26A434), the device enumerated as USB HID plus one-channel USB
audio input/output at 48 kHz. This proves enumeration only. The user observed
the application boot screen at `Loading preferences`, followed by repeated
restarts. The first serial capture failed
after 2.273 seconds with an I/O error, zero bytes and no health samples. A later
bounded reconnect attempt observed no runtime CDC interface. Input, acoustic
audio, connectivity, UI behavior and powered endurance have not passed this gate.

Exact target compilation identified an independent stack-budget defect:
`app_main` reserves 5,472 bytes and its `render_view` child reserves 2,720 bytes,
exhausting the configured 8,192-byte main stack before caller or deeper callee
frames. No panic trace was captured, so this evidence does not alone attribute
the observed reset. Preserve this failed image and evidence when validating the
repair; a later source or build pass cannot replace a successful device run.

### Startup and coordinator stack repair: 2026-10-22

The repair separates startup from the non-returning coordinator loop and moves
large copied observations and requests into bounded owner workspaces. BLE and
Classic bond readers provide direct copy-out APIs. Media and connection polling
keep incoming observations separate from retained state, and transient private
payloads are cleared after consumption. Remote device execution uses a
per-instance workspace with callback-reentry rejection; accepted jobs and staged
transactions retain independent copies. BLE target submission validates the
complete request before replacing the worker-owned candidate under its lock.

Independent review found a second real overflow in remote BLE target selection:
the previous known nested frames exceeded 8,192 bytes before SDK overhead.
The repair also removes the large target-request and candidate stack copies.
The stack-budget checker covers the named callback chains, compiler split bodies
and nested helper paths, with a separate 2-KiB SDK/library/interrupt reserve.
Its declared-path result is not an exhaustive whole-program or runtime bound.
The configured main stack remains 8,192 bytes; increasing it is not the repair.

The final candidate passed the complete host regression, including 147 coordinator
cases and the added request-lifetime, callback-reentry, copy-out, stale-state and
failure-preservation cases under sanitizers. The 748 native UI renders remained
pixel-identical to the approved reference. The expanded target budget passed
28 declared paths: remote BLE target submission uses 4,000 known bytes, and the
largest declared path uses 5,024 plus the 2,048-byte reserve, leaving 1,120 bytes
beyond that reserve. The startup frame is 320 bytes. These are compiler results,
not observed device watermarks.

The full pinned ESP32-S31 build and size check passed with no drift across the
417 captured firmware/component/test files. The application is 4,654,752 bytes,
SHA-256 `662df63b385ed62bdabab7224965724bd91e4f093b633663daa0ddba15856d99`.
The SDK, BSP, reviewed pairing patch and configuration match the checkpoint
above. Static DIRAM use is 302,722 of 503,744 bytes, leaving 201,022 before runtime
allocations; linked coordinator and UI observation workspaces reside in external
RAM. Repository text/link and diff-whitespace checks passed.

The app-only repair procedure verifies the frozen application and installed v3
table before writing OTA_0, then verifies the application and unchanged NVS/PHY
and table before reset. It does not perform a new backup. The separately retained
failed-image evidence above remains part of the device acceptance record.

The application-only candidate was written and verified against that hash;
NVS/PHY and the installed table remained unchanged. A completed 90.005-second
capture contained eight health samples with increasing uptime from 12 to 83
seconds and a minimum observed main-stack watermark of 4,172 bytes. No reset was
observed in that capture. The health gate nevertheless **failed**: display SPI
transfers reported `ESP_ERR_NO_MEM`, Wi-Fi worker startup failed, and I2S DMA
allocation and media initialization failed. Runtime internal free memory was
4,083 bytes; DMA-capable free memory was 167 bytes with a largest block of 40.
The first recorded allocation failure requested 8,190 internal DMA bytes.
The user saw the product interface with the boot logo still over it. USB audio
input/output enumeration at 48 kHz did not establish working acoustic audio.

An IMU `batch_time` timeout stopped the stream after 16 samples and prevented
calibration. Its cause is not established by the memory failure and requires
separate verification. This candidate is not device-qualified; publication is
held while the physical refresh and resource failures are investigated.

### Display and combined resource repair candidate: 2026-10-22

The resource repair bounds the board display queue without changing its full
viewport, TE synchronization or completion callback. Audited task-only UI,
coordinator, subscription and audio scratch buffers move to fixed external BSS.
Supported I2S payloads and TLS allocations use PSRAM, while selected flash-free
network/clock tasks use external stacks. Flash/NVS/OTA owners, locks, TCBs and
interrupt data retain internal memory. The [runtime memory plan](../design/runtime-memory.md)
records capacities, cleanup costs and the remaining coexistence qualifications.

A separate IMU correction discards only finite, monotonically timed batches
that exceed 20 ms while the prior accepted sample still meets the original
50 ms freshness limit. It never retimestamps or publishes the discarded batch.
Bus errors, clock rollback, expired samples and raced consumer faults still
stop the stream. Duplicate timestamps do not advance calibration or emit HID.
Rejected-batch counts remain observable.

Fresh configuration from tracked defaults and the full pinned build passed.
The 4,649,952-byte application SHA-256 is
`7cbe4c5a013dd82112a7b76a3caa9a929e57e27e0de2c43776e6f6bb407cc2b5`;
configuration SHA-256 is
`f927e2539734a1b8e969efb45bab19328cd5e63b6695792a42cbac40bfef51bc`.
The installed partition table and model image are unchanged. The full host
regression passed without drift across 459 captured firmware, component, test
and tool inputs. All 748 UI images match the approved baseline.

Final linked static DIRAM use is 236,324 of 503,744 bytes, down 66,398 from the
preceding stack-repair image. External BSS is 285,688 bytes. The optional Wi-Fi
speed sections are no longer in internal IRAM, and the audited buffer symbols
are in external BSS; queue control and locks remain internal. These figures do
not establish measured heap/DMA headroom. The initial target link correctly
failed because the wrapper was not extracted from its static archive; an
explicit symbol anchor fixed that integration issue before this successful
build.

The exact application was subsequently written and verified; the installed
table and NVS/PHY remained unchanged, and no new backup was performed. The
90.010-second capture contained nine health samples with uptime increasing from
23 to 103 seconds, and a minimum main-stack watermark of 4,176 bytes. Bluetooth
was initialized, connected and ready. IMU samples increased from 1,824 to 9,101
with completed 64-sample calibration and two retained late batches. No reset,
display allocation error or I2S error was observed in that captured window.
The user confirmed that the boot logo disappears completely, Menu and mode
switching and touch work, and no restart is observed. This establishes those
physical display/navigation observations, not host pointer or audio behavior.

The device gate nevertheless **failed**. Intermittent battery telemetry
`ESP_ERR_TIMEOUT` warnings remain, and the new runtime diagnostics expose invalid
audio state/error values (`0xa5a5a5a5`) and an invalid off-confirmed value (`0xa5`)
despite the snapshot reporting known audio state. Audio readiness cannot be
inferred from initialization or USB enumeration. Wi-Fi initialized in idle
station mode; no LAN join or server session was tested. Steady internal free
memory was 20,847 bytes with a largest block of 10,752, and DMA free memory was
11,415 bytes with the same largest block. This remains below the existing Wi-Fi
scan admission limits. The user also observed a failed Wi-Fi scan. The memory
gate is consistent with that result, although a scan-specific driver error was
not captured. These observations require further owning-layer repair
and coexistence validation; this image is not device-qualified and publication
remains held.

### Audio, battery and scan-resource follow-up: 2026-10-22

The audio repair checks initialization before publishing any engine snapshot.
Explicit alignment queries and aligned allocations correct a source-proven
contract mismatch: the pinned generic PSRAM allocator can return four-byte
alignment, whereas the linked audio engine requires eight. This explains a
possible rejected initialization, followed by the previously observed stack
poison publication; the original device allocation address was not captured.
Matching engine/store, remote adapter/bridge and network object allocations
are corrected at their owning boundaries. Weak-alignment regressions exercise
rejection, correct allocation, failure cleanup and retry. Late completion
notifications no longer borrow released audio state, and task-handle publication
uses the same owner lock as readers. Valid hardware wait/fault recovery remains.

Battery reads now follow the BQ27220 documented incremental-read and timing
requirements, with three bounded packets instead of six. The original 20-ms
whole-batch limit, 1-Hz polling and five-second freshness remain. Read, batch and
clock failures retain truthful diagnostics; no automatic retry or hidden stale
success is added. Native regressions cover legal clock stretching, partial
failure, overdue batches and recovery. Physical timeout recovery remains open.

The audited board audio and media control tasks use two external 6-KiB stacks
on the supported profile. The Wi-Fi static TX pool is reduced from eight to
four; existing scan admission limits and transport backpressure are unchanged.
The [memory plan](../design/runtime-memory.md) records the projected benefit and
the remaining combined-operation peaks. A projection is not device headroom.

Fresh configuration, full target build and host regression passed without drift
across 467 captured source/test/tool inputs. The regression includes 149
coordinator cases and 35 serial-parser tests, which independently reject the
observed poisoned runtime fields while retaining earlier error detection.
The target stack budget passed all 31 declared paths with a maximum of 5,024
known bytes plus the existing 2,048-byte reserve against an 8,192-byte stack.
Linked static DIRAM use is 236,332 of 503,744 bytes; this excludes runtime costs.

The application is 4,650,416 bytes, SHA-256
`bda4e94e0203b44f7d5e006a30a445b01ff37c4ccae9fc57d56ae33431d24641`;
configuration SHA-256 is
`39b6b39f809b5ad47714a5e159086f1c405481d847b02c94ac09642793d4377c`.
The partition table and model image remain byte-identical. Independent review
approved the exact 139-file source inventory and application-only device test.
The application was written and verified, preserving NVS/PHY and the installed
table without a new backup.

The completed 90.002-second capture contained nine health samples with uptime
increasing from 23 to 103 seconds and a main-stack watermark of 4,176 bytes.
All nine media observations were valid: audio OFF/OK with hardware-off confirmed,
and speech IDLE/OK. USB audio input/output enumerated at 48 kHz. These results
resolve the invalid snapshot observation; they do not establish acoustic audio
or recognition. The user confirmed that Wi-Fi scanning lists networks and that
the device successfully joins an existing LAN. The final two Wi-Fi observations
also reported CONNECTED with LAN ready. The control service remained stopped
with no provisioned trust; LAN connectivity does not imply API readiness.

The device gate nevertheless **failed**. All nine Motion observations retained
only 20 accepted samples, with no new samples throughout the capture and no
calibration progress. The repeated first-fault record identifies one earlier
`batch_time` timeout at uptime 3,148,860 microseconds, not nine separate faults.
Source inspection shows that a late batch with an expired cache permanently
stops and cleans up the sampler. The user independently reported Motion
unavailable. Two battery warnings were whole-batch timeouts of 38,553 and
23,694 microseconds after successful packets, not packet-read timeout errors.
The source of those scheduling or bus delays remains under investigation.

Observed free internal memory ranged from 37,407 to 43,423 bytes, with a largest
block of 25,600; DMA free memory ranged from 11,719 to 17,735, with largest blocks
of 8,192 to 16,384. Because the IMU worker had stopped, these measurements cannot
qualify simultaneous healthy sensor, radio and audio operation. Publication is
held for sampler recovery and timing repair. Physical pointer behavior, active
acoustic paths, authenticated services and powered endurance remain unverified.

### Sensor recovery and unified keyboard candidate: 2026-10-22

The next candidate replaces permanent sampler shutdown on transient acquisition
or freshness failure with bounded recovery. IMU and battery reads share one
worker, with IMU first and missed periods skipped. A single Bosch frame supplies
readiness and both sensor vectors. Clock rollback, invalid configuration and
other structural faults remain terminal. The 20-ms batch and 50-ms freshness
limits are unchanged; stale or rejected frames never advance accepted samples.
Recovery requires fresh data, resets the motion reference and does not silently
re-enable the user's Motion switch.

Keyboard now exposes a persistent Keys entry in every submode. Its aligned panel
provides Copy, Paste, Undo, Backspace, forward Delete, Enter, Esc, Tab, Home, End
and four arrows. Copy/Paste/Undo use the selected Ctrl or Command host convention
and send complete key-down/release pairs without changing the local draft.
Submode, Keys and Menu captions share the same 16-pixel font and baseline in both themes.

The full host logic/protocol run passed, including 151 coordinator cases and
54 health-parser cases. The native LVGL run passed 768 renders,
retained all 174 original scenes, and checked 100 resource cases. Each theme
completed 1,000 panel cycles without retained growth; core/peak object counts
were 106/148 under the unchanged 160-object ceiling. Unchanged frames caused no
extra refreshes, and final teardown retained no tracked allocations.

The target application is 4,651,040 bytes with SHA-256
`04a8ec591cb24b4373047ed2beb93ae445736095b34d3e3497700e97a35a2007`.
The complete build, size and coordinator stack-budget checks passed with 148
unchanged target inputs. The sensor retains a 4-KiB internal stack; its measured
944-byte call chain plus engineering reserve is a source budget, not a complete
SDK bound or measured sensor-stack watermark. See the
[runtime memory plan](../design/runtime-memory.md#persistent-sensor-worker).

Independent review approved the exact 149-file inventory and application for
app-only device testing. The application was flashed and its digest verified;
NVS/PHY and the installed partition table were preserved, with no new backup.
The runtime USB device re-enumerated.

The completed 90.001-second capture still **failed**, retaining one startup IMU
fault at uptime 3,103,083 microseconds. The maximum acquisition batch was
69,358 microseconds. The capture began later, so it cannot identify which startup
operation caused that delay. The sticky fault must not be cleared or treated as
a passing gate merely because sampling recovered.

Recovery was observed: all nine Motion records reported an active, live worker,
available IMU and `ESP_OK`; accepted samples increased from 2,070 to 10,072 with
no stall or regression. Sample ages were 1,950..9,469 microseconds and calibration
had completed 64 samples. The recovery count remained one and late-batch count
two throughout the window. Current batches were 1,162..1,223 microseconds, with
zero reported schedule lateness. No new battery warning was captured; this does
not establish that battery reads never fail.

All nine Wi-Fi records reported CONNECTED/LAN ready. Bluetooth remained connected
and ready; audio remained OFF/OK and speech IDLE/OK. Internal free memory was
38,791 bytes with a 25,600-byte largest block while the sampler was alive; DMA
free memory was 12,971 with a 9,216-byte largest block. The coordinator stack
watermark was 4,112 bytes; sensor stack watermark remains unmeasured. Main uptime
increased from 23 to 103 seconds. These observations do not qualify connected
scanning, physical pointer/host keys or active audio.

Device acceptance remains failed because of the retained startup fault, with
physical checks still outstanding. Source publication does not change that
result. Qualification explicitly requires Motion records, a live worker,
increasing samples and sample ages within 50 ms. Legacy logs without Motion
records remain readable but cannot satisfy this candidate's device gate.
Previous failed captures remain failed.

### Repeat device test and physical failures: 2026-10-23

The same `04a8ec59...` application was reflashed after all 148 target inputs,
467 host inputs and five frozen artifacts were verified against the reviewed
package. The application digest, partition table and NVS/PHY preservation were
verified again, without a new backup. macOS 27.0.1 registered the USB keyboard
and mouse drivers and one input/one output audio interface at 48 kHz. This is
interface evidence, not host-key delivery or acoustic qualification.

The immediate 90.005-second capture contained eight health observations and
still **failed** the unchanged error-log gate. Its two error lines came from
GPIO ISR service installation after the TE display path had already installed
that service. The pinned touch and button callers explicitly accept
`ESP_ERR_INVALID_STATE` and continue registering their handlers. Those lines
alone do not establish failed touch hardware, but the original failed capture
is retained without filtering or changing the acceptance criteria.

All eight Motion records were ACTIVE, available and calibrated, with a live
worker and accepted samples increasing from 1,069 to 8,073. Sample ages were
511..9,138 microseconds. There was no latched first fault or recovery in this
boot, but one IMU batch exceeded 20 ms (maximum 24,589 microseconds) and was
discarded. The previous accepted cache remained within its 50-ms budget, so
continued ACTIVE state is consistent with this rejected batch. Wi-Fi and
Bluetooth remained connected. The earlier boot's 69,358-microsecond delay and
its cause remain unresolved.

Physical feedback nevertheless **failed** the product behavior: Motion initially
moved abnormally and stopped after approximately two seconds while its switch
remained on. The touchpad could still move the same host's pointer. Wi-Fi Scan
reported Failed on one attempt and displayed a refreshed network list on a
later attempt with unchanged firmware. This establishes intermittent failure,
not a permanent parameter rejection or a repaired scan path. The user also
found the Keys panel's explanatory caption confusing; host-key reception was
not confirmed.

A subsequent capture was interrupted by a user-confirmed unplug or manual
restart after 15.562 seconds; it is incomplete, not evidence of a spontaneous
reset. After reconnection, a 45.001-second capture passed its bounded serial
checks with five fresh Motion observations. Neither that pass nor sensor
availability clears the physical Motion and scan failures. Investigation must
distinguish acquisition, consumed-sample timing, reference validity, pause
conditions, generated motion and HID admission/delivery. Exact scan failure
phase and error were not available in these logs.

### Reference-loss and physical-axis observations: 2026-10-23

Reviewed source `327c8c624edb2c86ab726c3521b657f14cdfd1f8` was published with
tree `a4979ca7489e08087d1e7233e3917a4e35c3d219`, then its 4,654,688-byte
application (`19b2605166104db54ed084bcb20b59da333ff8711769df5c2cb0fc4ffb79b7be`)
was written and separately digest-verified. The table and NVS/PHY were preserved
without a new backup. This candidate adds the symmetric fifteen-key panel and
observation of consumed samples, control gates, reference state and scan stages;
it does not change the Motion estimator or Wi-Fi scan policy.

Its immediate 90.002-second capture still **failed** on the two retained GPIO
ISR-service error lines. The eight observations showed fresh, calibrated IMU
sampling, no recovery, and a maximum consumed-sample interval of 40,883 us.
Only the last two observations had Motion enabled. A subsequent 90.005-second
capture passed the bounded serial checks but captured two active-Motion
observations with both engine and model enabled, input ready, no instantaneous
control gate, `reference=0`, and reference suppression. Movement admissions
stopped increasing between those observations and increased after the reference
recovered. Lifetime consumed-sample gaps, sample blocks and clock backsteps
remained zero. This establishes reference suppression at the observed instants;
it does not identify which acceleration/trust predicate invalidated reference,
prove continuous suppression between records, or establish host receipt.

The user still reported fast tilts producing no pointer movement, slow side
tilts moving vertically, and no movement from forward/back tilts or upright
hand-held use. The first two actions of a compound orientation test were later
reported incorrect. Their physical labels are withdrawn; the raw records remain
available locally. Mixed ten-second extrema cannot reconstruct paired sensor
samples, exact transition times or acceleration norms.

A separate confirmed pose placed the screen facing the user with text upright.
Three settled windows in a 45.005-second capture reported mapped acceleration
X between -0.851 and -0.816 g, Y between -0.032 and 0.023 g, and Z between 0.502
and 0.562 g. This is a tilted upright pose, not a measured exact 90-degree hold;
the preceding moving window is excluded from the static comparison. Together
with the confirmed reading orientation and screen-up positive Z, the result
supports removing the adapter's extra 90-degree transform: the corresponding
native vector is approximately (0, +0.84, +0.53) g, consistent with display
Y pointing toward the top of the screen. The factory demonstration's Euler/UI
labels alone had not established this body-frame mapping.

A common proper rotation of acceleration and gyro preserves the reference
estimator's norms, dot products and propagated change. Correcting screen axes
therefore does not by itself resolve the separately observed reference loss.
In a later scan capture, the user reported three successful requests. Diagnostic
generations 1, 2 and 3 completed with driver/read counts of 15, 15 and 14, SDK
event status zero and connected LAN state. No failure was retained in this boot.
These successes do not identify or clear the earlier intermittent failure.

The changed adapter, automatic grip recovery, pointer directions and fast-motion
response still need validation on the resulting image. No physical Motion,
intermittent scan, audio or endurance qualification is claimed by these captures.

### Grip candidate device result and causal replay: 2026-10-23

Published candidate `7572c1f8d0e394ee5dc7e5cf46c3b401f508a6d8`, application
SHA-256 `f6a3f52d9fd21ac02aa4f98149a442816560cbce8729f1fc2fef8b63bb7fb8eb`
(4,658,768 bytes), was app-only flashed and independently digest-verified.
Partition table and NVS/PHY remained unchanged; no new backup was made.

The immediate 90-second capture failed on retained GPIO ISR-service errors.
Its eight observations had Motion off, so healthy acquisition did not qualify
pointer behavior. A later 120-second capture passed bounded serial checks but
retained six active reference losses. The user reported uncontrolled, unintuitive
movement: the product Motion gate remains failed. The user's successful
Recenter without a jump was a separate observation after that capture, whose
Recenter counter remained zero.

Exact first-rejection and terminal frames were replayed through the published
reference function with strict compilation and sanitizers. The first frame
failed near-one-g and gyro-compensated acceleration-change predicates; the
terminal frame passed near-one-g but failed acceleration change and reached a
250,313 microsecond trusted-correction age. Innovation was not evaluated on
those two paths. A later 50,871 microsecond consumed-sample gap occurred after
the terminal frame and cannot explain the first loss. The fourteen intermediate
frames were not retained, so the replay does not reconstruct the full gesture.

The observed predicates match the implementation; the physical cause of the
acceleration changes remains unproven. The new [handheld movement requirement](motion-estimation.md)
replaces direct angular Mouse output. It does not convert the failed candidate
into a repaired or qualified device.

### Developer and Git identity source checkpoint: 2026-10-23

Reviewed source `62117195f5934e7789ef6b45a8ba91d110441091`, tree
`21434f0511fb1fc5522c761de83a68668fb5ed90`, was published and verified against
GitHub main. Its clean rebuild embeds that commit, clean state, project developer
and repository in startup diagnostics and About. The frozen application is
4,659,984 bytes, SHA-256
`eb0f67d6f965580e66f58a9d71582a72e1c9d2a1ca8699aaacbb0bdbb4c73fb3`.
It has not been flashed or physically inspected.

Full host checks passed before a URL-validation correction; the affected seven
generator tests and 159 coordinator cases passed again afterward. Independent
review closed the build/UI URL grammar mismatch using 84 shared boundary cases.
Native validation passed 102 renders/hit-test scenes, 116 resource cases and
1,000 navigation cycles per theme without retained allocations. Actual ESP-IDF
build and no-op checks passed; an isolated exact CMake dependency block also
passed eight real-Git transition stages. Stack budgets passed with the unchanged
2 KiB SDK/ISR reserve. These are source/host/target results only: metadata does
not alter or qualify Motion, audio, Wi-Fi failure recovery or endurance.

### Live sensor API source integration: 2026-10-23

The [sensor API](sensor-api.md) adds a 256-record, 88-byte-per-record raw journal
at the existing acquisition owners, with independent cursors over USB CDC and
authenticated station HTTPS. The catalog appends operations 149..151 without
changing the previous 148 entries. The diagnostics scope does not grant HID,
settings, microphone capture or enrollment authority. Input observations redact
private coordinates and invalid/overflow snapshots before publication.

The full host regression passed, including 160 coordinator cases and 163 with
diagnostic capture enabled, production board/USB/HTTP adapters, explicit ring
gaps and exhaustion, immutable reads, codec corruption checks and private-touch
redaction. The production LVGL visual suite passed with 800 renders and actual
pointer-driven private-editor/deferred-close checks. No product UI was redesigned.
These are host boundaries, not hardware throughput or physical input evidence.

The pinned ESP32-S31 build passed with a 4,675,776-byte application and 26 percent
of its six-MiB slot free. Its 92 declared stack paths passed with the existing
2-KiB reserve and 512-byte startup-frame gate; the measured startup frame was
336 bytes. Independent sensor-component review approved the frozen source and
evidence, including the collector's 21 tests. Later Bluetooth and Audio edits
require a new combined build, stack check and review before publication. USB and
authenticated LAN sensor capture on hardware are not yet qualified. This
integration does not enable the experimental handheld-translation estimator or
repair the earlier Motion result.

### Ordered capture deployment and repeated Waiting: 2026-10-23

Reviewed source `812ec0ebe6551c77a2792b3bdbb15201d76e8920`, tree
`4a584a072a7f213e79e8796b626775eebb04dbe7`, was published and rebuilt cleanly
with `CONFIG_APP_IMU_CAPTURE=y`. The 4,663,728-byte application SHA-256 is
`955a9479756f8203747d91710bbc2f92b55b5097f4c9450d7062f37f6b9c31bd`;
its ELF SHA-256 is
`3a3a914d4e1d7e3961a576470761720f59103f3954892c1c99d1fc82707279a7`.
The application-only write and independent digest verification succeeded.
Partition table and NVS/PHY bytes remained unchanged; no new backup was made.

This image adds an ordered diagnostic observer. The existing latest-value angular
Mouse path remains active; the unqualified handheld translation experiment is
not linked into firmware. Full host checks, the 162-case capture coordinator
suite, eight reader cases, target compilation, 51 declared stack paths and
independent source/package review passed before deployment. These checks do not
qualify physical movement.

The immediate 90.001-second health capture contained eight fresh, advancing IMU
observations, completed calibration, and ready Bluetooth. It still **failed**
on the two retained GPIO ISR-service installation errors. The traced touch and
button callers accept the already-installed service and register distinct
handlers after the display's initial installation. This does not change the
failed capture, or establish interrupt behavior during flash/cache operations.
One acquisition batch had exceeded the 20-ms limit by the first periodic
observation (maximum 24,722 us);
periodic accepted-sample ages were 1,432..9,148 us with no recovery or latched
source failure in those observations. Minimum observed free internal memory was
38,063 bytes; coordinator stack watermark was 4,160 bytes. No long-duration
resource or timing qualification follows from this bounded run.

The user reported repeated Waiting and jerky movement. The retained first
terminal incident occurred at sample time 43,700,793 us, exactly 250,001 us after
the last trusted acceleration correction. Both its first rejected and terminal
frames failed the near-one-g and gyro-compensated acceleration-change checks;
innovation was not evaluated on either frame. The old reference policy expired
the grip, suppressed movement and required a new stable interval. Later periodic
Ready observations do not erase these outages. Producer sampling continued near
100 Hz while the legacy latest-value consumer observed about 66 Hz; zero legacy
gap alarms only exclude intervals above 50 ms, not skipped publications.

A subsequent complete export identified the exact clean image and contained
638 sensor frames plus one initial intentional fence, in a 6.380-second capture
window (6.370 seconds between the first and last samples).
It had no sequence break, new dropped publication, source failure or stale
observation. Read-completion intervals were 7,800..12,198 us; 16 of 637 intervals
fell outside the experiment's fixed 100 Hz +/-5 percent assumption. The complete
ordered segment is usable as raw evidence, but not compatible with that fixed
cadence hypothesis. Its unstructured operator actions provide no exact physical
trajectory or verified initial rest. No frames were retimed or discarded.

A prompted lateral-motion trial then exported 1,541 sensor frames and six
markers in a 15.506-second capture window. Wire completeness passed, but ordered
segment usability failed: five internal REJECTED boundaries divided the trace,
and four previously accepted publications were flushed by the delivery queue.
The fifth rejection found no buffered accepted publication. Bracketing unchanged
late-batch counters distinguish these from over-budget reads: the board had
observed a missing gyro and/or accelerometer data-ready bit. The exact missing
bit was not retained at each event. Cached hardware remained available; this is
not an overflow, stale-observer or communication-fault diagnosis. The operator's
reported action sequence is an intent label, without a synchronized external
trajectory or measured stationary interval. It cannot qualify a replacement
estimator or be repaired by retiming/discarding the discontinuities.

The same boot observed LAN connectivity but an unprovisioned, stopped control
server and unknown clock. It does not qualify Wi-Fi management, OTA or raw-log
download. The [LAN development workflow](wifi-management-ota.md#development-update-and-diagnostic-workflow)
records the requested next access path and its outstanding implementation and
device gates. Raw logs, device identifiers and per-run artifacts remain local.

### Bluetooth discovery and USB audio investigation: 2026-10-23

The device still ran the verified `812ec0e` application during this investigation.
The user reported inconsistent saved-host deletion and pairing, then successful
Windows pairing but no iPad discovery after removing the iPad-side record.
Native Add host returned to the list without a persistent pairing-status page.
Source inspection found that native single-host deletion was not wired and that
Add/Forget all did not complete the durable target-policy transition required by
the BLE owner. A saved selected target can therefore block a later pairing window.
The repair must use acknowledged owner transitions, preserve unrelated hosts,
and show actual advertising state rather than equating an accepted request with
discoverability. These findings do not establish the result of a repaired radio
test.

An initial 90-second health capture passed with advancing IMU samples and BLE
initialized but disconnected. A later 90-second capture reported BLE connected
and input-ready; the peer identity was not established. That capture failed on
one retained IMU `batch_time / ESP_ERR_TIMEOUT` incident between captures, with
a maximum observed batch of 66,204 us and one recovery. Subsequent samples were
fresh, but the historical fault remains a failed health gate. Repeated reports
of the same first incident are not new incidents. The duration includes driver,
scheduling and lock time; it is not a measured I2C wire transaction or evidence
that Bluetooth caused the delay.

macOS enumerated the USB input/output audio interfaces. A bounded, output-only
probe selected that exact USB device, left system defaults unchanged and rendered
4,180,480 mono frames at 48 kHz. This proves host callback activity, not USB OUT
receipt or sound from the speaker. The simultaneous 120-second capture observed
Audio off throughout and retained the same earlier IMU fault. Ten-second
aggregate snapshots cannot explain the user's reported two-second audio stop.
Per-job first-frame/terminal state and cached transport/codec observations are
needed; no startup deadline or failure criterion is relaxed to conceal the issue.

A later 150-second replay with host output active again retained 15 Audio OFF
observations. The operator clarified that Start audio was gray and could not be
pressed. This attempt therefore did not exercise codec playback; the immediate
question is the local admission reason, distinct from the earlier Windows report
of stopping after two seconds. The new primary controls must expose their actual
unavailable reason without relaxing admission. A separate Windows touchpad report
describes USB lag and Bluetooth cursor jumps with Motion explicitly disabled;
that transport/input investigation remains open.

A production coordinator/interaction/HID host reproduction then confirmed a
lossy transient-busy boundary on both USB and BLE. Three relative movements of
20 units emitted 60 with an idle transport but only 40 when the middle sample
arrived during an in-flight report. A touch beginning during the same transient
busy state was canceled and emitted no movement after readiness returned. A
separate 300-unit sample emitted 127 with no retained remainder. These sanitizer
reproductions establish source defects; they do not measure their frequency or
end-to-end latency on Windows. The repair must retain bounded relative movement
across temporary backpressure while preserving disconnect, ownership, release
and button-ordering barriers.

### Combined sensor, connectivity, Audio and touch candidate: 2026-10-23

The combined source repairs native Bluetooth add/delete/policy transitions,
provides the simplified independent Mic/Speaker controls and their actual Start
admission reasons, and binds local audio commands to the original observed peer
and stream token. Cached audio diagnostics observe role, job, transport and codec
state without starting capture or exposing PCM. The raw sensor API and collector
are included for the next physical Motion investigation.

Touch movement now retains bounded relative displacement through temporary
USB/BLE backpressure and splits large deltas into the existing wire reports.
Independent review caught an explicit remote Cancel path that could leave a
partial displacement queued. The corrected path discards that remainder through
the existing ownership and neutral-release lifecycle; normal End still drains
accepted movement. The new production regression fails before the repair and
passes for cancellation before/after a partial report, while busy and while a
button is held. The standard host runner now includes these touch-pointer tests.

The final combined host suite and target build passed after that repair. All
296 declared stack paths passed with unchanged task budgets, the 2-KiB reserve
and a 336-byte startup frame under the 512-byte gate. The native all-suite run
passed 842 renders, all 174 retained baseline scenes, actual pointer hits and
presenter integration. Its 138 idle/hidden cases and 1,000 cycles across 41 panel
variants per theme retained no growth: core/peak objects were 112/154 under the
160-object panel limit, with zero live allocations after teardown. Representative
frames received visual review. Focused and full runs have different inherited
HID/draft fixture states; no cross-suite pixel equality is claimed. A later
documentation-only touch contract paragraph did not change UI sources or tests.

Independent component reviews closed the reported findings. These host and
compile results do not qualify audible playback, microphone capture, iPad
rediscovery, Windows pointer latency, physical sensor-export throughput or
powered endurance. The existing angular Motion algorithm remains unqualified;
this candidate does not enable the experimental handheld-translation estimator.

### Combined candidate device failure: 2026-10-23

The clean `495a8e1` image was written to the application partition and verified
independently. Settings, PHY data and the partition table were preserved; no new
full backup was made. A read-only USB sensor response subsequently confirmed
that exact clean Git identity and the packaged ELF identity on the device.

The 90-second health capture failed. It retained two duplicate GPIO ISR-service
installation errors during startup and a later media fault. During USB audio
startup, the codec reported prepared at 48 kHz, mono, with both roles supported.
A capture job entered Starting, then the engine entered `AE_FAULT` with
`AE_TIMEOUT`. Capture and playback had no first frame and zero delivered bytes.
The codec retained SDK error 263; cleanup confirmed audio off and retained
quarantine. The user also observed Timeout in USB mode. The diagnostic did not
identify the failing SDK operation, so this record does not uniquely attribute
the timeout to I2S, codec control or the host. Host output callbacks alone do not
establish audible playback, and a host-selected capture interface does not
establish a microphone grant.

The same capture observed advancing, fresh IMU samples, connected Wi-Fi and
ready Bluetooth HID. These observations do not resolve physical Motion,
pointer latency, iPad discovery or audio compatibility. The authenticated LAN
control service remained unprovisioned and stopped.

A separate 30-second read-only USB sensor collection verified 684 responses
and the running image identity but exported zero sensor records. Every sample
page reported a retention gap while the producer advanced; the collector
retained the gaps and marked the data unusable. This is failed physical
sensor-export evidence, not successful Motion data collection.

The user later reported that Add host was disabled. A further 45-second capture
confirmed `ble_failed=1`, disconnected and not ready, while preserving the first
failure: Advertising start completion, `ESP_FAIL`, SDK status 263, at uptime
660,845 ms. Earlier samples from this same boot had shown ready Bluetooth HID.
This is a real transport failure, not merely an inactive button. The Bluetooth
completion status belongs to its own SDK status domain: the pinned SDK maps
263 to `ESP_BT_STATUS_HCI_MEMORY_FULL` (HCI Memory Capacity Exceeded), not an
audio or generic SDK timeout. The pinned host command path also synthesizes this
HCI status when command-buffer allocation fails, before sending to the
controller. The retained status therefore identifies the advertising command
failure, not which side exhausted a resource or its triggering lifecycle. No bond records
were deleted, and no restart was used to replace the failed observation.

### Audio clock and live sensor-read repairs: 2026-10-23

The cold-microphone regression reproduces a lifecycle defect in the preceding
production adapter: the pinned SDK configures duplex RX as a slave, but the
adapter enabled RX while its TX master remained stopped. The clock-aware
hardware boundary returned zero bytes and SDK timeout 263 before the repair.
The repaired adapter starts a silent TX clock before capture, tracks it
separately from speaker permission and preserves it when only the speaker
stops. Playback data is purged before a later speaker start. Named SDK failure
operations remain observable after cleanup. These source and host observations
do not uniquely attribute the earlier physical timeout or establish audible
playback; the exact image still requires physical first-frame checks.
The board suite passes across the default and six alternative memory/interrupt
configurations with the pinned codec implementation. Production engine/adapter
tests cover cold Speaker without a capture consumer, both duplex start orders,
independent stops and restarts, stale PCM purging and failure cleanup. Seven
audio-diagnostic tests pass with first/last named SDK failures retained. No
startup, I/O or cleanup timeout was extended.

The sensor repair atomically returns the missing-prefix metadata and the first
bounded retained records from the same locked journal snapshot. The wire format
is explicitly version 2; the new collector retains strict version-1 decoding.
The fixed 256-record journal, 88-byte records, HTTP/USB page capacities and USB
frame limit remain unchanged. The actual journal, API, encoder, USB responder
and collector execute in a persistent advancing-producer regression. Using the
same final fixture and collector, the old production owner returns zero records
at both 40-ms and 160-ms read intervals. The repaired owner returns 72 records
plus 71 reported losses through cursor 143, and 21 records plus 138 losses
through cursor 159, respectively. Both remain marked discontinuous and unusable
for an unqualified continuous-data claim. The 25 collector tests and related
board, API and USB host checks pass; hardware throughput remains unqualified.

Bluetooth adds an eight-entry chronological advertising trace that freezes at
the first policy-classified fault. It records command and callback ordering,
explicit status domains, preceding host-memory observations and unknown or
overwritten context without recording host identities. Existing health logging
can replay this observation to a later USB reader. Independent review found and
closed a missing freeze in name-owned advertising completion errors; regressions
also preserve benign connection-stopped and late-drain behavior. The three BLE
host configurations pass their 100,000-cycle lifecycle checks, and the actual
coordinator replay checks pass. This is diagnostic support, not a repair for the
unresolved physical advertising resource failure. Pairing admission, retry,
rollback and fault retention policies are unchanged.

The resulting combined source passes the full host suite, target build and all
346 declared stack paths across 46 measured units. Input hashes remain stable
through these checks. Startup remains 336 bytes under the 512-byte limit;
task allocations and the 2-KiB SDK/ISR reserve are unchanged. New board-audio,
BLE-control and BTC callback paths use their actual owning task budgets; the
BTC SDK stack supplement is reported but excluded from the application budget.
An earlier successful target build is superseded by the reviewed diagnostic
correction. Product UI source is unchanged, so these checks do not claim new
visual or physical UI qualification. Independent component source/host reviews
approve the repairs and diagnostics; audible playback, microphone operation,
advertising recovery and physical sensor throughput remain pending a new image
and device observations.

### Clock-repair deployment and remaining stream failures: 2026-10-23

The reviewed clean `279e654` application was written and independently verified.
The 4,695,072-byte image SHA-256 is
`6dd50e6e2445ba8e7e0a503a64a7bae85b4b9397a782482ea0932d143b6369bc`.
The installed partition table and NVS/PHY bytes remained unchanged; no new
backup was performed. Runtime diagnostics confirmed that Git identity and the
packaged ELF identity. USB HID and mono 48-kHz audio input/output enumerated.

The 90-second capture retained eight increasing health observations and the two
startup duplicate GPIO ISR-service errors. The clock repair now allowed a
microphone job to deliver its first PCM frame; codec diagnostics reported no
SDK operation error. This is progress beyond the previous cold-start failure,
not a successful microphone qualification: the job subsequently ended Partial
with `AE_STOP_OVERFLOW / AE_OVERFLOW`, 278,400 received bytes, 270,720 delivered
bytes and eight discarded frames. USB capture rejection counts also increased.
No acoustic-fidelity or continuous-capture claim is established.

Three observed Speaker starts, including a cold Speaker-only start with capture
off, received zero playback bytes and timed out waiting for the first frame.
The USB interface remained selected and its connection generation was stable
during those attempts. Codec playback and its TX clock started without a named
SDK error, then stopped cleanly. The user confirmed a brief start followed by
Timeout. The output-only host probe rendered 4,646,912 frames, which proves host
callback activity rather than USB packet delivery or audible playback. Global
Audio OFF/OK after cleanup does not erase the retained failed playback and
partial capture jobs; the existing summary parser did not classify those job
failures, so raw per-job evidence remains authoritative.

During a later capture, the user selected a new Bluetooth host and disconnected
the current one. Advertising start completed with status 263 and the interface
became unavailable. The frozen trace retained the disconnect, start request,
successful API return and failed completion. An HCI command-response log for
opcode `0x200a`, status `0x07`, narrows this occurrence beyond the older generic
retained status. Neither available host heap nor the later printed disconnect
message proves the controller's resource or teardown state at the failure;
the pinned handler emits that message after invoking disconnect processing.
The underlying advertising failure remains unresolved in this image.

A separate 30-second USB sensor capture verified 660 responses and delivered
1,893 real records, including 1,870 IMU samples. This confirms that the repaired
gap response makes progress on hardware. Exact accounting retains 19,056 initial
and 1,137 ongoing missing records through cursor 22,086. The observed delivery
rate of approximately 63 records per second remains below the approximately
101-record producer rate. All 628 source-sequence skips are covered by explicit
retention gaps; no source epoch change or wire anomaly was observed. The data
remains discontinuous and unsuitable for a continuous-motion qualification.

### Physical disconnect and USB stream repair candidate: 2026-10-23

The Bluetooth repair replaces logical GATT closure with the public GAP physical
disconnect operation for owned current and rejected connections. The observed
live peer address is copied with the connection ID and generation; saved bond
identities do not substitute for that address. The pinned SDK's physical cleanup
and subsequent advertising messages are serialized by its owner thread. API
enqueue success still does not establish disconnect completion, and a missing
callback cannot reopen advertising or report successful management completion.

The production regression reproduces early advertising and failure through the
old logical-close boundary, then verifies that the new path waits for physical
completion. Independent review caught a new reservation race: authentication on
the same link could change its authentication revision and skip submission while
leaving Closing reserved. Physical-close ownership now follows only the live
connection identity. Deterministic callback interleavings cover authentication
success/error and a new connection reusing the old ID/address. All three native
BLE configurations pass their retained 100,000-cycle suites, including existing
failure, admission and bond-preservation cases. Physical pairing recovery and
the controller's underlying status-0x07 resource cause remain unqualified.

The actual pinned TinyUSB DMA class and FIFO reproduce a separate OUT stall:
an empty packet returns zero from FIFO append, and the original class exits
before rearming reception. The repaired class skips append for that empty
packet, still rearms, and emits no invented PCM. Normal bytes subsequently pass
through the actual adapter's consent, consumer-generation and complete-frame
boundaries. Positive-write and endpoint-submission failures remain observable.

A second real-driver regression confirms that the upstream IN FIFO overwrites
unread capture data. Twenty worker calls accept 1,920 bytes into a 392-byte FIFO
without an IN completion. Configuring only function 1's IN FIFO as non-overwritable
makes the same calls accept 392 bytes and preserve the remaining producer data
under the existing short-write, queue and age policies. Full/high-speed tests
exercise actual completion callbacks and 94/96/98-byte flow-control packets;
each step checks byte content and the separate FIFO, scheduled and completed
amounts. Of 3,840 accepted bytes, 3,836 complete and four remain queued, with no
padding or false completion. Rejected IN rearm accounting and OUT overwrite
behavior are not repaired by this change. Neither these host tests nor the
new bounded USB counters establish physical host receipt or acoustic quality.

A supplemental single-frame case then exposed another upstream defect: while
flow-control blackout selected 96 bytes, the FIFO held only 94. The driver ignored
the actual read count and scheduled two extra stale bytes. The final repair uses
the FIFO's returned count as the DMA submission length. Separate full/high-speed
comparisons preserve the failing intermediate image and verify exactly 960 bytes
for a single frame, followed by empty transfers, with no padding or repeated tail.
The earlier four-frame/jitter result remains valid for its original tested scope;
it did not establish this finite-frame boundary.

The build generates an exact-hash copy of the patched class in its output
directory and replaces one translation unit on the existing TinyUSB target.
The managed component, original license and shared class configuration remain
intact. Source, dependency lock, patch and existing output mismatches fail
closed. Incremental builds check the generated copy before compilation; final
artifact inspection must verify that precisely this copy was compiled.

The health parser now retains failed audio jobs independently from global
route cleanup. All 69 parser tests pass, including the original 54. Replaying
the same late portion of the device log changes a false passing result into
`audio_job_failed`, preserving both capture Overflow and playback Start timeout.
Normal cancellation and duration/byte limits remain distinct from a fault or
successful media delivery. Absent job observations remain unknown. These
source/host repairs do not establish physical qualification.

The initial integrated target build passed without drift across 633 repository
entries. Its compile database contains exactly one audio-class translation unit,
the generated postimage, with the shared UAC configuration retained. The managed
upstream class still matches its original digest. This development-identity
build is 4,695,920 bytes. Its image and generated source are
retained locally, superseded by the subsequent finite-frame repair. It was not
deployed and is not device qualification.

The final finite-frame repair passed the complete host suite and pinned target
build. The 633-entry repository inventory remained unchanged through the combined
host and stack checks. The final generated class matches the reviewed postimage
`a776472ab1863cf78e7cef4f8af279a256b2ccbf7df258156e45a9e4fd1c0fbc`,
with exactly one generated compile input and no managed-source fallback. The
development application is 4,695,920 bytes; a clean published identity and
artifact review remain required before deployment.

All 350 declared task paths across 50 measured translation units pass the
unchanged stack budgets and 2 KiB reserve. The USB worker retains its 3,072-byte
allocation; its largest selected path uses 208 known bytes, leaving 816 bytes
after the reserve. Review corrected an omitted nested FIFO peek frame before
the final measurement; the checker now rejects that required frame's absence,
with all 26 focused checker tests passing. Four actual DCD interrupt paths are reported separately:
each has 16 known application bytes and a 304-byte selected chain against the
configured 1,536-byte interrupt stack. This inventory is explicitly unqualified:
interrupt entry, other SDK paths and nesting remain unmeasured, and fitting the
known frames does not prove interrupt safety. No prior hook contribution or
before/after delta is inferred from absent symbols. Physical pairing, audio
delivery and long-duration operation still require device evidence.

### Stream-repair deployment and remaining transport failures: 2026-10-23

The reviewed clean source `c99a103bff361453c98e891b6b2d0ebe7b73d5bf`
was published and its 4,695,920-byte application was flashed and independently
verified. Application SHA-256 is
`15a376ed880701639cb62b0b5aade6d21d0335beef05f2a7ec5309e3f25eed0a`.
The partition table, NVS and PHY region were preserved; no new backup or
whole-chip erase was performed. The runtime identity matched the package.
Host tests, the target build and independent source/package review passed, but
the following physical results do not qualify the image for release.

The initial 90-second capture observed Bluetooth input ready and LAN connected.
Health still failed on retained GPIO initialization errors and an IMU batch-time
fault. Source and earlier-image logs establish that the duplicate GPIO ISR-service
installation messages predate this image; they do not establish the cause of
the audio or pairing failures. The IMU recovered and continued sampling, but
the delayed batch's cause and physical Motion behavior remain unqualified.

With an existing host attempting reconnection, a 120-second pairing capture
contained 115 accepted connections and disconnects, with no input-ready
connection. The earlier advertising-start failure did not recur in this
capture. Disabling that host's Bluetooth stopped the visible reconnect loop,
but an isolated iPad attempt still failed authentication. The user subsequently
restarted the device and reported a successful iPad connection; a fresh capture
also observed Bluetooth connected and ready. This does not qualify Add host
during an existing session. The device displayed its new name while iPad
displayed the former name. Actual advertised and GATT names were not captured,
so an iPad cache explanation remains unverified.

An independent naming gap remains: the [Classic audio contract](bt-audio.md)
requires the shared settings owner to supply the Classic device name, but the
current first-party name-update path sets only the BLE name. The pinned SDK
keeps BLE and BR/EDR names separately. This missing Classic integration does
not establish which name the iPad displayed or explain its pairing failure.

Authentication reason **decimal 86** maps to SMP reason 9 in the pinned SDK.
Both a locally rejected security grant and a peer failure can produce that
result. It does not prove repeated user attempts, peer cooldown or corrupted
durable bonds. Likewise, a negative long-term-key reply establishes the missing
local encryption-key flag in the current SDK record, not absence of all keys
in durable storage. No bond-forgetting operation was performed as a workaround.

The USB Speaker retest failed with a first-frame timeout. The Mac rendered an
output-only test stream to the selected device; this proves host callback
progress, not bus delivery or audible output. The device's observed USB link
was high speed. Its OUT callback counters stopped at two packets and 192 bytes;
the application made 2,012 reads returning zero bytes. The playback job retained
zero received/delivered bytes and no first frame. The codec entered playback
with its transmit clock running and no reported SDK error, then closed during
timeout cleanup. Callback counters occur after rearming, and IN callback counts
can include failed transfer events without distinguishing completion status;
they cannot establish successful host polling
or localize the stall to the host, controller or class driver. Further endpoint
and feedback evidence is required. The timeout remains a failure even though
the global audio route subsequently reports OFF/OK.


### Transport-phase candidate: local validation only

This follow-up enables the pinned controller's Periodic Transfer Interrupt
mode only for the ESP32-S31 Buffer DMA profile, preserving the production
one-millisecond audio interval. The forced configuration header loads the SDK
configuration before selecting the profile. The audio owner requires an
observed valid controller mode; unknown mode, reset or mode loss cannot reuse
an earlier session's authority. The mode lifecycle is separate from diagnostic
collection and does not change HID admission.

Both the audio class and DWC2 controller compile from hash-checked generated
copies. The source preimages, patches, outputs and actual compile inputs are
verified; managed third-party sources remain unchanged. New observations
separate attempted/accepted endpoint submissions, successful/failed completions,
empty packets and audio-class rearming. Feedback values retain their format and
reset epoch. Cached register snapshots come only from active controller paths;
the diagnostic getter does not access a possibly powered-down controller.

Bluetooth diagnostics retain the first failed authentication context plus
bounded admission history. They observe existing policy decisions rather than
changing acceptance, timing or bond storage. A bounded replay in the regular
health report makes an earlier failure visible to a later log reader. Raw peer
identifiers and keys are excluded. These observations do not themselves repair
or qualify the remaining live pairing failure. Source review found the BLE
advertising builder and standard GATT Device Name read use the same confirmed
BLE name as the effective-name display. That does not establish what was
transmitted or what iPad read; fresh protocol observations remain necessary to
distinguish payload propagation from host presentation.

The final complete host suite passed with no changes across 645 repository
entries between its before/after snapshots. It includes the original USB
adapter regressions, the required-profile isolation case, 13 generator checks,
six forced-include configuration rows and the real class/FIFO regressions.
The controller matrix retains 100 baseline/candidate outcomes with the same
positive progress and PCM-content oracle: all 50 candidate outcomes pass,
while 17 specifically identified baseline counterexamples remain reproducible.
The first integration run exposed a duplicate raw-class link input in the new
test runner; it was removed without weakening assertions or dropping scenarios,
and the complete suite was rerun successfully. Review also closed the
check-only generator race with deterministic file-disappearance/replacement
cases. Failed runs remain in local evidence.

The pinned target build passed with all production inputs unchanged. The
4,702,544-byte development application compiled exactly one generated audio
class and one generated DCD source, with no managed-source fallback. Both raw
preimages and generated postimages matched their reviewed digests.

The corrected stack checker passed all 455 declared task paths across 54
measured translation units, retaining the existing allocations and 2 KiB
reserve. Its 201-input measurement reported no drift. The narrowest selected
task margin after that reserve is 624 bytes. Review added an omitted optional
control-request helper and a regression for emitted helper/clone frames; it
was inlined in this build, so the correction did not alter the measured sums.
The separate 30-path interrupt inventory remains unqualified: its largest
selected chain is 512 bytes against the configured 1,536-byte interrupt stack,
without a measured bound on interrupt entry, other SDK work or nesting.

The requested delivery boundary for this candidate is local tests, target
compilation, independent review and source publication. It has not been flashed
or tested on a device. Acoustic output, live iPad pairing/name display, Motion
behavior and powered endurance remain unqualified; the prior physical failures
above are retained rather than superseded by software-only checks.

## Historical evidence

Evidence date: 2026-10-21. The pinned S31 toolchain has built the minimal composite HID firmware. Portable behavior, report layout and production BLE callback logic have executable host coverage. The physical development board is identified as Mosaico v1.2 (eFuse `0x0102`) with 16 MB flash, and its original flash contents have been backed up locally. Runtime deployment and host compatibility evidence are recorded separately below; a build does not establish a working HID device or long-term reliability.

| Host | Mouse path | Keyboard checks | Voice/audio | Status |
| --- | --- | --- | --- | --- |
| iPhone | AssistiveTouch pointer devices | BLE keyboard; English layout/input mode | HFP/A2DP in actual apps | Pending |
| iPad Pro 2018 | Native pointer and optional AssistiveTouch tested separately | BLE keyboard; software keyboard visibility | HFP/A2DP and HID coexistence | Pending |
| Windows 11 | System Bluetooth mouse | US ANSI and Ctrl shortcuts | HFP input/output and A2DP playback | Pending |
| Windows 10 | Add if an actual test host is available | Same layout checks | Hands-Free/Stereo endpoint behavior | Pending |
| macOS | System Bluetooth mouse | US ANSI and Command shortcuts | HFP input/output and A2DP playback | Pending |

For iPhone, check Settings -> Accessibility -> Touch -> AssistiveTouch -> Devices -> Bluetooth Devices, and enable AssistiveTouch. This is system configuration, not a proprietary client installation. See [Apple's pointer-device instructions](https://support.apple.com/en-us/111775).

## Host checks

`python3 tools/check.py` runs portable state, HID queue and haptic policy tests under strict C11 diagnostics and address/undefined-behavior sanitizers. It also validates the composite report map independently, exercises production BLE callbacks with substitutes at SDK boundaries, and checks the serial-health capture tool using synthetic streams. Coverage includes interruption, stale epochs/generations, release recovery, congestion deadlines, bounded lifetime cycles, motor cancellation races and numerical limits. These checks do not emulate a radio or prove host interoperability. See the [coverage matrix](test-coverage.md) for remaining integration and device cases.

### Historical design baseline: 2026-10-21

- C11 tests passed under Clang with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
- Prototype JavaScript syntax passed. Browser checks covered default mouse mode, motion toggle, motion selection of G, the Alpha -> A candidate/confirmation flow, explicit send interrupted by lock, independent audio exit, concurrency simulation and local takeover.
- Browser connections, audio, speech and sending are simulated. No Bluetooth or microphone permission was requested.
- At this earlier snapshot, only the portable core provided epochs and release gates; transport, queue and device UI had not yet been implemented.
- This historical record contains no target-build, flashing, pairing, latency or acoustic result. Current implementation evidence supersedes that scope below.

### Standards and English-language migration: 2026-10-21

- General working rules, engineering standards, knowledge boundaries and contribution guidance are defined separately from product requirements.
- First-party documentation and prototype copy are English. Upstream BSP contents and licenses are unchanged.
- First-party text hygiene, local Markdown links, diff whitespace and prototype JavaScript syntax checks passed.
- The English keyboard and spoken-letter screens were visually reviewed in the browser; group/character confirmation and the sample candidate flow were exercised.
- No portable C behavior changed in this documentation/copy update. Previous host results remain historical evidence, not a new hardware result.

## Hardware qualification stages

1. **Board identity:** record revision, BSP/SDK commits, power and configuration. Abort unsupported eFuse values. Check display, touch, AI, IMU, USB power and temperature.
2. **Minimum HID:** pair each host family; check report IDs, encryption/bonding, reconnect and sleep/wake. Type `aAzZ09 !?@` and confirm modifier release.
3. **Mouse:** 60 s stationary, different grips, fast/slow rotation, touch release, fine control, click vibration and interruption during drag. Retain raw/filtered evidence without concealing drift.
4. **Keyboard:** two-level nine-cell selection, deletion, space, case, mode changes, full 64-character drafts, repeated sends totalling over 100 characters and cancellation/disconnect/target changes. Old results must not reach a new target.
5. **Spoken letters:** repeated 26-letter/digit/spelling-word trials, confusion matrix and rejection rate, with separate accents/distances/noise conditions. Set thresholds from evidence.
6. **Independent audio:** capture, playback, meeting echo, mute, gain and HFP codec negotiation. Pairing alone is not an audio result.
7. **Concurrency:** same-host HID + HFP and HID + A2DP separately, proposed 30-minute interaction runs, then 8-hour powered soak. Record latency, underruns, disconnects and memory.

These durations are acceptance plans, not completed runs. IMU sample rate or report frequency is not end-to-end latency.

## Input firmware engineering record

- Target: `esp32s31`, experimental ESP-IDF 6.2.0 at `383dda6c182e3dabf6355160166fe007df775629`; BSP at `5f10831ddf8697680121e72ba0fd3dce7f7f1799`. Managed dependencies are fixed in `firmware/dependencies.lock`.
- Tooling is project-local: Python 3.12, official RISC-V compiler, CMake 4.0.3 and Ninja 1.12.1. `tools/idf.py` separates setup/build from deployment.
- The BLE development build uses one connection, encrypted composite mouse/keyboard reports, bounded input queues and release barriers. The coordinator watchdog has panic enabled with a five-second timeout.
- The product LVGL UI follows the original top-target/bottom-mode layout, large touchpad, preview-first keyboard and separate unavailable Audio page. Light and Dark themes cover all pages and overlays. Dark is the initial default; Ctrl/Command and haptic settings are explicit and persistent. Only a complete physical AI hold can unlock.
- Live BMI270 sampling, stationary calibration, wrist motion, touch fine control and local motion selection are implemented. The approved nine-cell grid and 64-character draft use bounded transport batches with accepted-prefix retention. Physical orientation, stationary drift and host outcomes still require device evidence. Audio, speech and remote Wi-Fi input remain unavailable; provisioning does not enable remote control.
- USB CDC provides read-only diagnostics; composite USB HID and transport switching are being integrated under the same USB owner. Health records include uptime, free/minimum heap, largest internal block, coordinator stack watermark, queue occupancy and lock state. Logs may drop when the host does not read; logging must not block HID.
- Bluetooth Device Information/Battery services, cross-host compatibility and long-duration qualification remain release gaps. Local battery/About screens are separate from GATT services. The current profile is a development HID subset, not a Bluetooth qualification claim.

### Historical expanded integration

The original product implementation passed a frozen independent v3 review and
target build before the user added device basics, multiple saved peers, a
nine-cell keyboard and USB selection. Those earlier results do not qualify the
expanded tree. Standalone reviewed components are published as completed stages
in [delivery progress](progress.md). The expanded linked candidate, deployment
and actual Bluetooth/USB host tests require fresh results below before delivery.

The expanded baseline passed independent review of 128 first-party code,
configuration and test files. The sorted SHA-256 inventory digest is
`11c200a3d1237b05b3c1a4021a5344dd4b5b0d49e2ce10d431308f26d0a1fe26`.
All ten review findings are closed for that snapshot. The complete host suite
passed with 77 coordinator and 16 settings-store cases, both BLE capacity
configurations, USB lifecycle/identity checks and the existing portable,
board, provisioning and serial suites. Native UI inspection covered 164 cases.

The exact candidate built for ESP32-S31 has application size `0x24da80` bytes
and SHA-256 `522fa60eab1bdcca0e0706da25f81bc29d3e2b51677dd9be12304816285e6ea7`.
Its four-MiB app partition retains 42 percent free. The candidate supports one
shared CDC/HID USB stack with chip-derived serial identity. These are compiled
capabilities; source/build success did not establish a working device. The
expanded deployment failed the device gate described below. Input, pairing,
wake, persistence and endurance still need successful device evidence.
New connection-priority/scanning and OTA requirements are
excluded from this source/build seal and remain separate extension work.

### Runtime evidence

Initial deployment completed on the confirmed Mosaico v1.2. Bootloader,
partition table and application writes passed esptool data verification. Native
runtime USB CDC enumerated after reset. The original 16 MiB backup was verified
again before writing and remains private local evidence.

The first 90.035-second capture observed eight increasing health samples
(uptime 22 through 92 seconds), queue occupancy zero, internal free memory
167,683..167,819 bytes, largest internal block 94,208 bytes and coordinator
stack watermark 2,000 bytes. No reset or runtime error appeared in that captured
window. This was an idle bring-up observation, not a load or endurance result.
The capture **failed** because its deadline split the final health line; the
capture helper was corrected with a fixed one-second grace for an already-started line. Its 21 offline tests preserve malformed/fault/freshness failures.

The host did not discover BLE on this initial image. Investigation found that
the pinned structured advertising API rejects a two-byte UUID input: it requires
full 128-bit UUID representations and compresses Bluetooth-base UUIDs on air.
The correction uses the complete HID service UUID and adds an initialization
boundary regression. Periodic health diagnostics now retain Bluetooth startup
failure visibility after late USB attachment. Corrected advertising is included in the product UI replacement; deployment and actual host-input qualification are recorded separately when performed.

Original-firmware backup, build logs, serial captures and identifying details
remain ignored local evidence. Never commit the factory backup because it can
contain credentials or pairing data.

### Expanded baseline: failed device gate

Commit `f5e724b9b0a5b7cfd36e636d1f1ee8ab4f09a3c2`, application hash
`522fa60eab1bdcca0e0706da25f81bc29d3e2b51677dd9be12304816285e6ea7`,
was written and verified successfully. Runtime USB enumerated. The user observed
the expected dark layout and nine-cell keyboard, but touches produced vibration
without visible button transitions, with residual content near the top.

The requested 90-second serial capture ended after 46.918 seconds with a serial
I/O error. Five health samples covered uptime 22 through 62 seconds. Internal
free memory ranged from 2,991 to 12,435 bytes; the largest internal block ranged
from 864 to 8,192 bytes, despite roughly 14 MB of total free heap. Repeated LCD
SPI transfer and LVGL draw failures reported `ESP_ERR_NO_MEM`, followed by
display-lock and battery telemetry timeouts. Bluetooth initialization had
returned success, but the later transport failure flag remained set. The
initial failure event was outside the captured window. A reset was not proven.

This is a **failed physical acceptance gate**. Failed redraws are a supported
explanation for the unchanged screen and residual pixels, not proof that every
touch path is correct. The repair must preserve internal/DMA memory, capture
startup allocation stages, and retest real display transitions and Bluetooth
readiness before attributing success to a layout change. Native render tests
cannot establish target memory sufficiency or display transfer completion.

### Memory-repair source and build

The scoped repair at `41d6b42b65e432724c94af0f15caf2140c12bb99` passed
independent review of 21 changed files. Its approved-file inventory digest is
`530fe4621ed8b5529a369f32283b3b02d8ccb3d5bea6ba4bdaa9733c2c10bb97`.
The complete host suite passed, including 80 coordinator and 23 serial-parser
cases; independent BLE checks covered both 15/32-bond capacities and first-failure
retention. The final linked ESP32-S31 application is 2,423,824 bytes (`0x24fc10`),
SHA-256 `899841a8a4496d52bba8fb537003b8cc2871f78f8264d2083a8f782a8f741e91`.
All first-party firmware source hashes stayed unchanged during that build.
The later focused-UI edits are excluded from this image and source seal.

Generated configuration confirms the intended PSRAM/internal allocation policy.
The target ELF places the fixed allocation-failure record in internal BSS and
its hook in IRAM, using inline lock-free atomic operations without logging or
allocator calls. The original BSP TE/full-frame/internal-bounce path is retained.
These are source/build observations; this repair image has not yet passed
physical display/input or long-duration acceptance.

### Unified API planning review

The eight planning documents published at
`25760ebff776562768dd1221f86b4e261ff58535` passed independent consistency review.
The approved inventory digest is
`964525d29c526bbe84d1154ddc765f2937b30c47f414c2b1d21b34ea4f8d4fbc`.
All 48 current `app_action_t` values have one matrix row, with no omissions or
duplicates; the reviewer checked activation effects and cross-document names,
scopes, units and lifecycles. All eight recorded planning findings are closed.

Repository text/local-link checks and the pinned SDK's candidate partition-table
validation passed. These checks do not implement or qualify authenticated remote
control, recording, policy storage or OTA. Machine-readable schemas, generated
bindings, actual partition migration and device validation remain open gates.

### Focused UI source, native rendering and build

The final compact UI production source hash is
`6b2963cbd567dcf9f58cfbf37891f5580f89dc6230e7c4827384819b91bf098a`.
The stable native validation manifest digest is
`5c5741e38219615ab7eb76822d5e34eb59c2ab04eee838b06c6bda6d815b3468`.
Both visual and resource executables passed under ASan/UBSan; the source/test
inventory stayed unchanged throughout the run. A later interaction-document
correction clarifies the already tested three-fixed-pages/one-panel architecture;
it does not change executable behavior or invalidate that run.

The suite retained all 164 prior scenes and added ten, producing 174 native
480 x 480 PNGs. Representative Mouse, Keyboard, Menu, settings, confirmation,
long-text and error-state renders were visually inspected. All 50 FULL-render
resource cases recorded zero new flushes and zero allocator activity for idle or
hidden-only changes over 100 snapshots / 10 simulated seconds. A real visible
change produced a flush, so zero work is not a disabled-rendering result.
Both themes completed 1,000 panel open/close cycles across 17 variants, with 102
core objects, a peak of 142, and unchanged warmed live bytes/blocks. Teardown
returned all observed allocations. These are host measurements, not target heap
or elapsed powered-time evidence.

The complete host suite passed with 91 coordinator cases and 23 serial-parser
cases. The final pinned ESP32-S31 application is 2,423,488 bytes, SHA-256
`df5f803e04c5ae08ef8c834e4b8464cb140f96b5edf32565f78507ecf080c920`.
The UI/header/coordinator source hashes remained unchanged during the final
build. Earlier compact-UI images are superseded. Subsequent physical evidence is
recorded below; host interoperability remains a separate acceptance gate.

### Focused UI deployment and Motion investigation

The image above was flashed with verification, preserving NVS. The user confirmed
working Menu/Keyboard/Mouse touch navigation and AI double-click. A 90.47-second
capture had nine health samples, stable free internal memory of 72,735 bytes and
largest internal block of 31,744 bytes. No LCD allocation failure or BLE failure
was captured. However, repeated `esp_lv_adapter_lock` errors caused the smoke
result to fail. Source inspection traces the warnings to the coordinator's
zero-wait presentation lock; they do not prove a 100 ms blocking stall. The
planned correction transfers presentation consumption to the LVGL owner task,
without weakening the log parser or increasing lock waits.

A separate 35.03-second capture observed Bluetooth initialized, connected and
ready, with no BLE failure, but again failed on LVGL lock errors. The user still
could not enable Motion after selecting USB. Existing periodic logs omit IMU,
calibration and selected-route readiness, so they cannot establish that cause.
The user's factory-ROM accelerometer observation is separate evidence of prior
sensor operation. Retained startup/stream diagnostics and explicit Motion
availability reasons are required before claiming a fix. These captures are not
elapsed-time endurance or complete host-input qualification.

### IMU startup diagnosis and configuration reproducibility

Diagnostic source `db0d24fbf9d4dbf97a52fe6ae58483c8b4ee2f3d` was built in an
isolated checkout with freshly generated configuration and flashed with hash
verification, preserving NVS. The 2,425,376-byte application SHA-256 is
`ae7adf59c0b58bb64a829224718bcfdd428c02545398ed1e705044d30aa09baa`.
A 36.001-second capture contained three complete health samples and failed
on retained errors plus an incomplete final health line; it is not a passing
runtime smoke test.

The first IMU fault was `first_sample / ESP_ERR_TIMEOUT`, with zero accepted
samples and status `0x90`: accelerometer and command ready, gyro not ready.
The selected input was connected and ready. This establishes a sensor startup
readiness failure rather than a Bluetooth admission failure; it does not prove
a defective sensor. The startup correction permits 100 ms for the first valid
sample, with deadline checks before and after acquisition. Runtime freshness
remains 50 ms and the acquisition batch limit remains 20 ms. Regressions cover
delayed gyro readiness, permanent absence and late batch completion. Physical
recovery remains unverified until the corrected image is deployed.

The same diagnostic capture retained an 8,190-byte internal DMA allocation
failure. Its freshly generated configuration used 16 Wi-Fi static RX buffers
and a receive Block Ack window of 16, whereas the earlier local configuration
used two each. Those limits had not been retained in tracked defaults. The
profile now records 2/2 explicitly and bounds TCP out-of-order retention to
four pbufs per connection. Fresh configuration generation and a new device
capture are required; earlier memory observations do not qualify this image.
The diagnostic image also includes the 50-percent fallback brightness change,
but physical brightness and preserved settings have not been measured.

### Corrected IMU deployment and remaining pointer qualification

The startup/memory correction was published at
`892805a4c37777ee22518de4517bf6974ed22222`. Its freshly configured, reviewed
2,425,632-byte application SHA-256 is
`4996587fad68e21ddc5075010d921c5e9ea8d896b45ef19035cee828c354b1f3`.
Flash verification passed and NVS was preserved. The 46.005-second capture
observed IMU samples increasing from 928 to 3,675, `ESP_OK`, and completed
64-sample calibration by uptime 22 seconds. No retained IMU, allocation or
Bluetooth fault was captured. Complete health samples reported 66,591 bytes
free internal RAM and a largest DMA block of 25,600 bytes.

That capture remains failed: its final health line is incomplete
(`malformed_health`, `unterminated_line`). A subsequent attempted capture found
no USB serial interface and collected no samples. Neither result is a passing
health or endurance gate.

The user confirmed that Motion can enable and produce pointer movement, but
movement is too slow and directions are unintuitive; touchpad movement works
normally on the same connection. Sensor startup recovery is established, while
air-mouse behavior is not accepted. The requested next refinement is automatic
adaptation between screen-up and tilted screen-facing grips, with configurable
speed and directions. The fixed-axis mapping in this image has not passed that
requirement.

### Native station setup and input controls candidate

The integrated candidate provides on-device Wi-Fi scan/select/password/join,
saved-network reconnect, explicit common host keys, touchpad tap/double-tap/drag,
a persistent tap-to-click preference and verified asynchronous Bluetooth-name
updates. Normal Wi-Fi setup joins the existing LAN; a recovery hotspot is an
explicit secondary action. A LAN address does not imply an implemented web/API,
MCP or OTA service. Those services remain unavailable/planned.

The native UI run passed 276 rendered scenes in both themes, preserving all
174 earlier scenarios. Sixty unchanged/hidden-state cases produced no redraw
or allocation; 1,000 panel cycles per theme retained no growth. Core/peak object
counts were 102/144 under the unchanged 160-object limit. Final teardown retained
zero live blocks. The run checked private draft wiping, consumed sessions,
generation-bound network rows, common keys and touch contact heartbeats using
the production LVGL UI. These are host observations, not device memory proof.

The expanded coordinator suite passed 108 cases, including actual HID down/up
sequences, queued swipe rejection, drag cancellation, local editor FIFO and
secret cleanup, acknowledged name persistence and the two added navigation
routes. The presenter module passed eight bounded-publication tests. Actual
LAN join/IP acquisition, on-air name changes and physical tap/drag/common-key
interoperability remain device gates for this candidate.

### Long-term power gate

The test plan requires 24 hours of powered operation followed by 72 hours after failure cases pass. Cover host sleep/reconnect storms, no USB reader, queue pressure, motor cancellation, storage wear, power interruptions, watchdog recovery and comparable memory checkpoints. Record temperature and power conditions. A short smoke run and 100,000 simulated host cycles are useful evidence but do not satisfy this gate; periodic reboot is not an acceptable substitute for fixing resource growth.

### Bounded USB duplex recovery: 2026-10-26

Clean source `84bd53f817c8d8236f363e86892a9cb8f13d133e` was deployed as an
application-only update and independently verified. The 4,707,072-byte image
SHA-256 is `812bb8875290a418432bd6bbbc86c51fe7eaa539d953df4978c417672d4830f0`.
Partition table, NVS and PHY data were preserved; no full backup was taken and
the optional startup-tone module remained disabled.

A 180.004-second runtime capture contained 18 health samples and no smoke-gate
failure codes. Two Mic jobs delivered 66.93 and 16.62 seconds equivalent of
mono 48 kHz PCM16; two Speaker jobs delivered 74.19 and 17.39 seconds equivalent.
All four jobs completed by requested stop with zero terminal errors. Capture
discarded no frames; playback discarded four queued frames at explicit stops.
The USB capture-admission rejection counter ended at one, playback drops at
zero and USB error at zero. This is not a zero-rejection device claim.

The user confirmed simultaneous Mic/Speaker operation, normal touch/navigation,
and successful restart of both controls. No crackle was reported in this repeat.
The host probe received input callbacks without callback errors and retained
no PCM. Its nonzero-sample count did not separately account for host silence
flags, so it does not establish meaningful recorded duration or speech quality.
The bounded run supports this USB duplex recovery, not endurance, Bluetooth
audio, echo cancellation or a general explanation of the earlier reported
freeze. Runtime logs and host-specific details remain local.

### Real-time microphone level candidate: 2026-10-26

The Mic card now exposes a read-only RMS dBFS level from validated mono PCM
already acquired by the board worker. Publication is bounded to 100 ms and
freshness to 300 ms. Missing, expired, muted, failed and all-zero observations
remain distinct. Permission/generation checks prevent a late read from restoring
a stopped meter. The post-copy freshness clock avoids misclassifying a concurrent
publication as a future-dated sample; command timing and revisions are unchanged.

The complete board suite, including six new energy/lifetime/clock scenarios,
passed with sanitizers. All 36 media-control cases and 163 coordinator cases
passed. The optional local-tone suite also passed all 13 cases after the timer
dependency change; that module remains disabled in the normal firmware.
The pinned target build and 494 declared task stack paths passed with no source
drift; interrupt safety remains unqualified.

The focused production LVGL Audio suite passed 138 renders and touch checks
in light/dark themes. The meter adds two persistent objects (114 core objects)
and preserves held Start/Stop/Mute/Menu/volume controls. Nine unchanged/hidden/
inactive cases per theme, each with 100 snapshots, had zero allocation or flush
activity. One hundred changed live values per theme retained stable live bytes
after layout and no animations. The public LVGL label setter temporarily used
56 bytes for layout bookkeeping per update; it was released after layout.
Assertions that assumed this public API allocated nothing during a visible text
change were corrected after tracing the library behavior; the original failed
probes remain local. Existing zero-idle gates were not weakened.

Representative final light/dark active, waiting, quiet, clipping and long-copy
renders were visually inspected. These checks establish source/native layout
and state behavior, not physical microphone sensitivity, host recording or a
device stability pass for this new meter image.

The subsequent application-only deployment of
`829ca37bf879f09417740f04c98b8312baaf72f1` verified the 4,709,360-byte image
with SHA-256 `24cfd0f11d5a86ab3cc023986ea88562ab197b8ddef11aa464152bfeedec2edf`.
Partition table, NVS and PHY bytes were preserved without a full backup.
Its 180.005-second capture had 18 health samples and failed with
`audio_job_failed` and `runtime_media_fault`. The first Mic start failed before
any PCM frame; the codec retained `ESP_ERR_TIMEOUT` from `i2c_master_transmit`,
and the engine entered quarantine with `AE_IO_ERROR`. USB input/output remained
selected and transport counters advanced; the host received no nonzero input.
The user reported disabled Mic/Speaker/volume controls and alternating Start/
waiting states after choosing USB. No successful meter acceptance is claimed.

The log contains no allocation, display-refresh, panic or reboot failure.
Tracing the retained timeout through the exact pinned synchronous I2C driver
and build configuration identifies failure to acquire the shared bus lock,
before submitting the register payload. The register, competing peer and any
indirect scheduling contribution were not recorded. The level computation had
not received a PCM frame. UI resource cost alone is therefore not an evidenced
explanation for this attempt. Raw failure and post-failure captures remain
local; physical recovery is pending.

### Bounded codec initialization admission: 2026-10-26

The codec's five-millisecond bus admission wait could expire while another
shared-bus owner was legitimately active. The pinned BSP touch peer and codec
control default use 100 ms. Initialization now uses that finite argument while
muted; runtime, mute and teardown retain 5 ms. The first failed or cancelled
initialization rejects later vendor callbacks before another SDK wait. The
original timeout result is retained, and cleanup failure remains uncertain.
There is no automatic retry, fabricated readiness or change to job deadlines.
The SDK separately applies its argument to admission and transfer; this does
not bound an entire initialization to 100 ms.

An identical native fixture with the real codec and a six-millisecond shared
peer hold reproduces the old image's retained SDK timeout and generic engine
I/O failure, while the repaired source prepares successfully. Six new cases
cover read/write contention, exhausted wait, transfer failure and stop/mute
cancellation; existing board regressions also pass. Thirteen observer cases
and 163 coordinator cases pass. Physical recovery still requires a new image.
The pinned target build and 504 declared task stack paths pass; interrupt
safety remains unqualified. The existing meter layout and control binding
code are unchanged by this repair.

The failed image also recorded repeated USB stream generations during initial
host setup, followed by stable selected input/output. This is separate from the
later codec failure. Existing one-second settling and quarantine rules remain
unchanged. A bounded transition record now copies Mic/Speaker Start permissions,
blockers, pending state and volume availability from the coordinator's existing
presenter candidate. It adds no hardware reads or readiness decisions and is
not a display acknowledgement. Meter-only changes do not generate these logs.
The exact cause of the reported actionable/disabled alternation remains a
physical gate; no cosmetic smoothing or UI-overhead attribution is claimed.

The subsequent application-only deployment of `f4c20a7` independently verified
image SHA-256 `20ad932bd263bc4b3eff2e3ebd5adf48bc520e6e70c7bad4333c3bfa299d0567`
and preserved partition/NVS/PHY data without a full backup. This attempt failed
before any user audio job: the boot probe reported `AE_UNCERTAIN`, cleanup was
uncertain and the engine entered quarantine. Both retained SDK errors were
zero; no register transfer failure was observed. The first retained serial
line was after this startup failure, so the exact preceding schedule was not
captured. New admission observations consistently blocked both Start controls
for quarantine while the host negotiated USB streams. This does not qualify
the previous flicker report or microphone meter; startup cancellation ordering
requires repair before another physical audio test.
The completed 240.005-second capture retained 24 health samples and the
`runtime_media_fault` failure code. All 47 sampled admission records blocked
both Start controls for quarantine; no user audio job was attempted.

The follow-up native reproduction injects the real engine's initial stop
inside the real ES8311 enable callback. It reproduces the retained probe
uncertainty, zero SDK error, stranded enable reference and subsequent stop
timeout. Sequential probe-first/stop-first tests had missed this interleaving.
The repair preserves the mandatory muted boot probe through queued safety
requests and acknowledges them only after actual teardown. The probe keeps
its original 5 ms budget from its first control call; the 100 ms allowance
applies only to an explicit user-job prepare. Ordinary prepare cancellation,
real SDK failures and the engine's 100 ms stop deadline remain unchanged.
No PCM, PA enable, automatic probe retry or private reference repair is added.
Nine added real-engine/codec cases cover those interleavings, a blocked SDK
call, queued mute and a genuine transfer failure. Healthy cases confirm off
within the unchanged deadline in the advancing-clock fixture; the genuine
failure remains quarantined. The relevant board suite, pinned target build
and 504 declared task stack paths pass. These are source/native/target checks;
the follow-up image still requires physical boot and audio acceptance.

### Boot recovery and USB waiting presentation: 2026-10-26

The application-only deployment of `3a17212` independently verified image
SHA-256 `0320648d6ff5723234a59778a63433f390051d9d6201f3a11920a21d10ee52be`,
preserving partition/NVS/PHY bytes without a full backup. The boot probe
completed with observed hardware, no codec error, no uncertain cleanup and
confirmed off. The user confirmed a changing microphone level, normal USB
duplex sound and usable controls. Three Mic jobs and three Speaker jobs ended
by requested stop with zero reported job errors. The longest delivered 79.67
and 35.35 seconds equivalent of mono 48 kHz PCM16, respectively. Host input
contained nonzero samples, with zero host callback errors; no PCM was retained.

This is not a complete smoke pass: serial capture stopped after 189.85 of the
requested 240 seconds, retaining 18 health samples and `capture_incomplete`,
`esp_error` and `serial_io_error`. The two ESP errors were the already-installed
GPIO ISR service messages at startup; the serial interruption cause is unknown.
The last retained audio state was off with no engine error. The capture retains
USB admission rejections and explicit-stop discarded frames; it is not a
zero-drop or long-term stability claim.

The remaining user report is rapid Unavailable/Preparing/possibly Ready text
after choosing USB. Initial presenter samples alternate blockers for no host
stream and an unsettled stream, with both Start permissions false. Permission
then becomes true after the existing settling interval. The 100 ms observer is
not per-frame display proof, so it does not disprove faster reported changes.
Source-matched rendering explains the observed Unavailable/Preparing alternation.
The presentation repair maps both idle waiting conditions to the same visible
waiting state and stable host instructions; actual Ready permission, disconnect,
fault and active-job behavior remain distinct. No delay, transport readiness
decision or command policy is changed. Per the user's subsequent instruction,
this follow-up is validated locally and published without another device test.
The focused production LVGL Audio run passes 142 renders and interaction
checks in both themes. Two 100-update alternation sequences per theme, including
retained completed-job notices, produce no extra flushes, allocations or live
memory growth. Held Start loses permission immediately when applied state
revokes it; blocked presses cannot become deferred consent. Real errors and
disconnect remain visible. Representative light/dark waiting layouts were
visually inspected and the pinned target build passed. This does not establish
physical display acceptance for the new waiting presentation.

### USB Mic restart failure during continuous Speaker playback: 2026-10-26

The application-only deployment of `9199f64` independently verified image
SHA-256 `4c4e3ddd1684eb0f6b36552bfccfe7e273ede1aaad5b0571e8eba4cfa85b4975`,
preserving partition/NVS/PHY bytes without a full backup. The boot codec probe
completed successfully with confirmed off. Before the host opened audio streams,
both USB alternate settings were inactive and both roles correctly remained
Waiting. This observation does not establish the later waiting-to-Ready display
transition; the user did not separately confirm its visual stability.

On macOS 27.0.1, the subsequent duplex test recorded nine distinct failed Mic
restarts while Speaker kept the same audio resource active. Each received one
960-byte frame, delivered zero bytes and ended with `AE_JOB_PARTIAL`,
`AE_STOP_IO` and `AE_STALE`. The engine and codec retained zero error and the
USB link identity remained stable. Three other Mic jobs and three Speaker jobs
ended by requested stop. The user reported that Mic could not start, then
recovered and worked again. Recovery does not erase the failed acceptance.

The host received 3,795,968 frames including nonzero input, with zero callback
errors and no retained PCM. These counters do not prove uninterrupted playback
or physical sound. Both serial captures were incomplete: startup retained
139.581 of 240 seconds with `capture_incomplete`, `esp_error` and
`serial_io_error`; the duplex capture retained 81.005 of 240 seconds with
`audio_job_failed`, `capture_incomplete` and `serial_io_error`. The startup ESP
errors were the already-installed GPIO ISR service messages. Serial interruption
causes remain unknown; this is not a smoke pass or a stability qualification.

Source tracing identifies a sequence-ownership mismatch: each immutable Mic
consumer starts its own frame sequence at one, while USB capture compared it
against the previous consumer's sequence for the entire audio resource. A full
resource restart clears that state, explaining the observed recovery. The
follow-up repair must preserve rejection of old consumers and duplicate frames,
and must not reset Speaker or weaken privacy and lifecycle checks. The user
requested local validation and publication only for the repair; no follow-up
device test is authorized at this stage.

An actual-engine/production-USB host reproduction matches the device signature:
the first Mic restart ends partial with 960 bytes received, zero delivered and
`AE_STALE`, while Speaker stays running and the audio/link generations remain
unchanged. This reproduces the defect without substituting a test-only engine
sequence or resetting the shared resource.

The USB adapter repair tracks capture consumer identity separately from its
sequence. A newer consumer retires the old capture queue and waits for the
worker's IN FIFO flush; older consumers and duplicate frames remain rejected.
The OUT FIFO, partial Speaker data and playback binding are retained. Worker
queue advancement also rechecks consumer identity after an in-flight write.
No engine/API/UI behavior, buffer capacity or timeout was changed; the adapter
adds two fixed 64-bit lifecycle fields.

The repaired production engine/USB integration delivers data after two Mic
restarts while Speaker stays running with unchanged audio/link identity. The
focused USB suite also covers stale high-sequence frames, replacement during a
partial/in-flight write and exact new-consumer bytes through the real pinned
FIFO at full and high speed, while preserving partial Speaker input. Existing
USB regressions and the pinned ESP32-S31 target build pass. These establish
local regression and compilation evidence only. The repair has not been
flashed or physically tested under the latest user instruction.

### Bluetooth management without a USB release dependency: 2026-11-03

The user reported that saved Bluetooth hosts could not be removed in any mode
and new pairing succeeded only with USB input connected. Source tracing found
the native management workflow waiting for the HID engine's retained
`release_pending` even after Bluetooth link-down. That flag deliberately remains
set until a connected input host accepts neutral reports. The same flag also
disabled disconnected saved-target selection. It must not be cleared merely to
permit management.

The repair removes those waits only after observed Bluetooth disconnection and
scopes connected release checks to the selected Bluetooth input transport.
Local and remote disconnect paths share that scope. Current-host release and
congestion checks, generation/identity validation, policy durability and
transport-switch cancellation remain in place. The BLE adapter's bond-removal
and storage implementation are unchanged.

The original manager stalls in the actual native BLE workflow with the future
release barrier set. The repaired workflow completes Add, Forget all, selected
single-host removal and unselected single-host removal, both from initially
disconnected state and after a confirmed disconnect, while retaining that
barrier. Removal still verifies the requested identity, unchanged remaining
identities and durable reconciled target policy. Connected pending release and
congestion still prevent premature disconnect. Controls tests cover all three
product modes and preserve cancellation after accepted policy work. Coordinator
tests cover USB-disconnected release isolation in every mode without sending
HID or clearing the future barrier.

The final native BLE suite passed all three capacity/profile configurations,
each with 100,000 lifecycle cycles under ASan/UBSan. Connectivity controls and
all 164 production coordinator cases passed. The pinned ESP32-S31 target build,
task stack-budget gates and repository hygiene checks passed. Interrupt stack
safety remains unqualified; these checks do not establish device endurance.

This is source/native evidence for a reproducible management dependency, not
physical proof for every reported pairing/deletion failure. No device, serial,
radio or flash testing was performed for this repair, as requested. Native
radio/storage substitutions do not qualify host discovery, pairing or physical
bond removal.

## Wi-Fi development management and diagnostics: 2026-11-03

The running Mosaico v1.2 image identified itself over USB telemetry as clean
`9199f64f620fc675bf8359c75f2aff8bf493bd41`. Initial live observations showed a
connected station, known absent trust and a stopped HTTPS service. The device
refused the management TCP connection. This was not evidence of a radio fault.
Explicit USB import and local review then installed a scoped native identity,
signed-update source and finite update policy while preserving settings. Actual
authenticated TLS reads succeeded; the clock became known and the update owner
reported the compatible layout, available configuration and confirmed running
image. No client secret or private certificate material is tracked.

The original companion repeatedly received busy on five state GETs, with each
fresh TLS request taking approximately 208..280 ms. Coordinator read receipts
remain valid for 250 ms. Reusing the pinned connection and polling only exact
busy GET responses produced seven successful state reads on the same running
image. No firmware freshness, command deadline or owner checks changed. The
old image truthfully reported the new runtime diagnostic resource as unsupported.

The source addition provides a scoped, copied runtime diagnostic resource and a
bounded private LAN collector. Native tests cover scope, unknown/zero, retained
failures, privacy, contention and a 3306-byte maximum-width fixture. Companion
tests cover bounded reads, no mutation replay, shared-connection ownership even
after async cancellation, and session closure. Collector tests preserve partial
evidence, boot changes, output limits and resource errors. Target compilation
and declared diagnostic HTTPS stack paths, including JSON parser recursion and
scalar canonicalization, passed. ISR and endurance qualification remain open.

The first signed LAN download targeted clean `b4a7e79`, a 4,717,536-byte image
with SHA-256
`7f3d95a21ddff60c3a5fb9a1ec23beed2abcc077be4858662b38e9bc0ce66184`.
Catalog verification and inactive-slot streaming succeeded, but the update owner
aborted after 2,719,744 bytes with error 259 (`ESP_ERR_INVALID_STATE`). It reported
`update_failed_no_success_claim`, no cleanup pending and no boot selection.
Authenticated reconciliation still observed the same running boot and confirmed
old image. No activation was attempted. The existing result does not identify
which authorization or maintenance predicate failed; a nearby sensor cache-age
fault does not establish causation. USB evidence also retained battery timeouts
and that sensor fault, so its health check did not pass.

The follow-up observation patch retains the first terminal update failure,
distinguishing an authority rejection from an SDK error with the same numeric
value. The existing firmware-state GET exports it with job/revision binding and
null unobserved authority. Full control-runtime tests, 164 coordinator cases,
45 stack-checker regressions, target compilation and the declared stack budget
passed. Native cases preserve initial waiting without a false failure latch,
first-error retention, unknown fields, authorization short-circuit behavior and
the unchanged 250/251 ms freshness boundary. This is a diagnostic improvement,
not a demonstrated fix for the interrupted physical download.

A separate five-second HTTPS sensor collection verified 97 responses containing
720 records (712 IMU samples, seven power records and one rejected IMU attempt).
Its receipt retained the initial journal retention gap and source error; it was
`complete_with_issues`, not a complete usable movement segment. No ongoing
retention gap or wire-format anomaly was observed in that bounded collection.

The clean `a79887c626c872f9373f775b5c4553be2746151e` diagnostic image was then
installed through the authorized app-only USB fallback. Its 4,719,328 bytes had
SHA-256 `c14e909e7c0e23be86b4b748df4e8c58aad15c95bbfd1948b5d6ae76a18687de`.
Chip, flash capacity, security state, installed partition table and confirmed
boot selection were checked before writing only the selected application slot.
Independent application verification passed; table and OTA-selection bytes were
unchanged. No backup, settings-region write or boot-selection mutation occurred.
The user confirmed normal startup, the main interface and touch navigation.

After reboot, saved-network fallback joined a different LAN. The original
address therefore timed out even though serial evidence showed a ready TLS
listener and synchronized clock. A bounded read-only probe used the observed
new socket destination while retaining the enrolled TLS hostname, CA validation
and exact certificate pin. Sensor catalog, protocol state, runtime diagnostics,
firmware state and clock all returned HTTP 200. The device's clean commit,
ELF SHA-256 and configuration matched the frozen package. Firmware state was
available/idle, running-image state was known and confirmed, and failure evidence
was null. This USB installation does not exercise OTA probation or confirmation.

The startup USB capture was deliberately interrupted to inspect its buffered
evidence; it retained 14 health samples, interruption failures and two SDK
`gpio_install_isr_service` errors. It is not a passing health qualification.
After the user rejoined the update-source LAN, all five diagnostic resources
returned through the ordinary pinned client; the bounded collector completed
without resource errors. A subsequent download failed at `HTTP_READ` after
2,277,376 bytes. Its publisher had closed the response after a short socket
write timeout. This is distinct from the original error 259, whose cause remains
unidentified. Separating publisher inactivity from total response duration and
retaining exact progress exposed the host-side failure without changing firmware
authority checks or transport deadlines.

The next transfer continued past that boundary, but its observer stopped after
180 seconds and closed the authenticated owner session. The device rejected the
next read after 4,116,480 bytes with an unauthenticated authority observation.
This was a test-harness termination, not an unexplained device failure. After
reconciliation, a new attempt kept observation open for the admitted ten-minute
job budget. It downloaded and verified all 4,719,328 signed bytes in approximately
210 seconds and reached `STAGED`, with no failure and no boot selection.

The separate activation request was accepted but failed after approximately
four milliseconds, still without boot selection. An unchanged-production native
regression reproduced the missing initial maintenance wait: admission had queued
the coordinator barrier, while activation immediately required it to be ready.
The repair shares download's bounded maintenance wait with activation. Tests
cover immediate readiness, delayed readiness, the five-second boundary, timeout,
cancellation, revoked authority, lock, restart permission, digest mismatch and
expiry. Review also exposed transient denial followed by restoration and a
scheduling overshoot past the deadline; failing regressions preceded their fixes.
The shared wait now terminates on every observed non-maintenance denial and
rejects late readiness even if authority is otherwise valid. Denied paths
preserve staged bytes and do not write the release floor
or boot selection. Existing post-wait authorization checks remain in place.

The bounded development publisher freezes the exact catalog/image bytes and
has fake socket/TLS/clock regressions for slow progress, inactivity, total expiry,
request rejection and private-data-safe failure records. These host tests do not
qualify its real-network throughput. Full runtime sanitizer tests, 164 coordinator
cases and the target build passed for the maintenance repair. The repaired
activation required installation and a complete physical OTA cycle, including
reboot and health confirmation.

Clean `a92c76aae53b894c9f9759856c74af6cd2a0b3a7` was installed and read back
through USB. The application SHA-256 was
`e444f42859570c88bd1b84149660adff2fd2a493972148838b344f78d668735f`.
The partition table and boot selection were preserved. After restart, five
authenticated diagnostic resources returned HTTP 200 on another saved LAN, but
the device timed out connecting to the enrolled publisher. Back on the publisher
LAN, the source received a TLS handshake. USB reported peer-certificate
verification failure and the clock resource reported an unknown time. No image
bytes were sent in either attempt. The device subsequently synchronized on the
same LAN. With the same running firmware, trust settings, catalog and publisher,
a fresh check verified the signed release with zero image bytes written.

Native regressions reproduced a pending first synchronization being delayed by
the normal refresh interval after reconnect, and a catalog check opening TLS
with an unknown clock. A new LAN-ready edge now starts a synchronization when
the clock has no valid anchor. Repeated ready notifications and reconnects with
a valid anchor preserve the refresh cadence. Catalog checks report
`time_or_trust_unavailable` before opening a source connection when the clock
is unknown. The device still checks signed release expiry after reading the
catalog. Only a real synchronization callback establishes time. A split-DNS
regression kept a positive answer from the old LAN across a station transition.
Clearing the cache at the network-entry boundary before publishing readiness
prevented reuse of that address. Native station and clock tests cover delayed
lookups, duplicate notifications, cache-clear failure and a second link loss
during entry.

## Manual Bluetooth host switching on Mosaico (2026-11-03)

With two saved hosts on the clean `a92c76a` build, the other host's selection
button was disabled. The peer resource reported one host eligible for exact
selection and one ineligible across all 78 observations. After manual Disconnect,
the device repeatedly accepted a radio link that never became ready for input.
The same capture included 28 connected samples with no matched saved row, 49
disconnected samples and one initially ready connection. USB recorded 176
not-ready connections with a median interval of 731 ms, and 257 negative
long-term-key replies. The SDK had no local encryption key for those
live requests. The saved list and its revision stayed unchanged. The captures
do not identify which physical host sent each request or establish loss of
stored keys.

## Signed Wi-Fi installation and first-boot rollback on Mosaico (2026-11-03)

The device verified the signed catalog for `6cfc83a` while running `a92c76a`.
With Bluetooth selected for HID, the first download wrote 65,536 bytes before
the input path failed the maintenance idle check. After switching HID to USB,
the second download wrote 1,490,944 bytes. The next read was denied because
the coordinator's observation was 263 ms old against a 250 ms limit. Neither
attempt changed the boot selection. The record does not show what delayed
that observation.

With USB HID still selected, a development publisher spaced 4 KiB writes to
approximately 22 KiB/s. The device received, wrote and verified all 4,725,904
bytes of the same signed image. Activation selected the new slot. After
restart, the authenticated device reported the clean candidate commit and
matching ELF digest. This single transfer does not establish that pacing is
required or that it prevents stale observations.

The new image entered first-boot probation. The device did not confirm it
within the 60-second window and subsequently booted the previous `a92c76a`
image. After rollback, the management API reported the old image confirmed
and both saved hosts present. The available observations do not identify
which local health gate interrupted the required 45 continuous healthy
seconds. Bluetooth host switching on the candidate remains unqualified.

### USB first-boot diagnosis

The USB loader wrote and verified the 4,730,064-byte `1bab69b` application
in the inactive slot and selected it for a trial boot. The former confirmed
application and its selection record remained intact. On the new boot, SPI
color submission failed twice with `ESP_ERR_NO_MEM`. The first allocation
failure requested 8,190 bytes of DMA-capable memory. Later heap samples
reported a largest DMA block of 7,168 bytes.

The renderer error reset the first healthy interval after 18 ms and remained
present through probation expiry. Two brief IMU freshness failures cleared
while the renderer error persisted. The device rolled back. A fresh USB boot
identity and authenticated management read identified confirmed `a92c76a`
with both saved hosts present. The wheel gesture was not qualified on this
candidate during the trial.

The USB trial of `02f5a97` on the same board produced no recorded panel
allocation failure or renderer-error gate after the smaller SPI chunks were
installed. This image still rolled back. The IMU consumer reported a cache-age
timeout during startup. Subsequent samples were fresh, the worker was active,
the current error was clear and the recovery count stayed at two. The health
log continued to report `imu_fault` through the end of probation. That gate
read the board's retained first-fault flag. The management API subsequently
reported confirmed `a92c76a` and both saved hosts.

### Confirmed USB update and mouse wheel test

The USB loader wrote and verified all 4,730,192 bytes of `b511941` in the
inactive application slot. It preserved the previous confirmed slot and
settings. No backup was made. On first boot a brief IMU freshness failure
reset the health timer. After 45 continuous healthy seconds the application
requested confirmation. The management API reported the expected build and
ELF digest, successful confirmation, probation closed and two saved hosts.
The captured trial contained no display allocation failure or rollback.

On the Mouse page, the tester tapped both arrows, slid one finger up and down
from the gap between them, and held still before lifting. The tester confirmed
the directions and that scrolling stopped. macOS received 19 nonzero USB wheel
reports from the Mosaico, totaling 8 positive and 15 negative steps. This run
covers the USB wheel path on this board and host.

## Windows Bluetooth audio on Mosaico (2026-11-04)

The board booted and confirmed `1264c11` after a USB update to the inactive
slot. Both saved Windows pairings (HID and Classic audio) survived. On this
first boot HID was ready at 4.7 seconds and update probation ended at 52.2
seconds. The operator reported about a minute of unresponsive controls. The
coordinator rejects ordinary local input during probation. There is no
timestamp for the individual taps or observation of an ordinary cold boot in
this run.

**Speaker recovery passed in this run.** With Windows playing music over A2DP
at 44.1 kHz, the same playback job remained active for an observed 227.9
seconds and delivered 19,484,262 bytes to the codec. Two partial-frame
expiries discarded 642 aged samples. Fresh audio continued on the same link
and consumer, and the operator heard playback resume without pressing Start.
On the earlier `4392232` image, a comparable expiry disabled delivery until
the user restarted Speaker.

**Sound quality remains open.** The operator still heard interruptions or
distortion. The interval above included 164 incomplete ACL replacements and
177 media sequence warnings. The longest observed decoded callback interval
was 196 ms and the longest interval between codec writes was 200 ms. No A2DP
queue overruns, malformed PCM or LCD transfer errors were observed in that
interval. These observations do not identify where the fragments were lost.

Separately, the operator verified on `d736f52` that the Windows microphone
input meter followed speech from Mosaico over HFP. That microphone test was
not repeated on `1264c11`.

## Device evidence template

```text
Date / firmware commit / BSP commit / IDF commit:
Board revision / power / Wi-Fi state:
Host model / full OS version / Bluetooth adapter:
Mode / active profiles / layout / accessibility settings:
Steps / expected / observed / local evidence location:
Passes / failures / next action:
```

Keep raw logs and private host details local. Promote only reviewed, portable conclusions under the knowledge contract.
