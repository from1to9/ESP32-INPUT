# Mosaico audio and recording byte adapters

Run `python3 tests/board_audio/run.py` after resolving the pinned firmware
dependencies. Strict C11, AddressSanitizer and UndefinedBehaviorSanitizer cover
first-party production code. The ES8311 test compiles the actual pinned codec,
volume conversion and interface deletion implementations. SDK IO, scheduling
and the target-specific reference-manager lock are controllable boundaries;
the codec itself is not replaced. A pre-existing vendor signed/unsigned loop
comparison remains a visible warning in vendor compilation only.

## Audio boundary

Six microphone-level cases exercise the real adapter/codec with controlled SDK
PCM: integer RMS energy and peak, true silence, signed extrema, cadence,
short/failed/invalid transfers, mute/stop during IO, generation replacement and
backward/negative clock rejection. Only complete successful mono reads enter
the external-memory energy window. The cached snapshot getter performs no IO
or clock reads; capture revocation invalidates it before late IO can publish.
Numeric snapshots publish at most every 100 ms after the first valid read.
These digital measurements do not establish acoustic calibration or device UI
acceptance. The normal board suite runs these cases with its existing clock,
privacy, startup, real-engine and SDK capability regressions.

`board_audio_mosaico_init` creates one private worker and queues a muted boot
probe/teardown. Capabilities remain unavailable until that probe succeeds.
Readiness is a separate one-time obligation, outside the request mailbox. The
engine's mandatory initial stop can run before or after it without deleting
readiness or overwriting a running probe. The probe never enables PCM DMA.
Its own retained result distinguishes failed readiness from a later successful
stop. Composed real-engine/real-board/pinned-codec tests cover both startup
orders, an I2S ownership failure and a codec-control failure; no failure is
automatically retried and no media job is automatically started.
`board_audio_mosaico_port` implements the shared `ae_codec_port_t`; requests copy
their bounded payload, return `AE_AGAIN`, and require an identical poll to
consume their result. No caller buffer is retained. A second different request
returns busy. Safety mute/stop preempts unfinished media and closes software
egress gates immediately. The worker, rather than the manager or Bluetooth
callbacks, owns all codec, GPIO, power and I2S operations.

The resident worker retains its 6144-byte stack. Supported PSRAM/external-task
builds place that stack in PSRAM with an internal TCB; allocation failure is
reported without retrying internal RAM. Configurations without that support,
or with cache-off-safe I2C/I2S interrupts, keep the stack internal. Seven native
configurations check the exact creation API, capacity, no-fallback failure and
explicit retry, alongside DMA placement and both-channel cleanup. These tasks
have no deletion lifecycle; any future deletion must use the matching SDK API.

Explicit muted codec preparation uses a 100 ms I2C timeout argument, matching the
pinned codec-control default and the BSP touch peer on the shared bus. Runtime,
mute and cleanup calls retain their 5 ms argument. A first failed initialization
callback latches its exact error; later vendor callbacks cannot start another
SDK wait. Stop/mute cancellation is checked before each admitted initialization
control and before prepare commits, with cleanup using the original short
budget. No register operation, job or preparation is automatically retried.
The one-time boot probe retains its original 5 ms argument from its first SDK
call. Queued stop/mute closes PCM gates immediately but does not cancel that
already-muted readiness obligation inside the vendor's reference-counted enable
transition. The probe completes real teardown before stop acknowledgement;
genuine SDK or cleanup failure stays a failure, without reprobe or reference
repair. Nine real-engine interleavings inject initial stop during constructor,
sample configuration, enable and initial mute, including a blocked in-flight
SDK call, explicit mute arrivals and a preserved genuine enable failure. Their
clock advances during each SDK callback and the real engine polls concurrently;
successful cases acknowledge off within the unchanged 100 ms stop deadline.
The SDK applies its argument to bus admission and transfer waits separately;
this is not a claim that an entire initialization finishes within 100 ms.

Six real-codec budget cases cover finite six-millisecond shared-peer occupation
on read/write admission, admission still blocked after 100 ms, a real transfer
error, and stop/mute cancellation during preparation. They verify that later
vendor callbacks start no more waits after failure/cancellation, retain the
original SDK error, leave capture/playback gates closed, release only confirmed
ownership, and restore 5 ms runtime/mute/teardown behavior on every exit.

The physical ES8311 is a singleton in BOTH mode, with shared I2S0, MCLK 256,
stereo 16-bit wire slots, six DMA descriptors of 240 sample frames per direction
and a 20 ms maximum read/write wait. Codec I2C uses the BSP bus and 0x19 address,
with 5 ms individual transactions. There is no resampling. Supported rates are
8, 16, 44.1 and 48 kHz, with 10/20 ms media frames that fit the shared 1920-byte
limit. Capture is mono from the left wire slot. Stereo playback is explicitly
downmixed with a 32-bit sum before division, then duplicated to both wire slots;
this preserves media timing and never advertises stereo microphone capture.

The bounded capability table has capacity 16 and advertises all 14 accepted
rate/channel/frame-duration tuples. An independent explicit expectation table
checks uniqueness and the actual codec prepare/stop path for each. A composition
test uses the real audio manager and board capabilities for USB mono48k/10ms
capture/playback and local recognition mono16k/10ms admission. USB selection and
authority are explicit boundary inputs; the test does not claim a connected
host or acoustic output. Actual engine/board/pinned-codec execution additionally
checks cold Speaker without a capture consumer, Speaker-first and Mic-first
duplex, independent role stops/restarts and completed synthetic PCM frames.
Codec completion notifications run outside
the adapter lock and let the manager retire/enqueue frames at media cadence.

Prepare leaves PA/DAC muted and RX/TX DMA disabled. Capture mute stops RX DMA
and clears rejected samples; it does not claim analog ADC power-off while the
speaker is used. Stop disables both channels, holds PA low, verifies DAC mute,
flushes speaker DMA with zeroes, suspends/closes the codec and releases owned
resources. It leaves the shared display supply enabled. Failed teardown stays
uncertain. Pinned codec enable/suspend failures can leave reference counts
ambiguous; the adapter retains the codec identity and quarantines it until
reboot instead of inventing reference repair or a successful restart.

The pinned SDK makes the second initialized RX channel a slave of the TX
master. Capture therefore starts zero-preloaded TX clocks before RX, while
keeping DAC mute, PA low and the playback permission gate closed. Status
separates `tx_clock_running` from acknowledged `speaker_muted` and actual
`playback_running`. Stopping Speaker first acknowledges DAC/PA mute, briefly
stops TX to overwrite all old speaker DMA with zeros, and resumes the master
only for an independently active RX. Stopping Mic preserves an active Speaker;
the last role stops both channels. No IO, stop or startup deadline is extended.
The physical capture timing effect of the clock pause still needs device
qualification.

`--clock-only` runs the focused cold-Mic and shared-clock lifecycle regression.
The I2S boundary now models the pinned TX-master/RX-slave dependency: RX alone
returns timeout with zero bytes. This reproduces the former production failure
instead of fabricating samples. Regressions cover clock-only privacy, TX/RX
enable and disable failures, old DMA purge, next-generation cleanup and a stop
during clock enable. First and last named SDK errors remain in cached status
through successful teardown; the observer distinguishes I2S read/write,
I2C control and resource operations without logging PCM or identities.

Tests cover actual left-slot PCM extraction, stereo downmix including signed
limits, copied write identity, zero/partial/misaligned SDK transfers, exact
format/range bounds, stale generations, mute/stop during IO, rejected late read
delivery, cancellation of queued unmute, independent speaker data surviving
microphone mute, DAC/gain/volume readback, discarded codec write errors, driver
lifecycle quarantine, allocation failures and retryable resource cleanup.

## Internal owned recording region

`board_recording_flash_init` starts a separate worker and a read-only open.
The installed partition must have label `recordings`, subtype `0x40`, valid
aligned metadata and bounds within its actual flash chip. No physical offset
is encoded in this backend. A missing old-layout partition stays unavailable.
The current partition table provides 832 KiB; the first 4096 bytes hold a
versioned volume identity and CRC marker, leaving 828 KiB for the encrypted
store. This CRC is corruption detection, not authentication.

Opening never provisions or repairs. Explicit `provision` scans every byte
before claiming an erased region, commits its marker in two verified programs,
and does not erase. Repeating the same committed identity preserves all data;
another identity, corrupt/unknown contents, or interrupted unknown provisioning
requires migration. NOR program and one-sector erase verify readback and latch
fault on uncertainty. All data offsets exclude the ownership sector. Sync
observes completed synchronous SDK writes/readbacks and rechecks the marker;
there is no buffered filesystem or fabricated durability callback.

The current byte format supports the unencrypted partition used by the tracked
layout. Transparent partition encryption is rejected because its erased-byte
semantics differ. Application-level encryption belongs to the object store.
`board_recording_flash_device` binds directly to `ae_storage_device_t`, using
copied asynchronous read/program/erase/sync requests. Protected key retrieval,
durable nonce leases and trusted time must be supplied by their security/time
owners. Their readiness is never fabricated by this adapter. The byte backend
alone does not establish encrypted recording availability.

Tests retain a simulated NOR array across reopen and exercise explicit
provisioning, full-region erased scans, unknown-content preservation, old/missing
layout, geometry/bounds, owner-task enforcement, failed/partial marker writes,
no-op SDK writes/erase, exact payload identity, readback, restart and faults.

## External NAND preservation

`board_recording_nand_probe_start` performs only raw READ_ID and GET_FEATURE
commands, on its own worker. It never calls the logical page API, Dhara, format,
program, trim, erase or sync. It rejects an existing BSP/SPI owner and retains
resources if an accepted SPI transaction has not acknowledged completion.
The cached result distinguishes identification from unknown geometry/ownership;
external NAND recording remains unavailable pending backup and migration.

This is necessary on the pinned driver: `spi_nand_flash_init_device` ignores
the FTL initializer's result; Dhara initialization ignores map-resume failure;
and `spi_nand_flash_read_page` can rewrite pages after corrected ECC. A successful
initializer or logical read therefore cannot prove a safe read-only mount.
Tests enforce the raw command allowlist, owner preservation, valid versus empty
ID, uncertainty quarantine and unavailable store capabilities.

All evidence here is host-tested/source-reviewed. Target object compilation
does not establish acoustic output, microphone data/slot placement, simultaneous
IO, timing, safe loudness, resource margin, optical indicator visibility or
recording endurance. Device access is intentionally deferred by the user.
## Playback PCM observation

Run `python3 tests/board_audio/run_pcm.py --output-dir <new-local-directory>`
for the focused snapshot suite and the existing board/codec regressions.
It uses the real board adapter and pinned ES8311 with SDK boundary models.

`board_audio_pcm_snapshot()` copies a separate task-only external-BSS snapshot;
it does not grow the internal mailbox/status or perform I/O. Content counts
cover one normalized mono sample per stereo I2S time slot, after downmix, and
only the aligned, in-bounds prefix reported by `i2s_channel_write()`. Failed
SDK calls still contribute their valid reported prefix; `failed_writes`
counts nonzero SDK returns, not successful job delivery. Capture and preload
buffers do not contribute. Zero/invalid reported lengths cannot establish
known PCM content. Peak magnitude includes 32768 for `INT16_MIN`.

An owning prepare attempt resets scope; stop retains the final generation.
Separate flags distinguish scope, observed SDK result, observed samples,
requested volume and actual cached register reads. Volume/mute match flags
compare existing register reads with the request; they are historical values,
not a live hardware getter. Native tests do not qualify DMA/cache, target
placement, acoustic output or speaker stability.
