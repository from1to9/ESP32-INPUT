# Portable interaction model

The coordinator owns this fixed-storage C11 model. It has no board, UI,
Bluetooth, heap or clock dependencies. The [product requirements](../../docs/requirements.md)
and [interaction contract](../../docs/interaction.md) define the nine-cell
keyboard, local confirmation, draft preview and explicit Send.

Both selection stages use a row-major three-column, three-row grid with indices 0..8. The combined Letters page has nine groups of four, covering A-Z and 0-9 exactly once:

```text
ABCD  EFGH  IJKL
MNOP  QRST  UVWX
YZ01  2345  6789
```

Symbols retain all 32 original punctuation characters exactly once, in nine groups:

```text
.,!?  ;:-_  +=/*
()[]  {}@#  $%&
\|<   >"'   `~^
```

Children occupy row-major cells 0..3 for four-character groups, or 0..2 for the last four symbol groups. Remaining cells are empty, disabled and excluded from gyro selection. Group selection resets focus to center cell 4. Child selection resets to the valid cell nearest geometric center, cell 1; equal-distance choices use the lower row-major index. Confirmation, Back, page changes and canonical state boundaries discard the old relative angular origin. Case applies to labels and appended letters. `INTERACTION_DIGITS` remains reserved/rejected because the combined page already contains every digit.

Touch and physical confirmation share the same group/character path, updating only the 64-character printable US ANSI preview. A group confirmation never appends a character; character confirmation never sends it. Motion and dwell never confirm or send, and Space remains an explicit preview action outside these groups.

## Coordinator integration

Copy canonical `input_state_t` and current output readiness through `interaction_sync` each tick. Epoch, mode, owner, link and lock boundaries clear motion intent and pending clicks, reset local selection and mark any open send for settlement. A transient `output_ready=false` only pauses pointer output/admission of a new send: it preserves motion intent, selection, button timing and an existing send. The engine owns epoch invalidation on real readiness loss and prolonged congestion. The coordinator applies physical events and mode/lock/motion decisions to the HID engine; this model does not mutate the engine or emit reports.

`interaction_send_begin` freezes the preview and returns a unique token for one explicit request. The coordinator copies its preview, submits HID batches of at most 32 characters, and passes cumulative accepted key-down counts to `interaction_send_update`. For a second batch, add the accepted first-batch prefix to its local count. Rejection settles with count zero; completion or cancellation settles with the final cumulative count and `active=false`. After a boundary, the final count may still be applied to the old token, but it cannot resume that send. Until settlement, editing and another send are blocked. Exactly the accepted prefix is removed; the unsent suffix remains. No Enter, implicit send or automatic replay occurs. Progress remains visible after settlement until successful append/delete/clear changes the draft. Transport acceptance does not establish target delivery or focus.

Feed debounced physical levels to `interaction_button_update` and poll `interaction_tick`. DOWN freezes pointer output immediately. The first release waits 280 ms; a second DOWN strictly before that deadline suppresses single-click and produces DOUBLE on release. At the exact deadline the first single-click is final. A hold fires once at or after 900 ms, suppressing clicks; continuous holding across a lock transition cannot fire another hold. Events are classified here; only the coordinator interprets trusted HOLD as lock/unlock. `interaction_consume_press` suppresses the local-takeover click while retaining the ability to hold and lock. A backward clock drops pending gestures and motion intent.

Keyboard gyro focus remains fixed from physical DOWN through pending click classification, including the interval between the 150 ms release freeze and the 280 ms single-click deadline. Suppressed samples discard filter/selection carry, so a single-click confirms the selected group/character and motion then resumes from the reset origin. Double-click and hold classification do not confirm a character. Mouse motion keeps its existing 150 ms release freeze.

For asynchronous touch transitions, pass the actual transition timestamp plus 150 ms to `interaction_freeze_until`. This preserves suppression for a tap completed between coordinator polls, without replacing the current touch level or observing a synthetic future clock. Only a later deadline extends suppression and clears residual motion; repeated/older deliveries are idempotent and do not cancel motion intent.

## Touchpad taps and held drag

The fixed-storage touch recognizer observes raw logical coordinates through `interaction_touch_input`; it does not submit HID. Begin/End/Contact carry absolute points, Move carries unscaled relative displacement. Pass original ordered callback milliseconds separately from current coordinator milliseconds and the authoritative UI revision. Queued events retain their press duration/double-tap spacing; actually future or expired events are rejected. Raw excursion is measured before fine gain, and returning to the starting point cannot restore a tap after exceeding slop.

A qualifying first release proposes one atomic left click. A second eligible contact within the double window/distance proposes left down immediately: a quick lift proposes left up and completes the second click; held movement drags until lift. No third click, delayed first click, indefinite latched gesture or automatic replay is generated. `allow_motion` lets the coordinator call the existing fractional fine-motion path once, after required down admission. Movement-only mode and `tap_allowed=false` retain movement for an existing explicit drag while suppressing pad-generated clicks/holds. Scrolling/control surfaces never impersonate a pad tap; explicit-button ownership remains with the coordinator.

Every proposed effect has a unique token. Immediately call `interaction_touch_admit` with actual engine admission, never guessed host delivery. A refused click/down is consumed; a refused up returns latched `RELEASE_REQUIRED`. Cancellation, context/owner/mode/epoch/lock/link change, bad clocks, lost contact, profile change or duration expiry likewise retain cleanup until the coordinator acknowledges ownership of its neutral-release barrier. Clearing recognizer state is not a successful release. The existing HID engine reserves hold-release capacity and preserves its release barrier after transport failure.

Defaults and validated bounds are declared in [the header](include/interaction.h): tap enabled, maximum press 250 ms (50..500), second-contact window 280 ms (100..500), raw slop 8 px (2..24), double distance 16 px (at least tap slop and at most 48), contact deadman 1000 ms (100..2000) and maximum held drag 30000 ms (1000..60000). Configuration is one atomic validated profile; identical profiles are no-ops, changed profiles consume the current contact and cancel any touch-owned hold. Only the enable preference is currently persisted in device settings; timing/geometry configurability is portable runtime behavior, not an implemented settings/API control.

Call `interaction_touch_tick` after ordered queued events. A real stationary 0/0 Move heartbeat refreshes contact without moving the pointer; cached polling must not manufacture that evidence. The UI adapter must supply fresh physical contact, currently at most 50 ms apart, and stop after release. Total drag duration remains bounded despite heartbeats. Whole-gesture wake and first local takeover are routed/consumed before recognition; a remote/virtual caller cannot mint local presence. Host double-click behavior and timing/geometry tuning still require actual device/host qualification.

## Motion and touch

Board input is already axis-mapped angular velocity in degrees/second and acceleration in g, with monotonic microsecond timestamps. Calibration requires at least 64 consecutive samples spanning 500 ms, acceleration norm within 0.08 g of one, per-axis acceleration change within 0.03 g of the window's first sample, raw gyro no greater than 5 dps, and gyro variance no greater than 0.04 dps squared. It uses online statistics, not stored sample history. Reject nonfinite values, gyro outside +/-2000 dps, acceleration outside +/-16 g, repeated/backward timestamps, and intervals over 50 ms. Invalid/gapped intervals clear residual motion and restart an incomplete calibration window. Users must hold still; a gravity-only stationary check cannot distinguish every constant yaw rotation from bias.

Mouse Motion uses gyro-propagated attitude and the aiming direction set during
sensor calibration. Motion toggles and touch
suppression preserve the basis and continuous attitude. Their output
boundaries clear filter and carry and discard the first eligible interval.
Keyboard selection uses its existing signed local angular axes and never
emits host pointer movement. Audio emits no movement.

Recenter captures the current aiming direction with a fresh, valid attitude
and preserves bias, settings, epoch, sample history and Motion intent.
The local settings action works with Motion off. Remote
`input.motion.recenter` uses the same owner path under an active device-IMU
input-control lease, and duplicate requests retain their original result.
Bias calibration disables Motion and measures new bias and direction.

The existing `imu.state` (`/api/v1/imu`) response adds `motion_requested`,
`input_ready`, `output_ready` booleans and `reference_state` with values
`unknown`, `unavailable`, `acquiring`, `unstable` or `ready`. The coordinator
uses one shared derivation for UI and this encoder: unknown before its model
clock has been observed, unavailable without actual IMU capability, acquiring
during calibration or attitude recovery, otherwise ready for fresh attitude.
The existing unstable value remains available to older API consumers.
Calibration fields remain separate. Reference readiness does not imply route
readiness, absence of touch/freeze suppression, or host delivery. Input/output
readiness retain their existing route/model meanings. Too-small responses fail
with `CP_TOO_LARGE`, zero length and an empty body when capacity permits.

Keyboard angular position spans all three rows and columns. Selection compares only available cell centers and retains the current cell on ties. A candidate must become closer by the configured angular hysteresis along the line between centers before focus switches. This applies to horizontal, vertical and diagonal boundaries, including gaps in sparse child grids. Empty cells have no selection region, and a saturated angular position cannot trap focus on an unavailable cell. Calibration and all motion tuning still require physical measurement.

Touch movement accepts finite relative displacements in +/-4096 HID units per call, with gain divisors 1, 4 or 8, fractional carry and output bounded to +/-127. It requires active touch, Mouse mode, a current local owner and output readiness. Invalid/suppressed intervals and gain changes discard residual motion. HID units are not screen pixels. Calibration, axis orientation, default gain, filtering, freeze and selector thresholds still require actual-board tuning; host regressions do not establish physical accuracy.

The [aiming reference](../../docs/motion-estimation.md) describes the
projection, conditional gravity correction, delayed-sample continuity and
recovery after a real sample gap. The model uses the existing right-handed display body
frame and the original 50 ms sample interval limit. Keyboard selection keeps
its immutable signed axes and local grid geometry.

Configuration accepts independent X/Y gains 0.25..100 HID units/degree, deadzone 0..10 dps, filter time constant 0..1 seconds, settling freeze 0..1000 ms, selector cell width 1..45 degrees and hysteresis from zero up to half a cell (exclusive). Defaults are X=Y=4, no inversion, deadzone 0.5 dps, filter 0.03 seconds, freeze 150 ms, cell width 6 degrees and hysteresis 1 degree. `interaction_freeze_after` applies the configured duration to original event time with saturating arithmetic; `interaction_freeze_until` still accepts an explicit later deadline. Configuration changes clear filters and carry. Status flags distinguish calibrated, gravity ready, gyro-predicted and suppressed output, with a reason for suppression. Calls and snapshots must remain on the coordinator; the model is not thread-safe.

For Mouse these gains and the filter time constant are base values. The
[aim response](../../docs/motion-estimation.md#current-orientation-design)
scales them with projected turn speed. The selector uses its configured
filter directly.

### Optional reference-loss evidence

`interaction_reference_diagnostics_bind` attaches fixed caller-owned storage;
the model only gains one pointer. Storage and model must share a coordinator
and lifetime. A copied preview model cannot write or read the original binding.
Init detaches, bind/reset clears observations only, and the getter returns NULL
for absent or copied bindings. No allocation, sensor access, logging or extra
clock observation occurs in the sample path. Trust calculations, short-circuit
evaluation and admission remain unchanged.

Loss counters count actual ready-to-unavailable transitions by call-site reason;
repeated samples while unavailable are not new losses. All counters saturate
with explicit flags. The first active incident is immutable until diagnostic
reset/rebind: Mouse motion was enabled, its source owned input, output was ready
and control was unlocked at the transition. Touch/haptic/button/freeze context
is retained, not silently excluded. Explicit calibration/cancel disables intent
before invalidation and therefore cannot occupy this active incident slot.
Inactive losses remain counted without occupying it.
Recenter records a saturating count and the last reset reason and model
timestamp independently of loss and rejection evidence. Motion toggles retain
the calibrated reference. The enable-reset counter remains for older captures.

An episode starts at the first rejected correction while reference is ready and
ends on a trusted correction, reacquisition or invalidation. Its near-one-g,
gentle and innovation rejection counts can overlap. Innovation remains unknown
when the existing near-one-g/gentle short circuit did not evaluate it; haptic
prediction does not evaluate correction checks. A hard norm rejection records
only that safe-norm check. The incident retains the episode's first rejection
and terminal frame, including original sample pair/timestamps, actual previous
acceleration, bias, gravity before prediction, dt, norm, last trusted timestamp,
control context and known/pass masks. Binding midstream cannot invent a previous
sample; previous-known stays false until a matching sample has been observed.
Non-sample losses have sample-known false and do not reuse an old work frame.

The coordinator emits one `MotionRef` row at the existing ten-second cadence,
plus at most two `MotionRefFrame` rows for the frozen incident. Rows stay below
1024 payload bytes; repeating the immutable incident allows late attachment.
`losses` lists reason indices 1..12 from the header enum (clock, calibrate,
cancel, missing, invalid sample, sample time, haptic age, haptic normalization,
hard acceleration norm, prediction normalization, correction normalization,
ordinary trust age). The two age reasons describe older captures. Frame
vectors are comma-separated exact IEEE binary32
hex bits, in gyro X/Y/Z then acceleration X/Y/Z order for samples, and X/Y/Z
for bias/gravity/previous acceleration. Unknown fields must be interpreted via
their known flags, never as measured zero. These observations identify executed
rejections and loss branches; they do not alone establish physical sensor phase,
mounting, translation versus rotation, or host pointer delivery.

## Focused host regression

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I components/input_core/include -I components/interaction/include \
  components/input_core/input_core.c components/interaction/interaction.c \
  tests/test_interaction.c -lm -o /tmp/esp32-interaction-test
/tmp/esp32-interaction-test
```

## Fresh multi-contact snapshots

The separate `interaction_multitouch.h` recognizer consumes at most two contacts
from one existing hardware read, with stable IDs, increasing sequence, original
monotonic time, epoch/UI revision and explicit support/fresh/valid/overflow
flags. It never reads the device or manufactures evidence from cached LVGL
state. Feed it before the primary-pointer event and obey consume/cancel/release
results. Capability plus the gesture preference gates use; movement-only passes
`enabled=false`. Two fingers cancel single-finger recognition and consume the
pointer. A short still pair yields one atomic right-click; centroid movement
commits bounded vertical wheel output. A remaining finger is consumed until
all-up, never converted into a pointer jump or tap. Rejected effects are not
replayed; held cleanup remains latched until the coordinator owns its neutral
release. See [focused regressions](../../tests/multitouch/README.md).

## Authorized input sources

The existing native edit, send-begin, touch, motion-enable and motion-sample
entry points remain local-source wrappers. Their `*_owned` counterparts take
an explicit `input_owner_t` and compare it with actual model ownership before
editing, enabling or producing input through the same private implementation.
Explicit motion disable remains a safety operation. They do not grant a lease, impersonate
physical activity or temporarily change ownership. The coordinator must first
validate the remote session, lease, producer, epoch, target and visible context.
`interaction_edit_owned` atomically validates append/delete capacity and selects
the same grid/group/character path used by native input. Motion source intent
is bound explicitly and cleared at owner/epoch boundaries. Virtual touch has
its own source-bound recognizer state and does not assert physical touch-down.

`interaction_calibration_cancel` stops accumulation, invalidates bias/reference
and keeps motion off. Late samples cannot finish that calibration; a new
explicit `interaction_calibrate` call is required. The coordinator checks that
the cancellation belongs to its active calibration job before calling it.

A source-bound virtual pad contact also clutches owned motion while gravity
tracking continues. This is an input suppression state, not physical presence;
it does not renew idle, wake, consent or a remote lease.
