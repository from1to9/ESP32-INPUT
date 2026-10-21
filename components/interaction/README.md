# Portable interaction model

The coordinator owns this fixed-storage C11 model. It has no board, UI, Bluetooth, heap or clock dependencies. The [product requirements](../../docs/requirements.md), [interaction design](../../docs/interaction.md) and [original prototype](../../design/app.js) define the preview/confirmation/send flow. The current nine-cell layout follows the user's subsequent 3x3 keyboard decision; the original prototype is not rewritten.

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

## Motion and touch

Board input is already axis-mapped angular velocity in degrees/second and acceleration in g, with monotonic microsecond timestamps. Calibration requires at least 64 consecutive samples spanning 500 ms, acceleration norm within 0.08 g of one, per-axis acceleration change within 0.03 g of the window's first sample, raw gyro no greater than 5 dps, and gyro variance no greater than 0.04 dps squared. It uses online statistics, not stored sample history. Reject nonfinite values, gyro outside +/-2000 dps, acceleration outside +/-16 g, repeated/backward timestamps, and intervals over 50 ms. Invalid/gapped intervals clear residual motion and restart an incomplete calibration window. Users must hold still; a gravity-only stationary check cannot distinguish every constant yaw rotation from bias.

Calibrated rates pass through a deadzone and a first-order filter. The deadzone clears a stopped axis immediately, preventing filter tails from moving a stationary pointer. Mouse integration uses production `input_motion_step` with actual sample intervals and bounded fractional carry. Keyboard Motion integrates local angular focus with configurable boundary hysteresis and returns zero host movement. Audio always returns zero. Touch, button down, menus and haptics suppress gyro output; touch release has a 150 ms freeze. Touch/haptic suppression preserves prior mouse intent; menus and canonical state boundaries clear it. Returning to Mouse requires explicit enabling.

Keyboard angular position spans all three rows and columns. Selection compares only available cell centers and retains the current cell on ties. A candidate must become closer by the configured angular hysteresis along the line between centers before focus switches. This applies to horizontal, vertical and diagonal boundaries, including gaps in sparse child grids. Empty cells have no selection region, and a saturated angular position cannot trap focus on an unavailable cell. Calibration and all motion tuning still require physical measurement.

Touch movement accepts finite relative displacements in +/-4096 HID units per call, with gain divisors 1, 4 or 8, fractional carry and output bounded to +/-127. It requires active touch, Mouse mode, a current local owner and output readiness. Invalid/suppressed intervals and gain changes discard residual motion. HID units are not screen pixels. Calibration, axis orientation, default gain, filtering, freeze and selector thresholds still require actual-board tuning; host regressions do not establish physical accuracy.

Configuration accepts pointer gain 0..100 HID units/degree, deadzone 0..10 dps, filter time constant 0..1 seconds, selector cell width 1..45 degrees and hysteresis from zero up to half a cell (exclusive). Defaults are gain 4, deadzone 0.5 dps, filter 0.03 seconds, cell width 6 degrees and hysteresis 1 degree. Calls and snapshots must remain on the coordinator; the model is not thread-safe.

## Focused host regression

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I components/input_core/include -I components/interaction/include \
  components/input_core/input_core.c components/interaction/interaction.c \
  tests/test_interaction.c -lm -o /tmp/esp32-interaction-test
/tmp/esp32-interaction-test
```
