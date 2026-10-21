# Interaction prototype

Open `index.html` directly, or serve the repository with Python's local HTTP server. This prototype has no external assets or dependencies and does not access Bluetooth, microphone, accounts, or any target device.

Three foreground modes share one state. The right-hand controls simulate physical button gestures, motion, disconnect/reconnect and remote ownership. Each page exposes intended UI decisions. The audio coexistence checkbox is a **design simulation**, not a capability claim or default product setting.

Keyboard motion selects a group and then a character using the simulated AI click. Touch can select the same grid. Spoken letters use a clearly labeled sample selector; start, stop, confirm, then send. Sending removes the simulated accepted prefix from the draft; interruption preserves only unsent characters. Enter is a separate explicit shortcut.

This is not the firmware UI: LVGL widgets, real touch gesture recognition, IMU calibration, button timing and BLE transport integration remain future work. The screen uses a 480 × 480 logical layout; narrow browser previews reflow within that height and are not a scale-accurate hardware model.
