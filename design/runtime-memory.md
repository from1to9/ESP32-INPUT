# Combined runtime memory ownership

This plan applies to the pinned ESP32-S31 Mosaico profile. It preserves the
combined HID, audio and station LAN capabilities. Linked placement and host
regressions are separate from physical coexistence and endurance qualification.

## Observed failure and resource ownership

The stack-repair device run maintained increasing uptime but could not allocate
an 8,190-byte internal DMA buffer. The LCD SPI transfer failed after the logical
boot-to-product handoff, leaving old boot pixels visible. The same run failed
to create the Wi-Fi worker and reported audio initialization failures. Available
PSRAM did not satisfy these explicit internal-memory requests.

The pinned BSP submits a full 480 x 480 RGB565 PSRAM frame through the stable
internal-bounce SPI path. Its ten-entry transaction queue can retain ten rounded
8,190-byte bounce chunks until results are recycled: 81,900 payload bytes plus
metadata. The board policy caps only the matching display host and chip select
at one queued transaction by default. The SDK still sends every chunk and the
final completion callback. The ceiling is configurable as one or two; increasing
it requires renewed device memory and throughput qualification. The transfer
observer remains observational. Vendor sources and direct-PSRAM DMA policy stay
unchanged.

The 73,710-byte difference between queue bounds is not a promised increase in
measured free memory. Asynchronous drawing overlapped Bluetooth startup in the
failed capture, so the stage-to-stage heap difference cannot be attributed to
Bluetooth alone.

## Placement and concurrency

| Resource | Placement policy | Capacity and evidence boundary |
| --- | --- | --- |
| LCD pixels | Existing PSRAM frame, internal DMA bounce | Full 460,800-byte frame; default at most one 8,190-byte queued payload plus metadata |
| I2S payload | PSRAM DMA only on supported PSRAM targets without IRAM-safe I2S interrupt mode | Six 240-frame stereo 16-bit buffers per direction; 11,520 payload bytes for duplex, descriptors remain internal |
| UI and coordinator copies | Explicit fixed external BSS for audited task-only buffers | Capacities and lifetime unchanged; queue controls, locks and ISR data remain internal |
| Wi-Fi buffers | Two static RX, four static TX, existing finite PSRAM cache | Approximate static RX/TX/management payload 12,100 bytes before driver tasks, queues and control overhead |
| Optional Wi-Fi speed code | SDK flash-text placement with the existing PSRAM XIP profile | Mandatory interrupt/cache-disabled code retains its separate SDK placement; throughput and power need device measurement |
| TLS allocations | Explicit PSRAM allocator | Existing 16,384-byte inbound and 4,096-byte outbound record sizes, certificate/hostname/date/signature checks unchanged |
| Flash and NVS owners | Internal stacks | Main coordinator, Wi-Fi management, trust/runtime and updater retain internal stacks |
| Cache-enabled network workers | Explicit external stacks on supported builds | Capacities remain unchanged; allocation failure is reported, without an internal fallback on a supported PSRAM build |
| Board audio and media control workers | Explicit external stacks on the audited PSRAM profile | Two 6-KiB stacks with internal task control blocks; unsupported configurations retain internal stacks, and supported allocation failure has no internal fallback |
| IMU and battery acquisition | One persistent internal sensor-worker stack | 4 KiB plus its task control block; IMU has priority within each iteration, battery is sampled at most once per second, and brightness uses a separate worker |

I2S payload allocation failed in the original run; moving that payload avoids a
future internal peak rather than reclaiming an already resident 11,520 bytes.
Likewise, modules whose workers failed to start must be included in the repaired
runtime budget instead of counting them as already resident.

The declared first-party network stack capacities are 16 KiB for Wi-Fi, 12 KiB
for management, 4 KiB for clock and 16 KiB for firmware updates. A live server
adds 4 KiB for its owner and 12 KiB for HTTPS; a recording download adds 8 KiB.
Audio worker capacities total 26 KiB, including the two external 6-KiB stacks,
with a separate 4 KiB recording-flash worker when needed. SDK tasks, TCBs,
queues, allocator overhead and cryptographic
workspaces are additional costs. These are declared capacities, not observed
stack usage or a whole-program upper bound.

External network stacks do not move their TCBs. The pinned SDK's capability-aware
self-deletion can create a temporary internal cleanup task with a 1,536-byte
stack plus TCB; a shutdown wave involving server, HTTPS and download must retain
room for each cleanup. Three cleanup stacks are a planning allowance for that
wave, not a global maximum: a later admission can overlap an earlier deferred
cleanup. Repeated reconnect/download teardown therefore needs measured
headroom and endurance qualification. Cache-disabled storage access remains with internal-stack
owners. A future callback that performs flash access invalidates the external
stack call-chain audit and must be reviewed before integration.

TLS PSRAM placement includes session secrets and handshake material. This
profile does not claim encrypted external memory or hardware key isolation;
secure boot and flash encryption are separate provisioning decisions. No fuse
or security provisioning change belongs to this repair.

## Follow-up to the physical resource test

The second device repair cleared the boot logo and restored physical navigation,
but its measured steady internal free memory was 20,847 bytes with a largest
block of 10,752. Both remained below the existing Wi-Fi scan admission limits;
the user also observed a failed scan. Initialization alone did not establish
usable LAN operation. The full failed result remains in the
[validation record](../docs/validation.md).

The next bounded candidate reduces static TX buffers from eight to four. The
pinned SDK supports 1..64 and estimates approximately 1,600 bytes per buffer;
the estimated 6,400-byte saving is not a measured heap gain. The existing finite
32-packet TX cache, two static RX buffers, RX Block Ack window, dynamic RX limit
and TLS record limits stay unchanged. A full transmit pool uses the SDK's
bounded cache; this does not guarantee throughput or a successful send. Existing
transport backpressure, deadlines and exact-grant cancellation still apply.

The board audio worker and media control worker use explicit external stacks
on the audited PSRAM profile, with internal task control blocks and unchanged
6-KiB capacities. Their callback audit excludes flash/NVS work; board audio also
requires the existing non-IRAM-safe I2C and I2S interrupt configuration. The audio
manager retains its internal stack because recording
service executes NVS key and nonce operations. The speech worker's model mapping
and opaque recognition library are not qualified for an external stack. The
updater remains resident and internal; deferring it would merely move its
allocation to an update-time peak.

Combining the two stack changes and TX reduction projects about 39,535 bytes
free from the measured baseline, only about 6,767 above the scan total threshold.
This arithmetic does not prove a sufficiently large contiguous block or cover
later driver, connection, audio and teardown allocations. Scan/join, BLE input,
HTTPS, bounded audio transfer and repeated teardown must be measured together
before claiming coexistence. The scan acceptance thresholds remain unchanged.

## Allocation alignment and state publication

Placement and alignment are separate requirements. The pinned generic
`SPIRAM | 8BIT` allocator can provide only four-byte alignment while some
production objects require eight. Opaque engine, store, protocol and adapter
objects expose their required alignment; callers request it explicitly and
initializers reject misaligned storage before accessing it. Typed owner
allocations use their type's alignment. Existing memory-capability and fallback
policies remain explicit; byte buffers do not acquire arbitrary extra alignment.

An initialization failure that returns no engine cannot publish a known audio
snapshot, dispatch work or read cleanup fields. The owner retains the failure
and releases its resources. An initialized engine reporting a hardware wait or
fault remains responsible for its existing stop/recovery lifecycle. Callback
unregistration must also tolerate a completion already copied by another worker;
late wake notifications cannot reference released owner storage. Weak-alignment
and initialization-failure tests exercise these production ownership boundaries.

## Acceptance boundaries

- Verify the final linked sections and effective generated configuration, not
  only defaults or native tests where placement attributes are empty.
- Confirm physical complete redraws and increasing UI updates after boot and
  repeated mode/menu transitions; old physical pixels must disappear.
- Confirm asynchronous Wi-Fi, audio and control initialization independently.
  USB enumeration and generic uptime alone do not prove these modules work.
- Capture free and largest blocks for both internal 8-bit and internal DMA
  capabilities during display, radio, audio and LAN activity. A large total
  heap does not establish usable DMA capacity.
- Preserve Wi-Fi scanning's existing 32 KiB free internal and 16 KiB largest
  internal block admission gates. Exercise scan/join and bounded network,
  Bluetooth and audio concurrency, including teardown and reconnect peaks.
- Keep allocation errors, unknown module state and failed operations visible.
  Short captures do not satisfy the separately specified powered endurance gate.

The IMU's rejected acquisition batches are a separate timing issue. Memory
placement alone does not establish sensor recovery or correct pointer motion.

## Persistent sensor worker

The sensor recovery candidate keeps the acquisition worker alive after transient
communication or freshness failures. Consequently, heap captured while the
earlier sampler was stopped cannot qualify a healthy concurrent workload.
Conservatively budget the entire 4,096-byte live stack and 100-byte target task
control payload, plus allocator and alignment overhead, against that failed-state
baseline. The old capture does not prove when the previous task's storage was
reclaimed; this is a planning allowance, not an exact heap delta.

The measured target IMU call chain is 944 bytes, including the Bosch native I2C
callback, split stack adjustments and bounded SDK address arrays. Adding the
2,048-byte engineering reserve fits the retained 4-KiB allocation. This does not
bound every ROM, logging, allocator, RTOS or interrupt path. The current logs
measure the coordinator's stack watermark only; the sensor's actual stack
watermark remains unverified and must not be inferred from that value.

The 20-ms acquisition budget and 50-ms freshness limit are unchanged. A late
sample is rejected, and recovery requires a new monotonic, fresh acquisition.
Bounded retry delays do not provide a hard upper bound on opaque SDK I2C calls.
Validate increasing accepted samples, live-worker state, freshness and internal
heap during connected Wi-Fi scans before accepting sensor/radio coexistence.
