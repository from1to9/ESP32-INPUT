# Local reset storage tests

Run `python3 tests/factory_reset/run.py`. Strict C11 and address/undefined
sanitizers compile the production storage owner against NVS and partition
boundary substitutes. No device or real user storage is opened.

The suite covers normal unarmed boot (including an absent optional control
partition), legacy-only blob namespaces hidden by the SDK iterator, missing
user namespaces without creating them, corrupt/unreadable markers, storage-init
errors, mismatched partition ownership, retained PHY calibration and OTA floor,
all current user stores, marker admission, and retry after each clear/commit/
erase boundary fails either before or after applying. The marker can disappear
only after every user store is cleared. These checks model NVS entry atomicity;
they do not qualify physical flash behavior or power-cut endurance.

`tests/coordinator/run.py` covers the production local confirmation, expiry,
stale/locked input, actual input release and audio-off, update exclusion and
failure/timeout navigation. `tests/device_restart/run.py` checks dispatch and
marker-write failure. `tests/ui/run.py --suite about` renders both themes and
exercises the real LVGL confirmation. Resource and target stack gates include
the new panel, boot owner and existing restart worker.
