# Interaction prototypes

The current [focused preview](compact.html) implements the authorized core-workspace layout in the [UI plan](ui-plan.md): larger Mouse controls, a real nine-cell Keyboard, truthful unavailable Audio, and one Menu/AI-double-click quick panel. It uses a 480 x 480 logical canvas and synthetic fixtures. It has no external assets or dependencies and never accesses Bluetooth, a microphone, an account or a target device.

The preview's right-hand controls simulate physical AI gestures, theme, wake, focus and connection/ownership states. Keyboard input remains in a bounded preview until explicit Send; Enter is a separate shortcut. Audio and spoken recognition remain unavailable. The recording fixture demonstrates an indicator and local Stop only, and prevents screen blanking while active. Browser behavior is not evidence of firmware timing or hardware capability.

The [earlier prototype](index.html) preserves workflow history, including its previous fixed navigation and explicitly simulated audio/speech. Its layout has been superseded by the focused UI plan. Both previews use the current dark palette; Light retains the original green palette.

Actual firmware uses the production LVGL UI. Native 480 x 480 screenshots, pointer tests and FULL-render allocation/flush checks are described in [tests/ui](../tests/ui/README.md). Browser inspection, native rendering and physical-device qualification are separate evidence levels; neither prototype can establish the latter two.
