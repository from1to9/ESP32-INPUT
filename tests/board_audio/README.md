# Mosaico audio and recording byte adapters

Run `python3 tests/board_audio/run.py` after resolving the pinned firmware
dependencies. Strict C11, AddressSanitizer and UndefinedBehaviorSanitizer cover
first-party production code. The ES8311 test compiles the actual pinned codec,
volume conversion and interface deletion implementations. SDK IO, scheduling
and the target-specific reference-manager lock are controllable boundaries;
the codec itself is not replaced. A pre-existing vendor signed/unsigned loop
comparison remains a visible warning in vendor compilation only.

## Audio boundary

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
host or completed media execution. Codec completion notifications run outside
the adapter lock and let the manager retire/enqueue frames at media cadence.

Prepare leaves PA/DAC muted and RX/TX DMA disabled. Capture mute stops RX DMA
and clears rejected samples; it does not claim analog ADC power-off while the
speaker is used. Stop disables both channels, holds PA low, verifies DAC mute,
flushes speaker DMA with zeroes, suspends/closes the codec and releases owned
resources. It leaves the shared display supply enabled. Failed teardown stays
uncertain. Pinned codec enable/suspend failures can leave reference counts
ambiguous; the adapter retains the codec identity and quarantines it until
reboot instead of inventing reference repair or a successful restart.

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
