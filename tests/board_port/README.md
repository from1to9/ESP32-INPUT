# Board adapter host regressions

Run `python3 tests/board_port/run.py`. The suite compiles the actual
`board_mosaico.c` implementation and portable haptic component with strict C11
diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer. Controllable
driver doubles model the ESP-IDF/BSP boundary; they do not claim hardware or
radio success. Task creation records ownership and allocation, while the sensor
worker's stop path runs against a deterministic clock.

Checks cover raw physical down/up registration and timestamp delivery, release
registration before press, exclusive raw versus legacy hold callbacks, 900 ms
legacy hold, mandatory button failure, 100 Hz gyro/accelerometer configuration,
real acceleration values, cached reads without bus access, acquisition and
sample-age deadlines, untouched output on failure, missing data-ready flags,
init/start/read/task-allocation faults, non-finite values, a consumer fault
during an in-flight bus read, worker cleanup, clock rollback and 100,000 samples
across the 32-bit millisecond boundary. Sensor driver doubles assert that no I2C
operation occurs inside a critical section.

The cached timestamp is monotonic acquisition-completion time. The pinned BSP
has separate gyro and acceleration getters; the adapter calls both in one
bounded batch, which is not an atomic six-axis sensor frame. Actual ODR,
sampling jitter, bus timing, axis mapping, calibration quality, stack margin,
switch behavior and endurance still require device qualification.

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
