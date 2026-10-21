# Board adapter host regressions

Run `python3 tests/board_port/run.py`. The suite compiles the actual
`board_mosaico.c` implementation and portable haptic component with strict C11
diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer. Controllable
driver doubles model the ESP-IDF/BSP boundary; they do not claim hardware or
radio success. Task creation records ownership and allocation, while the sensor
worker's stop path runs against a deterministic clock.

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
and distinguish unknown age from a fresh sample. Cache consumption retains the
existing 50 ms shutdown policy, including a stale consumer during an in-flight
batch; diagnostics do not recover the stream or change its acceptance limits.

Startup readiness has a separate 100 ms first-sample acceptance window. The
regression supplies accel-ready/command-ready before gyro-ready, accepts both
at 80 ms, rejects a missing gyro at the deadline, and rejects a batch completing
after that deadline. Runtime samples still expire at 50 ms and batches over
20 ms still fail. Bosch's [BMI270 datasheet, Table 3](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf#page=13)
lists 45 ms gyro suspend-to-normal startup at maximum ODR. The 100 ms window
budgets startup, a 100 Hz period, bounded acquisition and scheduling margin;
it is not a claim that the datasheet mandates 100 ms or that device motion has
passed qualification.

The cached timestamp is monotonic acquisition-completion time. The pinned BSP
has separate gyro and acceleration getters; the adapter calls both in one
bounded batch, which is not an atomic six-axis sensor frame. Actual ODR,
sampling jitter, bus timing, axis mapping, calibration quality, stack margin,
switch behavior and endurance still require device qualification.

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
fixture rejects gauge-control/profile writes and asserts the configured 3 ms
per-call timeout. Gauge/worker allocation failures remain truthful and bounded.

Brightness checks cover unknown initial level, queued versus applied state,
off/restore, pending-value replacement during a hardware write, rate limiting,
coalescing, lock/write failures, explicit retry and generation exhaustion.
A deterministic notification flood across 32-bit millisecond rollover verifies
that battery polls stay at most once per second while brightness attempts stay
at most once per 20 ms. The fixture verifies display-lock ownership and does
not provide display-off/deep-sleep substitutes. These checks do not establish
physical battery accuracy, charge wiring, luminance, idle power or endurance.
