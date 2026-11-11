# Mouse Motion aiming

Hold the desired aiming direction during sensor calibration, then rotate the
device. USB and Bluetooth use relative HID reports. The model runs on the
device without a screen calibration step.

The implementation includes the pointing ray and sample history from
`959aabd` and the horizontal sign correction in `d8e0701`. See
[validation](validation.md) for native results and device observations.

## Current orientation design

The model subtracts the measured gyro bias and propagates a unit quaternion
with ordered body-frame increments and actual sample intervals. The display
frame is right-handed, with X right, Y toward the top and Z outward.
Calibration and Recenter capture a horizontal pointing ray expressed in that
body frame. Calibration measures bias. Recenter retains bias and uses the
current valid pose to capture a new ray.

For each live sample, the starting attitude expresses world-up in the body
frame. The current up direction and the calibrated ray define perpendicular
right and up tangents. The model projects gyro motion onto these tangents
for pitch and yaw. Roll about the ray contributes no direct pointer motion.
Both axes ease toward zero as the ray approaches vertical and azimuth becomes
ambiguous. By default, moving the ray right emits positive HID X and raising
it emits negative HID Y. Independent inversion settings reverse either axis.

The saved base gains are in HID units per degree. The response multiplier is
`1 + min(speed / 120, 1)`, where speed is the larger absolute projected axis
rate in degrees per second. The model applies this multiplier to each base
gain, capped at 100 HID units per degree, and divides the configured filter
time constant by the same multiplier. Zero disables the filter. Deadzone and
reversal clear the affected filter and fractional carry. Each relative HID
report remains bounded to 127 units per axis.

## Sample continuity and recovery

The sensor owner retains 20 ordered frames in the current source generation.
When a delayed Mouse coordinator read would otherwise span more than 50 ms,
it can copy up to 18 intermediate frames between retained endpoints from that
generation. The latest sample must still be fresh within 50 ms, and each
observed sample interval must satisfy the 50 ms limit. At a steady 100 Hz,
the full window spans 190 ms.

Historical frames advance an established attitude using validated gyro
samples. Acceleration sanity checks still apply. These frames do not correct
gravity, calibrate, move keyboard focus or emit historical HID movement.
The first eligible live interval reestablishes the output boundary without
replaying motion. The history window cannot recover samples from an
overwritten endpoint or an unavailable or changed source.

Live gravity correction checks acceleration magnitude, gyro-compensated
change and angular innovation. Gyro propagation can continue while these
gates reject correction or haptics suppress live output. Genuine gaps,
out-of-order or invalid samples, and impact or freefall outside haptics
invalidate attitude. Recovery needs at least eight stable samples over
100 ms. It realigns tilt with gravity while retaining the last heading
estimate. A turn about gravity during an unobserved interval remains unknown,
and continuous six-axis tracking can drift in heading. Use Recenter to
capture the desired direction again.

Ordinary Motion toggles and touch pauses preserve the valid reference. Resume
clears filter and fractional carry before fresh output. Queued non-IMU input
retains its HID turn.

## References

[AirPointer](https://github.com/musubipapi/airpointer/tree/dc9f277f71ae7af3d68bceb1952c2a8b3602bbcb)
informed timestamped gyro propagation, conditional gravity correction and
calibration. [AimMac](https://github.com/Coolerwave/AimMac/tree/4da2f7c89e33e35610bb2b4ebe807d82038ab760)
and [WatchPointer](https://github.com/umarz317/WatchPointer/tree/504137a1a8748e3560f1c203d06fbd4e0832d582)
provided references for timestamped tracking and speed-dependent smoothing.
ESP32-INPUT uses its own C implementation and relative HID output. It
incorporates no upstream code or assets from these projects.
