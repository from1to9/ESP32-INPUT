# Handheld swing input: algorithm selection and qualification

Status: research and local prototype, 2026-10-23. The requested Mouse behavior
supersedes direct angular pointer control. No replacement estimator is currently
qualified for device use. Keyboard motion selection retains its angular behavior.

## Intended interaction

Move the pointer by moving the held device left/right or up/down. Holding
orientation participates in gravity compensation and direction mapping. The
user specifies an intuitive input result and use of the available inertial
sensors, not a particular integration formula or exact physical-position
measurement. Raw acceleration, tilt and angular velocity must not directly
become Mouse pointer velocity. Stopping should hold the pointer's position;
braking should not immediately reverse it. Continued intentional movement must
not silently stop after an arbitrary short timer. Touch priority, drag release,
physical controls, transport selection and authenticated remote ownership retain
their current contracts.

## Open-source candidates

| Source | Useful part | Reuse boundary |
| --- | --- | --- |
| [Fusion](https://github.com/xioTechnologies/Fusion/tree/a8d7224f36a0ec82345ef49a3db50e65f8d3bab8), MIT | Embedded C six-axis orientation, gravity and linear acceleration | First prototype candidate; does not estimate velocity or position |
| [VQF](https://github.com/dlaidig/vqf/tree/86ba56bdd3158b9b05f9f9fe5596866ba326438c), MIT | Six-axis orientation, rest detection and gyro bias estimation | Alternative orientation reference; fixed sample periods and C++ integration need explicit handling |
| [Gait-Tracking](https://github.com/xioTechnologies/Gait-Tracking/tree/250474fe11f460a67fa8628e82b7a9e8c5b4f96b), MIT | Demonstrates gravity removal, velocity integration and zero-velocity correction | Offline foot-mounted processing; future motion margins and endpoint drift interpolation are unsuitable for live HID |
| [Oscillatory tracking](https://github.com/xioTechnologies/Oscillatory-Motion-Tracking-With-x-IMU/tree/314208abbb592c9623ad0940138ea597447774a9) | Illustrates inertial integration and drift | Offline bidirectional filtering, periodic zero-mean assumption and return toward origin; no compatible project license established, so do not vendor |

The superseded [older gait demo](https://github.com/xioTechnologies/Gait-Tracking-With-x-IMU/tree/f877838b4dba55a8f32442275e2c10f56035f3a2)
is also an offline foot example, with no project license established during this
review. Source visibility alone is not a reuse license. Ordinary angular air
mouse examples do not establish the requested translation behavior.

Fusion is a candidate dependency, not an already adopted production dependency.
Its pinned API expresses acceleration in g; convert units once. Its sample-period
setter compensates small sample clock errors around the configured rate. Startup
and rejection timing retain sample-count semantics, so arbitrary variable-rate
consumption is not justified by that setter. Keep startup/recovery flags visible.
VQF's inferred rest is likewise not proof of zero linear velocity.

## Candidate processing boundary

1. Acquire ordered, timestamped accelerometer and gyro measurements through the
   existing sole board producer. Preserve sequence gaps and overflows explicitly.
2. Apply verified body axes and calibration. Estimate orientation and remove
   gravity. Gyro data contributes to orientation, never a direct Mouse command.
3. Use a stable grip frame for direction mapping during a movement. Estimate
   short-term movement with causal processing; retain initial acceleration and
   braking. Do not use future samples or offline corrections in live HID.
4. Convert the estimated movement to bounded relative HID reports, preserving
   fractional carry and actual sample intervals. A rest/reset discards future
   carry without rewinding already accepted host movement.
5. Reset on invalid samples, discontinuity, ownership changes and suppressed
   interaction; never replay the paused interval when movement resumes.

A candidate estimator must distinguish requested Motion, orientation readiness,
movement validity and sensor failure. A new gain unit must receive a versioned
setting rather than reinterpreting the existing HID/degree values. Algorithm
constants and qualification bounds belong to a documented profile.

## Limits that experiments must retain

The [BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf)
describes acceleration and angular-rate measurements. At a fixed orientation
and in the same gravity field, rest and uniform motion can have the same ideal
readings. IMU-only rest detection therefore needs an interaction assumption; it cannot universally
identify when the hand has stopped. Orientation error also leaks gravity into
movement estimates. Rotating about the wrist physically moves the sensor along
an arc, which cannot always be distinguished from intended translation with the
same six-axis observations.

These limits require measured operating behavior, not invented precision or a
blanket claim that all rotation is rejected. Expressing output as velocity
instead of position does not eliminate accumulated drift in relative HID output. An ideal sensor-center rotation can
be tested independently. Continuous tracking, changes of grip and very slow
movement require their own evidence. A 500 ms stroke experiment or a small-angle
bench condition is not an approved product timeout or permanent restriction.

## Acceptance before integration

- Analytic acceleration/coast/braking/reversal and varied grips; verify sign,
  speed response, causality, numeric bounds and no post-stop pullback.
- Ideal rotation, noise, bias, uncertain gravity, fast motion, timing jitter,
  missing data and every reset/suppression transition. Preserve failures.
- Replay complete raw sequences with a physical movement reference. Sparse
  extrema or isolated incident frames cannot validate a movement estimator.
- Measure the full firmware's sampling/queue/CPU/memory behavior, then observe
  actual USB and Bluetooth pointer response. User hand comfort remains an
  explicit acceptance criterion, independent of parser or transport success.

The first prototype remains outside the production input path until its results
support integration. A failed candidate must be reported with its actual limits;
changing the desired interaction or hiding failed traces is not a repair.

## Reproducible feasibility comparison: 2026-10-23

The [isolated experiment](../experiments/mouse_motion/README.md) preserves the
pinned source, analytic fixture generator, settings and evaluation criteria.
It is not linked into firmware. The original 28 cases contain 41,873 ordered
synthetic frames, including twelve-second waving, a five-second coast, braking,
ideal sensor-center rotation, perturbations and interruption. Parameters and
criteria were frozen before each separate experiment; no failures were removed.

| Actual Fusion configuration | Cases satisfying every criterion | Overall result |
| --- | --- | --- |
| Velocity integration with explicit rest/recenter | 11 / 28 | Not qualified |
| Integration with one-second velocity decay | 10 / 28 | Not qualified |
| Integration plus a causal quiet-period reset | 12 / 28 | Not qualified |
| Integration with upstream normal-square-root option | 12 / 28 | Not qualified |

These are case-level outcomes, not a device success rate. The first two
configurations have no automatic rest inference. Their quiet/recenter input is
an explicit interaction prior, not independently measured zero velocity. The
reset follow-on uses current sensor data, never ground-truth motion labels.

Direct integration continued serving the twelve-second regular waves but left
excess velocity after stopping. Decay reduced sustained coasting speed and
introduced early braking reversal, including when supplied with exact analytic
acceleration. The quiet-period reset improved some stopping behavior but also
reset during constant-speed travel and slow waving. Higher normalization
precision did not remove the dynamic stopping/braking failures. These results
reject only the tested configurations, not every possible inertial algorithm.

A privileged analytic-acceleration run isolates the integrator from orientation
error. It is explicitly a diagnostic oracle, never a substitute for actual
Fusion output or evidence of device performance. Strict compilation and
ASan/UBSan passed; completing the experiment is separate from satisfying its
movement criteria. Independent reruns retained complete outputs and failures.

The next implementation decision needs a full ordered real-sensor capture with
an external movement reference, then a separately registered estimator change.
The current latest-value handoff cannot supply every accepted sample to the
consumer. A bounded ordered queue can preserve accepted publications, but it
cannot recover missing hardware samples or establish a regular cadence by
itself. Sensor timing, gravity compensation, motion/rest inference and HID
response require separate evidence. No new Mouse estimator is enabled or
flashed by this research stage.
