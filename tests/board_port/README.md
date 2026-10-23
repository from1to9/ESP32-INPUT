# Board adapter host regressions

Run `python3 tests/board_port/run.py`. The suite compiles the actual
`board_mosaico.c` implementation and portable haptic, input and interaction
components with strict C11
diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer. Controllable
driver doubles model the ESP-IDF/BSP boundary; they do not claim hardware or
radio success. Task creation records ownership and allocation, while the sensor
worker's recovery/fatal paths run against a deterministic clock.

The independent raw observation journal retains 256 typed 88-byte records in
22,528 bytes of external RAM. Its SDK-independent record and payload contract is
[`board_sensor_journal.h`](../../firmware/main/board_sensor_journal.h); the API
owner explicitly encodes the fields rather than sending native structs. The
same existing acquisition owner records accepted IMU publications and typed
not-ready, late, communication, fatal, publication and startup failures. Accepted
float bits, units, axes and timestamps are unchanged. Hardware loss generation
is distinct from actuation delivery epoch and coordinator permission. A rejected
attempt has timing/status/error metadata, never fabricated zero axes. Missing
time, ready bits or source identity remain explicitly unknown. Clock failure
may retain a backward start/end pair with duration marked unknown.

Rejection markers do not discard previously retained accepted observations.
The existing actuation queue still performs its original segment cancellation;
its contracts, cache/recovery authority and acceptance thresholds are unchanged.
The journal appends a single bounded copy under the existing status lock, with
no allocation, extra sensor transaction, network operation or producer logging.
The existing power poll adds cached field values and validity, including gauge
temperature provenance. There is no additional power polling task or cadence.

Each journal reader supplies an independent last-delivered cursor. Reads copy
consistent oldest/latest bounds and at most eight records under the same lock,
without consuming records or mutating source health. Overwritten cursors first
receive a zero-record gap with its exact lost count and explicit resume cursor.
Future cursors and invalid arguments leave outputs untouched. Identifier
exhaustion stops retention without wrapping or affecting hardware; retained
history remains readable with the exhausted state. The API layer owns boot
identity and transport/subscriber loss reporting. The ring is bounded history,
not evidence of lossless sensor conversions or unlimited client backpressure.

`board_power_observe` derives the legacy power freshness result in an output
copy without clearing cache/capabilities; `board_power_read` keeps its existing
source-authority behavior. The generic input tee accepts only bounded touch and
button observations from their existing owners, rejecting IMU/power injection.
Touch-only redaction requires zero payload and validity. It does not replace a
touch callback, read GPIO/I2C or grant HID authority. Public touch change-event
coalescing and privacy eligibility belong to the coordinator.

Focused journal regressions cover two readers with different drain rates,
exact payload retention around rejected acquisitions, ring wrap/overwrite gaps,
read-only isolation, untouched invalid/future-cursor outputs, startup readiness
and failures, hardware generation across recovery, clock rollback, ID exhaustion,
power provenance and pure age/clock observation, and input whitelisting and
redaction. These are host proofs. Target external-RAM placement, lock duration,
stack margin, transport delivery and device performance require integration
and exact-image hardware validation. Historical capture evidence is unchanged.

The optional ordered delivery API uses the same sole acquisition owner and
unchanged native six-axis samples. The SDK-independent
[`board_imu_stream.h`](../../firmware/main/board_imu_stream.h) defines frames,
delivery states and discontinuity reason bits. The private portable
[`board_imu_queue.c`](../../firmware/main/board_imu_queue.c) owns eight fixed
slots; the adapter serializes queue, cache and status under its existing lock.
There is no new task, allocation or bus reader. Native host measurements are
32 bytes per sample, 48 per paired frame, 48 per batch result and 456 for the
complete queue (384 bytes of frame payload). These are host layout measurements;
the target build and device timing/stack margin require separate verification.

Attach grants one nonzero consumer token. Every new attachment gets a distinct
token; source loss and fences retain that ownership token but advance delivery
epoch. The producer captures its epoch before acquisition, then pairs the
sample, independent accepted-publication sequence and epoch under the same lock.
An in-flight pre-fence/pre-attachment frame may refresh the legacy cache but
cannot enter a new delivery epoch. Diagnostic sample-count saturation cannot
repeat an operational sequence. Exhausted sequence/epoch/token identifiers fail
delivery closed without wrapping or fabricating a hardware failure.

Drain copies at most eight frames in order and acknowledges a pending
discontinuity with zero frames before later data. `ESP_OK` confirms a valid
query; only `BOARD_IMU_DELIVERY_DATA` with a positive count delivers samples.
Empty, discontinuous, unavailable and permanently failed delivery are distinct.
`source_error` independently observes current cache/hardware availability. Invalid
arguments or ownership tokens leave caller outputs untouched. Overflow discards
the entire buffered segment and incoming frame, reports an explicit reason and
counts those accepted frames as lost. A stale prefix discards the whole segment
even when its latest frame is fresh. Exactly 50 ms remains acceptable. Late or
not-ready acquisition reports a delivery rejection even when the unchanged
legacy cache is still usable. Queue loss alone never changes capability,
recovery generation or first-fault history. Ordered drain is observational:
even expiry of the latest cache or a backward clock only invalidates delivery,
returning the observed timeout while leaving every legacy source field and
read marker unchanged. Legacy consumption/acquisition independently retain
their existing fail-closed hardware policy.

Consumers should keep their bounded batch buffer in persistent storage, process
real timestamps outside the adapter lock, and recheck freshness and their own
control epoch before producing output. Fences do not retract already copied
caller data. The queue cannot reconstruct missed ODR samples, prove uniform
10 ms cadence or qualify an estimator. Intentional attach/fence reasons must
remain distinguishable from source/overflow/stale/rejected loss. Lost-frame
diagnostics count accepted frames canceled while subscribed (including detach),
not rejected reads or publications while no subscriber exists; the lifetime
total saturates independently of operational identity.

The queue regression delivers 150,000 accepted frames exactly once with a
100 Hz producer and a 66.7 Hz consumer. It also checks partial drains/ring wrap,
unchanged float representations and timestamps, explicit whole-segment overflow,
50 ms boundaries, a stale prefix with a fresh latest frame, clock/order loss,
in-flight fences acknowledged before publication, detach/re-attach ownership,
invalid-output preservation, unavailable source, diagnostic saturation and
identifier exhaustion. Adapter regressions exercise the real publication path,
cache/queue coexistence, in-flight fences and replacement consumers, startup
failure, late/not-ready rejection, source loss/recovery, retained first-fault
history, observational stale/clock rejection and delivery failure independent
of source health. Driver doubles still
reject sensor I/O inside the critical section.

Display setup checks create the panel and install the first-party transfer
observer before the adapter can register its callback/start rendering. A panel
creation error stops setup; unavailable observation leaves the ordinary display
working while capture presentation remains unconfirmed. The observer itself is
tested in [the display transfer suite](../display_transfer/README.md).

Checks cover raw physical down/up registration and timestamp delivery, release
registration before press, exclusive raw versus legacy hold callbacks, 900 ms
legacy hold, mandatory button failure, 100 Hz gyro/accelerometer configuration,
real acceleration values, cached reads without bus access, acquisition and
sample-age deadlines, untouched output on failure, missing data-ready flags,
init/start/read/task-allocation faults, non-finite values, a consumer fault
during an in-flight bus read, worker cleanup, clock rollback and 100,000 samples
across the 32-bit millisecond boundary. Sensor driver doubles assert that no I2C
operation occurs inside a critical section.

IMU health snapshots verify retained initialization/start milestones, accepted
sample counts and saturation, last hardware/cache-read status, data-ready bits
and the first fault's stage/error/time across cleanup and later failures.
Repeated diagnostic reads perform no bus access, cannot latch an expired cache,
and distinguish unknown age from a fresh sample. Cache consumption stops output
after the unchanged 50 ms freshness limit. This is an output gate rather than
permanent sampler shutdown: late/not-ready/expired-cache loss and Bosch
`BMI2_E_COM_FAIL` enter unavailable recovery with normal scheduling followed by
10/20/40/80/160/250 ms capped backoff. A persistent disconnected device remains
unavailable without reset/reinitialization or a tight retry loop. A later newly
acquired, finite and monotonic frame can restore capability; user Motion intent
is not restored. Clock rollback, nonfinite conversions, missing handles,
inconsistent configured ranges, unknown driver failures and out-of-order
timestamps remain fatal. First-fault history is retained separately from the
current phase. Diagnostic reads never recover a stream or alter acceptance.

The loss generation advances once on entry into recovery, not on repeated
failed cache reads. An in-flight pre-loss frame cannot restore capability;
genuine fatal outcomes still win over a concurrent transient generation change.
Tests cover both races, early communication/not-ready clock rollback, recovered
counter saturation and generation exhaustion. No failed frame advances the
accepted count or cached timestamp.

Startup readiness has a separate 100 ms first-sample acceptance window. The
regression supplies accel-ready/command-ready before gyro-ready, accepts both
at 80 ms, rejects a missing gyro at the deadline, and rejects a batch completing
after that deadline. Runtime samples still expire at 50 ms and batches over
20 ms still fail. Bosch's [BMI270 datasheet, Table 3](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf#page=13)
lists 45 ms gyro suspend-to-normal startup at maximum ODR. The 100 ms window
budgets startup, a 100 Hz period, bounded acquisition and scheduling margin;
it is not a claim that the datasheet mandates 100 ms or that device motion has
passed qualification.

The cached timestamp is monotonic acquisition-completion time. The pinned BSP's
gyro and acceleration getters each issue a complete corrected Bosch read. The
adapter instead calls that same driver once, checks the returned frame's status,
and applies the explicitly configured BSP-equivalent 4 g/2000 dps range scales.
The native boundary supplies real integer axis counts, including quantization.
This is one full-register burst, not proof of an atomic ODR snapshot. Actual ODR,
sampling jitter, bus timing, axis mapping, calibration quality, stack margin,
switch behavior and endurance still require device qualification.

One persistent sensor owner serializes runtime IMU and gauge work at priority 6,
the pinned LVGL priority; radio/system priorities remain higher. It has a 4 KiB
internal stack. The IMU gets the first opportunity per normal 10 ms schedule,
and gauge work is admitted at most once per second. Missed periods are skipped;
there is no catch-up queue. Brightness retains its separate worker because its
display-lock wait can reach 100 ms. Long battery batches still fail the original
20 ms gate and cannot permanently stop IMU acquisition. Scripted schedules cover
long successful-but-late gauge packets, brightness producer floods, persistent
transport failures and later recovery. The SDK's shared I2C binary semaphore
does not provide priority inheritance; this policy does not prove global
touch/codec bus fairness, blackout tolerance or hardware timing.

Worker-creation failures preserve independent ownership: a failed brightness
task does not remove the gauge; a failed sensor task fails IMU/gauge closed and
attempts bounded cleanup, retaining any failed-cleanup handle. Fatal IMU cleanup
runs once; the sensor owner may continue battery polling. Memory qualification
must charge the live sensor stack and TCB, including after a recoverable fault.

`board_imu_read` supplies both acceleration and angular velocity in the same
right-handed display frame (X right, Y top, Z toward the viewer):
`(sensorX, sensorY, sensorZ)` for the current Mosaico panel orientation. The
identity transform has determinant +1; both vectors use the same basis.
The basis-vector regression checks both vectors, orthonormality,
the determinant +1 cross-product invariant, unchanged raw cache and timestamps,
and no sensor access during consumption. Consumers use these fields directly.

Physical evidence from Mosaico v1.2 selects this basis. These poses do not
physically qualify v1.0 or other board profiles. Flat screen-up observations put
specific force on native +Z. A confirmed text-upright, screen-facing inclined hold put it
on native +Y with positive Z, while the former extra `(-Y,X,Z)` rotation reported
X=-0.851..-0.816 g, Y=-0.032..0.023 g and Z=0.502..0.562 g. A native-boundary
fixture derived from that hold fails the former mapping. It checks the screen
top axis, rather than merely asserting the chosen matrix. The first captured
window included movement; only the subsequent settled windows support this
fixture. Earlier mislabeled edge-lift trials are not mounting evidence.

The [factory orientation code at commit 2e1017e](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/main/platform/mosaic_imu_platform.c#L113-L125)
describes a 90-degree mounting and remaps Euler UI Pitch/Roll. Its
[bubble widget](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/mosaic_ui/apps/imu/imu_app.c#L70-L75)
uses those angles as pixel positions. These UI labels do not independently
define this product's three-dimensional body axes; translating them into an
additional vector rotation was not supported by the physical hold evidence.

The cross-layer regression links the actual board adapter to the production
interaction model. It calibrates at flat, observed inclined and upright grips,
enables Motion, then feeds twelve new matching zero-gyro frames for intentional
reference acquisition. It checks that the reference remains unavailable before
100 ms and that the acquisition-completion frame still reports reference
suppression, while calibrated bias and Motion intent are preserved. Only then
does it supply single-axis pitch and world-up yaw in both directions through
the Bosch integer boundary. It checks independent horizontal/vertical output, fresh
gravity tracking and geometry through every frame. This is host evidence;
dynamic physical direction and feel still need device qualification. Removing
the mounting error does not fix or qualify fast-motion reference loss.

Power checks use controllable checked I2C responses at the BSP-owned bus
boundary. They verify little-endian readings, signed current, gauge readiness,
actual 0% versus unknown percentage, absent-cell versus unavailable-gauge
states, unknown external power, zero-current charging uncertainty, every
register-failure boundary, batch/staleness limits and later read recovery. The
fixture rejects gauge-control/profile writes. The checked reader uses three
incremental-read packets: OperationStatus, the contiguous 0x06..0x0d block
(temperature, voltage, battery flags and signed current), and StateOfCharge.
The [TI BQ27220 datasheet, sections 7.3.1.1/3/4](https://www.ti.com/lit/ds/symlink/bq27220.pdf)
and [technical reference manual, sections 5.1/3/4](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf)
document incremental addressing, at least 66 us between gauge packets at
400 kHz, and up to 4 ms of clock stretching. The reader uses a 100 us packet
gap, 5 ms SCL wait request and 6 ms software timeout, replacing the insufficient
3 ms software and S31 default 2.5 ms hardware limits. The original 20 ms
whole-batch acceptance, one-second poll and five-second freshness limits remain.
Tests cover legal 4 ms stretches on every packet, exact batch boundaries,
late all-success batches, raw bus timeouts, clock rollback, failed packets and
later recovery. Failure warnings retain the actual error and add read versus
batch/clock stage, command and known elapsed time; no failed reading is made
successful. Gauge/worker allocation failures remain truthful and bounded.

Brightness checks cover unknown initial level, queued versus applied state,
off/restore, pending-value replacement during a hardware write, rate limiting,
coalescing, lock/write failures, explicit retry and generation exhaustion.
A deterministic notification flood across 32-bit millisecond rollover verifies
that battery polls stay at most once per second while brightness attempts stay
at most once per 20 ms. The fixture verifies display-lock ownership and does
not provide display-off/deep-sleep substitutes. These checks do not establish
physical battery accuracy, charge wiring, luminance, idle power or endurance.
