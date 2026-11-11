# Mouse Motion aiming

The current Mouse interaction is rotational aiming. Hold the desired aiming
direction during sensor calibration, then rotate the device. Both USB and
Bluetooth use the existing relative HID reports. This design operates on the
device and does not measure absolute screen position or physical translation.

## Current orientation design

The production interaction model applies the measured gyro bias and advances a
unit quaternion using ordered body-frame angular increments and actual sample
intervals. The display body frame is right-handed with X right, Y toward the
top and Z outward. At calibration, the reference frame coincides with this
body frame and records measured world-up. Horizontal aim is rotation about
that up axis. Vertical aim uses the calibrated display-right axis projected
onto the plane perpendicular to up. If display-right is nearly vertical when
captured, the projected display-top axis supplies a well-conditioned
alternative. Manual Recenter uses the current pose to replace the vertical
axis while retaining world-up, sensor bias, settings and Motion intent.

Each cursor delta uses the gyro rotation vector expressed in the reference
frame by the starting quaternion. This is the local angular increment, so a
pose passing 180 degrees has no angle-wrap jump. Gravity correction updates
the attitude estimate without contributing movement to the current report.
Positive rotation about the calibrated pitch axis raises the aiming direction.
Relative HID Y increases downward, so this rotation emits negative Y by
default. The vertical inversion setting reverses that mapping.

The saved independent gains are the fine-motion base in HID units per degree.
For Mouse, the response multiplier is `1 + min(speed / 20, 3)`, where speed is
the larger absolute projected axis rate in degrees per second. Each base gain
is multiplied up to the existing 100 HID units per degree ceiling. The
configured filter time constant is divided by the same multiplier during a
turn. A value of zero bypasses the filter. Each axis clears filter and
fractional carry at the deadzone or a reversal. The existing single HID
report bounds each axis to 127 units and drops any excess from that interval.

Rotation around the third calibrated axis contributes no direct
movement. The fixed mapping follows the calibrated axes after a change in
grip. Use Recenter to adopt a different held direction.

Near-one-g magnitude, gyro-compensated sample change and angular innovation
gate gravity correction. Continuous valid gyro samples support propagation
while those gates reject acceleration or while haptics suppress output. This
removes the prior 250 ms correction-age cutoff for continuous gyro. Real
freefall or impact outside haptics, nonfinite or saturated samples, missing
samples and out-of-order or over-50-ms intervals invalidate attitude. Recovery
requires at least eight stable samples spanning 100 ms. It aligns tilt with
gravity while retaining the last heading estimate and the calibrated axes.
Heading change about gravity during an unobserved interval remains unknown.
Recenter can restore the user's chosen directional mapping. Continuous
six-axis tracking can drift in heading.

Ordinary output pauses preserve the basis and valid attitude. The first
eligible interval establishes a fresh output boundary and emits no movement.
Queued non-IMU input retains its HID turn rather than delaying an IMU delta
behind it. No device or Windows pointer response is implied by native checks
or a target build.

[AirPointer at dc9f277](https://github.com/musubipapi/airpointer/tree/dc9f277f71ae7af3d68bceb1952c2a8b3602bbcb)
informed the quaternion propagation, conditional gravity correction and
calibration approach. Its MIT-licensed source separates gyro attitude and grip
calibration in `Orientation.js`, with host-side screen output in
`ClosedCaseSession.swift`. The ESP32 implementation uses its own C math,
existing sensor units and calibrated axes, and the relative HID path.
