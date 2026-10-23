# Mouse translation experiments

Scope: isolated host experiment. No production changes, target build, hardware,
Keyboard implementation, device writes or runtime control. Upstream Fusion is
unmodified and pinned in vendor/Fusion/provenance.json with its MIT license.

Before the first build or estimator execution:
    python3 experiments/mouse_motion/benchmark.py freeze

Then, once only (never overwrites an existing run-1 directory):
    python3 experiments/mouse_motion/benchmark.py run

freeze.json hashes every source, protocol, generated fixture and vendored input.
run verifies those hashes before building. All acceptance checks were selected
before running, and failed checks are preserved in run-1/results.json. No tuning
is permitted against this first run. Any implementation/fixture defect must be
documented and retained before a separately numbered revision is attempted.

Candidates: integrate uses each sample's compensated acceleration * dt, without
discarding onset samples; leaky solves dv/dt = acceleration - velocity / 1 s
for a held acceleration sample. This leak is a hypothesis to bound velocity
drift, not a physical drag model or a fixed maximum duration. Both retain state
for the full session, with explicit resets only at control/sensor boundaries.
Only pointer output has a 0.01 m/s deadband; velocity integration has no motion
onset threshold. Neither candidate estimates absolute position. Fractional
pointer counts are transport quantization and are cleared with velocity.

Arming requires an explicit rest/recenter event after actual Fusion startup.
This supplies a zero-velocity prior; the IMU cannot independently prove it.
Faults and output permission loss clear velocity/carry/arming and require a
new explicit rest/recenter event. That is a disclosed bench policy, not a
proposal to silently stop normal waving or a completed product interaction.
Gravity and axes use Fusion only; there is no direct gyro, tilt or Euler-to-
pointer term. Fusion's normal flags are not a claim of velocity accuracy.

The orientation-oracle runs use analytic grip acceleration through the SAME C
velocity/output algorithm and SAME actual Fusion gates. They isolate velocity
model error from gravity compensation error and are never product evidence.

The synthetic source supplies interval-average analytic translation acceleration
to isolate quadrature error. Real sampled accelerometers will not guarantee
this integral or capture every onset impulse. Source cadence must be established
at the board producer, not inferred from nominal consumer polling frequency.
100 Hz +/-2 percent jitter is included; outside +/-5 percent is explicitly
rejected. Fusion startup/rejection timeouts still count configured-rate samples.

Directions are compared by velocity angle <=30 degrees only when truth speed
is >=0.05 m/s. Speed error uses vector RMS relative to truth RMS over those
frames. Every half-wave must meet 90 percent aligned active coverage. Reversal
delay is the first output rate with the new sign after each true zero crossing;
an absent reversal is assigned a whole half-period delay. Stop is evaluated
300 ms after truth stops. Pointer drift is L1 integer-count path, not endpoint
cancellation. Pure rotation is ideal rotation about the sensor center; real
wrist-pivot rotation includes genuine sensor translation and is not equivalent.

Passed synthetic checks do not establish intuitive human control. This experiment
can reject a hypothesis; device data and a human usability trial would still be
required to qualify a successful one. Parameters are bench choices, not measured
device calibration, noise characteristics or accepted product thresholds.

Portable source archive and execution

This directory preserves unqualified research source only. It is not included
in firmware CMake or any production default. Python 3.9 or later and clang with
AddressSanitizer/UndefinedBehaviorSanitizer are required. No third-party Python
packages or network access are needed; the required MIT Fusion subset is pinned
and present under vendor/Fusion/.

Run the baseline freeze and run commands above from the repository root, then:
    python3 experiments/mouse_motion/rest-followon/run.py freeze
    python3 experiments/mouse_motion/rest-followon/run.py run

The independent normalization comparison uses the same baseline freeze and
criteria, changing only the documented upstream normal-square-root option:
    python3 experiments/mouse_motion/compare_normal_sqrt.py

Run each stage only once in a clean copy. Commands refuse to overwrite existing
evidence. Preserve previous evidence before choosing another clean checkout or
copy for a rerun; ignored files are not automatically disposable.

Execution success means the compiler/runner completed and saved results. The
diagnostic acceptance outcome is the printed passed field and each case/check
in run-1/results.json, rest-followon/run-1/results.json and normal-sqrt/results.json.
The preserved configurations fail the overall synthetic acceptance matrix.
Exit status zero does not mean product qualification. Do not weaken thresholds
or delete failed cases to turn the diagnostic outcome into a pass.

Generated fixtures, source freezes, run metadata, large CSVs and binaries remain
ignored local evidence. No raw physical capture or previous machine-specific
run metadata is distributed here. Freeze files are generated from the source
in this checkout before building, with relative file names in the manifests.
