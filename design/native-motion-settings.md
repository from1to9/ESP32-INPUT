# Native Motion settings integration

Status: UI implementation and coordinator boundary, 2026-10-21. Input projection,
gesture recognition, persistent settings and physical qualification remain
owned by their production components. This UI does not infer sensor readiness.

## Interface

[app_ui.h](../firmware/main/app_ui.h) preserves the original 54 action values
and appends `APP_MOTION_CONFIG`. Its `motion_patch` field contains a typed field,
expected settings revision and a value with explicit units. The coordinator
validates current authority, page, revision and bounds before applying exactly
one field. It rejects malformed/stale patches rather than treating `x/y` as an
alternate settings payload. Queue acceptance is not persistence success.

`app_view_t.motion_settings` contains available/editable flags, the actual
settings revision, both axis gains, deadzone, filter/freeze time, inversion and
typed automatic-reference status. `available=false` or invalid numeric values
disables the edits and displays unavailable values; zeros are not substituted
with defaults. Editing admission is independent of HID connectivity. The
existing settings-save state continues to report actual persistence results.

The added settings pages are `APP_SETTINGS_MOTION`,
`APP_SETTINGS_MOTION_ADVANCED` and `APP_SETTINGS_GESTURES` (after Wi-Fi). Parent
integration must extend page-range validation and action guards:

- Motion and tuning accept `APP_MOTION_CONFIG` only for their displayed fields.
  Motion Back returns to Input; tuning Back returns to Motion.
- Gesture help accepts the existing `APP_TAP_TO_CLICK` and safety `APP_RELEASE`.
  It returns to Input. Input retains the original tap setting and calibration.
- Navigation remains a coordinator acknowledgement, not UI-local page state.
  Snapshot fields must be copied by any presenter snapshot construction.

The UI captures the settings revision at press, alongside epoch/UI revision,
and rejects release after either identity changes. Slider release, presets,
nudges and inversion all use the same typed mutation. The UI changes no setting
optimistically and never sends a whole stale settings structure.

## Product behavior

Input settings links to Motion sensitivity and Gesture help. Horizontal and
vertical sensitivity each offer Slow (2.00), Default (4.00), Fast (8.00), a full
0.25..100.00 slider and 0.25 nudges. Values represent the actual shared setting
in HID units per degree. Adjusting one axis cannot change the other. Boundary
nudges stop at the nearest valid endpoint; unavailable controls submit nothing.

Motion tuning exposes deadzone (0..10.000 degrees/second, 0.100 nudges), smoothing
(0..1000 ms, 10 ms nudges), touch-release pause (0..1000 ms, 25 ms nudges), and
separate reversible horizontal/vertical inversion. These are the settings-v4
bounds. Automatic orientation has no manual posture selector: the owner reports
unknown, unavailable, acquiring, ready or unstable reference state.

Gesture help describes tap, double tap and double-tap hold/drag only when the
shared tap recognizer is available and enabled. Movement-only remains explicit;
a previously saved false value must be respected. Show actual dragging/release
state and an always-visible, conditionally enabled Cancel drag action. Touch
priority and post-release Motion freeze remain input-owner behavior.

Two-finger or swipe-scrolling help appears only with the matching implemented
capability flag. Those flags do not claim device/physical qualification. Until
the actual coordinator and sensor path support the gesture, direct users to
the existing Scroll controls. UI help never implements a second recognizer.

## Validation

Use native 480 x 480 checks for both themes, known/unavailable/invalid values,
slider/preset/nudge typed payloads, isolated axes, minimum/maximum values,
inversion, stale press/release, disabled controls, navigation, gesture capability
help, resource cycles and no idle redraw. The existing native UI regressions
remain required. Input/settings tests establish actual configuration semantics;
native UI fixtures alone do not establish Motion projection or touch hardware.

## Single-reader contact boundary

`app_ui_touch_context()` is read-only and called under the existing LVGL lock,
before normal primary-pointer events. It returns the visible snapshot's epoch
and UI revision, plus `eligible_pad` and inclusive logical pad bounds. Only a
local, unlocked, awake Mouse primary page with valid visible geometry qualifies;
a covering/deferred panel, other mode, remote owner or teardown does not. It
performs no layout update, allocation, event submission or hardware access.

Use a coordinator queue envelope with a tagged union of the existing
`app_intent_t` and the input owner's `interaction_contact_snapshot_t`. This keeps
the domain multi-contact type out of the UI header and avoids adding a second
UI action namespace for raw contact data. The adapter reads the controller once,
queues the complete contact snapshot before its normal primary-pointer intents
in the same FIFO, and passes the primary point to LVGL. The callback owns real
freshness/sequence/time; cached LVGL PRESSING never becomes new multi-contact
evidence. The coordinator revalidates epoch/revision, bounds for every contact,
wake capture, release, local ownership and the saved gesture preference.

The context query is presentation evidence only, not HID/capture permission or
proof of a qualified multi-touch device. Native tests cover valid bounds,
mode/overlay/lock/sleep/remote rejection, deferred covering panels, teardown and
allocation/flush-free repeated reads.
