# Precise gyro propagation and braking candidate

Status: unqualified, 2026-10-23. The first frozen run completed successfully but
failed the movement acceptance matrix. No production input route selects this
candidate, and it must not be flashed as a Mouse fix.

This separately registered hypothesis tests exact quaternion gyro propagation
after initial stationary alignment, no in-motion acceleration-based tilt
correction, full causal velocity integration, and a conditional braking/stop
prior. It removes neither later gyro drift nor accelerometer bias. The
[portable component contract](../../../components/handheld_motion/README.md)
states ownership, units and reset responsibilities.

The original 28 fixtures and all 198 checks are unchanged. The synthetic source
still contains 41,873 frames. The new profile's 2000 counts/m gain was declared
before execution; it is the only intentional pointer-sensitivity change and
does not change the speed/direction/latency acceptance thresholds. The adapter
uses actual pinned Fusion only for the original safety-reference flags and
gates; compensated acceleration comes from the new observer. No analytic truth
is supplied to its compensation, velocity or stopping logic.

This is first-party experimental code, not a VQF port or a copied gait/oscillatory
tracking algorithm. The hypothesis follows the retained Fusion evidence: motion
acceleration can alter tilt feedback even with normal recovery flags. Standard
quaternion rigid-body propagation removes that feedback path; the braking prior
tests a control-state constraint without quiet-resetting known coast. Neither
step supplies an observation of accelerometer bias or exact rest. The adapter's
unchanged MIT Fusion subset and pin remain in the parent `vendor/Fusion/`
provenance; no additional dependency or external algorithm source was vendored.

## Reproduction

From the repository root, generate the original input freeze first if absent:

```sh
python3 experiments/mouse_motion/benchmark.py freeze
python3 experiments/mouse_motion/gyro_candidate/run.py freeze
python3 experiments/mouse_motion/gyro_candidate/run.py run
```

Use a clean copy for a new run; existing freezes and output directories are
preserved. The runner hashes the new component, adapter, protocol, portable unit
tests and original frozen inputs before compiling/executing. It builds with
strict C11 warnings and AddressSanitizer/UndefinedBehaviorSanitizer, writes all
frames and checks under ignored `run-1/`, then runs independent portable
invariants. Successful process execution is separate from diagnostic acceptance.

## First-run findings

| Outcome | Result |
| --- | --- |
| Cases satisfying every original criterion | 24 / 28 |
| Individual criteria satisfied | 190 / 198 |
| Overall acceptance | Failed |
| Portable invariant tests | Passed |
| Strict compiler and sanitizer diagnostics | None |
| Caller-owned state size on host | 496 bytes |

The passing scenarios include the first impulse, five-second constant-speed
coast, coherent acceleration/braking/reversal, continued fast/slow waves and
varied ideal grips, ideal sensor-center roll/yaw, small clock jitter, reset,
permission and interruption. These are synthetic results, not human usability
or device proof.

Four perturbation cases fail and remain in the complete result file:

| Case | Retained failure |
| --- | --- |
| Tilted moving wave with noise/bias | Velocity NRMSE 1.782; stop residual 0.648 m/s; direction and reversal failures |
| Stationary noise | 780 pointer-count path units over 10 s, allowed 5 |
| Stationary bias step | 6358 path units over 10 s, allowed 5 |
| Stationary noise/bias with tilted grip | 6369 path units over 10 s, allowed 5 |

The bias-step failures are worse than the earlier Fusion configurations, despite
better ideal-motion behavior. Initial alignment and precise propagation are
insufficient to bound long-term velocity. These data cannot justify activation,
nor can the four cases be removed or thresholds relaxed to obtain a pass.

The independent portable tests cover a sub-threshold onset impulse, five-second
known coast without a quiet reset, braking that remains forward until velocity
is canceled, stable stopping, tilted-axis mapping, 720-degree ideal sensor-center
rotation, cancellation, missing cadence and invalid measurements. Their success
does not override the four failing diagnostic scenarios.

Read-only review also identified an isolated snapshot inconsistency: the
explicit recenter frame sets `armed=true` while its phase still reads `UNARMED`.
The benchmark adapter separately identifies the recenter event, so this does not
change the retained movement metrics. It is a known issue for a separately frozen
revision before any adoption; the first run and its exact source remain intact.

No parameter changes followed the first result. Complete ordered real-sensor
captures with explicit action labels and an external movement reference are the
next prerequisite. Labels describe operator intent, not exact velocity truth.
The parent coordinator's capture-only firmware stage is separate from enabling
this observer. No firmware build, device write or publication was performed by
this component's author.

Local evidence: `freeze.json`, `run-1/results.json`, `run-1/gyro-braking.csv`,
compiler logs and `unit.*`. The first pre-execution freeze SHA256 is
`9eca291c3cd7e6c50c7245956f90f475737462e0ba4ef1fcc1375f33b1940097`;
the first result SHA256 is
`1aca54cf84b54f43dfff647537e73e97cbb4b7dd5a09383e0402ef29d6d31924`.
These index an exact local experiment; newly generated freezes naturally differ
when checkout-relative source documentation changes, even with identical math.
