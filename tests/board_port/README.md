# Board adapter host regressions

Run `python3 tests/board_port/run.py`. The suite compiles the actual
`board_mosaico.c` implementation and portable haptic component with strict C11
diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer. Controllable
driver doubles model the ESP-IDF/BSP boundary; they do not claim hardware or
radio success. Task creation records ownership and allocation, while the sensor
worker's recovery/fatal paths run against a deterministic clock.

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

`board_imu_read` rotates both acceleration and angular velocity into the same
right-handed display frame (X right, Y top, Z toward the viewer):
`(-sensorY, sensorX, sensorZ)`. Acquisition/cache diagnostics stay in sensor
coordinates. The basis-vector regression checks both vectors, orthonormality,
the determinant +1 cross-product invariant, unchanged raw cache and timestamps,
and no sensor access during consumption. This replaces the former improper
permutation at the application boundary; consumers must use these fields
directly rather than rotate them again.

Provenance: the [official factory orientation code at commit 2e1017e](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/main/platform/mosaic_imu_platform.c#L113-L125)
documents a 90-degree clockwise BMI270 mounting and maps screen pitch to sensor
roll, screen roll to negative sensor pitch, and leaves yaw unchanged. The vector
rotation follows that code's roll/pitch conventions. This is source-grounded
evidence; installed display orientation, positive physical direction and air
pointer feel remain pending physical qualification. The BSP getters themselves
only scale sensor values and do not establish chip placement.

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
