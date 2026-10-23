# Ordered raw motion capture

Status: diagnostic implementation, pending exact-image device qualification.
This is a prerequisite for [handheld swing input](motion-estimation.md), not an
enabled replacement Mouse estimator. The existing latest-value interaction path
remains unchanged. Keyboard selection, HID authority and transport policy do not
consume the new diagnostic queue.

## Build and lifecycle

The ordinary build defaults `CONFIG_APP_IMU_CAPTURE` to disabled. Enable it in
the local SDK configuration for a reviewed diagnostic build. The existing build
helper still never flashes a device. Preserve the binary, ELF, configuration,
source identity and hashes together before deploying a diagnostic image.

One coordinator attaches to the bounded eight-frame board stream. This is a tee
of accepted publications from the existing sensor worker, with no additional
sensor read or task. Operational sequence and delivery epoch are paired with
each sample. The public latest-value API remains available. Stream attachment,
fences, overflow and stale observation must not change legacy hardware health,
recovery, failure history or input state.

In Mouse mode, an observed Motion rising edge starts a capture if the observer
is idle. A stream fence excludes queued and in-flight pre-start samples. The
coordinator copies at most eight rows per drain into 6000 preallocated PSRAM
records. A record is 88 bytes on the target-independent host layout: 528000
bytes for the buffer, plus the bounded drain batch and observer state. Actual
target storage and runtime headroom must be measured separately.

Turning Motion off, leaving Mouse mode, or filling the record buffer finishes
the capture. Storage exhaustion does not disable Motion or alter its output.
The buffer is never overwritten during capture or while awaiting export.
After recording ends, while Motion is off, the coordinator attempts at most
one line of up to 320 bytes per loop through the existing diagnostic console.
Re-enabling Motion pauses export and does not overwrite the completed capture.
After the footer output attempt, the buffer may be reused on another rising
edge. Console writes are not acknowledgements: a host must independently check
the complete record. Missing output requires a new capture, not an assumption
that the device delivered it.

No per-sample text formatting, logging or allocation runs while recording.
The queue and PSRAM copies still consume time; device measurements must compare
read duration, queue loss, observation age, heap and stack with capture inactive
and active. Deferred export must also leave normal input and release handling
responsive. This design does not claim zero timing perturbation.

## Evidence format

`tools/motion_capture.py` reads the existing USB diagnostic interface without
commands, reset, buffer clearing or device writes:

```sh
python3 tools/motion_capture.py --port <device-port> --seconds 180 --output <new-prefix>
python3 tools/motion_capture.py --from-log <saved-log> --output <new-prefix>
```

Use the project-local SDK Python environment for live capture if `pyserial` is
not installed in the default interpreter. Output files are exclusive-created;
existing evidence is never overwritten. The reader caps logs at 4 MiB, four
sessions and 6000 rows per session. A 6000-row export can take roughly a minute
or longer; keep Motion off until the footer has been received.

The version-1 stream contains:

- `IMAGE`: full Git identity/state and the ELF SHA-256 from the app descriptor.
  Match it against the frozen artifact/configuration manifest. Git identity
  alone cannot distinguish diagnostic build settings.
- `BEGIN`: version, session, actual record count, observation start/end times
  in monotonic microseconds, and stop reason (1 requested, 2 capacity).
- `ROW`: session, index, accepted-publication sequence, delivery epoch,
  acquisition-completion timestamp, dequeue observation timestamp, current input
  epoch, six exact float32 bit patterns (gyro XYZ in degrees/s, acceleration XYZ
  in g), context flags, discontinuity bitmask, cumulative dropped accepted
  frames, source error bits and delivery state.
- `END`: session and record count. Indices, identity and the footer are all
  required for wire completeness. Zero rows or a capacity-limited session is
  not a usable complete movement segment.

Both sensors retain the board's right-handed display frame: X right, Y top,
Z outward. The current Mosaico profile configures 4 g and 2000 degrees/s with a
nominal 100 Hz polling worker. Completion timestamps and accepted sequences do
not prove every hardware conversion was captured or a uniform sample cadence.
No missing samples are interpolated, retimestamped or replaced with zero.

Context bits are: 0 frame present; 1 Motion requested; 2 locked; 3 touch held;
4 physical button held; 5 overlay; 6 haptics; 7 model output ready; 8 calibrated;
9-10 input mode; 11-12 input owner; 13 release pending; 14 congestion;
15 dropped-frame counter saturated. These describe observation-time context,
not permission at acquisition time. They must not authorize historical HID
output or reconstruct exact control transition times.

Discontinuity bits and delivery states are defined in
[board_imu_stream.h](../firmware/main/board_imu_stream.h). The dropped counter
combines canceled/lost accepted frames, including intentional fences; it is not
an overflow counter. A marker row has no sensor frame and cannot be replayed as
zero acceleration. Pre-start losses and samples acquired after finish are
outside this capture; first/last row sequence/time describe its actual coverage.
An undrained final tail is not claimed to be captured through the finish time.

The parser reports wire completeness, ordered segment usability, compatibility
with the existing 100 Hz +/-5 percent candidate assumptions, and physical
qualification separately. It preserves raw bits and invalid observations.
`segment_issues` lists contradictions, faults and losses without discarding
their rows. Observation/sample times must fit the capture; observations must
not run backward; frame flags must agree with delivery state. Unknown flags,
new dropped publications, counter rollback or saturation prevent a usable
segment. Initial intentional fence markers may establish a historical dropped
counter baseline before the first frame; later losses remain failures.
Physical qualification always remains false: raw logs cannot prove actual hand
trajectory, pointer direction, stopping behavior or usability.

## Physical reference and regression boundary

Record a stable hold before moving. Use separate, externally marked trials for
fixed-orientation lateral translation, vertical translation, sustained travel,
braking, reversals, twelve-second waving and wrist rotation. User-reported
actions are intent labels, not exact position or velocity ground truth. A
visible ruler and time-aligned external recording provide a stronger reference.
Keep the complete trace and every loss, rather than selecting only good frames.

Host tests cover queue order/loss/fences and adapter isolation, capture ownership
and storage bounds, exact bit preservation, partial/duplicate/missing records,
and coordinator isolation, full-buffer behavior and paused export. Run
`python3 tests/motion_capture/run.py` and
`python3 tests/coordinator/run.py --capture`; both are included in
`python3 tools/check.py`. Target build and stack evidence precede flashing;
hardware timing and actual hand control remain separate gates.
