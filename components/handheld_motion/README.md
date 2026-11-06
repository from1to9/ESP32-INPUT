# Historical handheld translation observer

This experimental component records the earlier translation research. Current
Mouse Motion uses rotational aiming in the [interaction
component](../interaction/README.md).

Status: unqualified, 2026-10-23. This independent portable component has no caller
in the production input engine. Do not enable it as a replacement Mouse path.
The [frozen comparison](../../experiments/mouse_motion/gyro_candidate/README.md)
retains its failures. Keyboard selection and existing pointer gain settings are
not changed.

Run focused host invariants from the repository root with
`python3 tests/handheld_motion/run.py`. This preserves compiler and execution
evidence under ignored `.agent/local/`; it does not run a target build or qualify
the failed movement diagnostic.

The observer removes gravity using an initial stationary alignment followed by
quaternion propagation from measured gyro. Gyro is never directly converted to
pointer movement. Translation acceleration is integrated into velocity; relative
pointer counts come from that velocity. No position estimate is maintained.

Initial alignment assumes 300 stable samples with approximately 100 Hz cadence.
The mean gyro supplies an initial bias and mean specific force supplies gravity.
The variance and magnitude limits are declared in the experiment protocol.
Six-axis heading is arbitrary; the same heading in the recentered grip anchor
cancels when acceleration is mapped back into grip X/Y.

After alignment there is deliberately no accelerometer tilt correction. This
avoids learning sustained translation as tilt, but leaves orientation drift and
new accelerometer bias unresolved. The observed perturbation failures are large;
an `aligned` flag is not a certificate of accurate velocity or safe pointer use.

Every valid armed sample contributes to velocity immediately, including initial
sub-deadband impulses. There is no velocity leak, motion-duration cutoff or
reset merely because acceleration becomes quiet. A braking history followed by
estimated speed below the pointer deadband and four low-residual samples can
supply a stop prior. This is a control assumption, not measured zero velocity;
very slow coasting and stopping remain ambiguous.

The default profile uses 2000 pointer counts/m, distinct from the production
HID/degree settings. This new bench gain was registered before execution to
evaluate the unchanged first-event latency criterion. It is not a migrated user
setting or an approved product default.

## Ownership and integration boundary

Use one serialized coordinator owner and fixed caller-owned state. There is no
allocation, I2C access, callback, task, UI, HID transport or shared mutable global
inside this module. The host state is 496 bytes; target memory, stack, execution
cost and timing remain unmeasured. Double-precision trigonometry is intentional
for this host hypothesis and needs target evaluation before adoption.

1. Initialize with a validated profile. Establish an explicitly requested
   stationary alignment interval; low gyro and near-1-g force cannot themselves
   distinguish rest from constant velocity.
2. Feed each ordered producer frame exactly once. The sample timestamp determines
   the interval; never use drain arrival time or repeat a latest-value cache.
   The default 9.5..10.5 ms envelope is a bench choice, not a verified board
   cadence specification.
3. On delivery loss, epoch discontinuity, rejected/stale prefix, source failure
   or missing cadence, call `handheld_motion_discontinuity` and retain the actual
   failure evidence in the owning board/coordinator. No missing sample is filled.
4. Permission loss calls `handheld_motion_suppress`, which clears velocity,
   fractional carry and arming while preserving orientation. An explicit
   permitted recenter supplies a zero-velocity prior and records the grip frame.
   It does not independently observe that the operator has stopped.
5. Revalidate engine/source/control epoch at eventual HID submission. Clear any
   pending transport movement in that owning layer; this observer cannot retract
   an already queued or delivered report. Freshness and ownership are caller
   responsibilities in addition to the per-sample numerical checks.

`handheld_motion_update` reports whether the sample was numerically accepted.
Inspect the output phase, alignment and arming separately. Sensor faults clear
the observer; a successful call during alignment or suppression does not imply
an admissible movement report. Production calibration, UI capability state,
setting versioning and recovery policy remain outside this isolated API.

The experiment adapter additionally retains actual pinned Fusion startup,
overrange and acceleration-recovery admission gates. These are benchmark
reference checks, not a claim that this component includes Fusion or can supply
those external gates by itself. Integration must explicitly resolve that boundary.
