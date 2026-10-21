# Multi-contact interaction regressions

Run `python3 tests/multitouch/run.py`. The strict C11 sanitizer build executes the
production recognizer with complete contact snapshots, independent of LVGL or
touch hardware. Cases cover both release orders, reordered stable IDs,
fractional scrolling, clamping without replay, pair-to-one draining, tap motion
excursion, join/release deadlines, malformed/old/missing evidence, capability,
movement-only preference, held-button cleanup, clock regression and sequence
exhaustion. They establish portable behavior, not fresh hardware sampling or
HID delivery. The parent integration owns the single reader and effect admission.
