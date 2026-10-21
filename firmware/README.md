# Mosaico BSP bring-up scaffold

The pinned official BSP supplies board revision detection, power, IMU, display and touch. This app initializes those services and displays an explicit bring-up label. It does **not** implement the product LVGL UI, pairing, HID, audio or ASR.

Use ESP-IDF >= 6.2 with `esp32s31` support. Run `idf.py --preview set-target esp32s31` and `idf.py build` from this directory, after initializing the submodule. Component paths are relative to `main/idf_component.yml`. The SDK itself and resolved component versions are not yet pinned by a successful build; the first hardware bring-up must record them and commit `dependencies.lock`.

Unknown board eFuse aborts before initialization. There are no board pin overrides. Do not flash until board revision, data port, factory firmware backup and recovery steps have been checked. Build success alone is not a compatibility result.
