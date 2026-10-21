# Native product UI checks

Run `python3 tests/ui/run.py` from the repository root after resolving the locked firmware dependencies. The runner finds CMake and Ninja in the project toolchain or system PATH. It builds the unchanged production `firmware/main/app_ui.c` against managed LVGL 9.5.0, with Montserrat 14 metadata, 16 body and 22 title fonts and the RGB565 software renderer. Firmware must enable those same font sizes. No browser reconstruction, image mock, hardware connection or device write is involved.

The CMake target enables strict compiler diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer for production UI code and LVGL. LeakSanitizer is enabled where supported; Apple's bundled ASan does not support it. Build output is retained in `.build/ui-host/build.log`.

The [UI layout contract](../../design/ui-plan.md) defines the 480 x 480 grid, colors, settings hierarchy and coordinator boundary. Dark uses a black background, light blue selection and near-white text; Light retains the original green palette. The actual objects are restyled in place when the coordinator acknowledges a theme change.

The pointer driver performs actual LVGL hit testing. Assertions cover:

- Equal three-by-three keyboard column boundaries, button dimensions and text baselines; aligned utility rail; header name/status separation; every visible label/button inside its parent and screen; minimum 44 px button targets.
- Relative touchpad movement, the pad/scroll gap, drag/release, press-time revision capture, modal ownership, physical-only unlock and in-place theme changes during touch.
- Nine-cell group selection, four valid second-stage characters and inert empty cells, explicit Enter versus Send, Stop, 64 wide-character preview/counter separation, long status truncation and unavailable speech/audio controls.
- Busy transport draining between press/release, first-touch control delivery, and active Release/Stop while ordinary input is unavailable.
- Settings page intent admission; all timeout values; brightness endpoints and release-only submission; acknowledged state versus pending actions; defaults, saved, saving and persistence failure.
- Pairing count/capacity uses the provider limit; full or over-capacity and unknown count/capacity disable Add without eviction. Closing an already open pairing window remains possible.
- Pairing open/close/forget, disconnect and Wi-Fi forget confirmation intents. The 32-entry Bluetooth list covers current connection, first/next/last page, back navigation, empty and unavailable states without treating missing evidence as an empty list. Pressing a request cannot directly confirm it or invent an updated connection state.
- Wi-Fi cleanup retry and uncertain/corrupt saved-network messages stay visible, including from Connectivity. Forget remains available when the coordinator reports that cleanup of uncertain storage is useful.
- Protected setup starting/active/ended, bounded synthetic SSID/passcode displayed in full, and credential label clearing on close, expiry and lock. All setup strings are test-only data, not real credentials.
- Input transport selection from all three modes, independent Bluetooth/USB state, pending transitions, unavailable USB, selected-path errors, no optimistic selection or duplicate input, and no inferred audio capability.
- Screen-off black framebuffer, first whole touch consumed on wake, and a subsequent fresh touch delivering its action.

The run produces 164 PNGs in `.agent/local/ui-nine-transport/`, covering both themes for every primary page, Settings category, modal and key availability/failure boundary. Additional scenarios include unknown battery/Wi-Fi signal, charging/full/missing battery, weak signal, Wi-Fi connecting/retrying/error, pairing updating/error, long About identity and full-width haptic availability. The synthetic peer is explicitly test data; production uses the coordinator's real target identity and measurements.

Inspect the generated PNGs after changes. Passing assertions cannot establish visual quality, physical display readability, sensor accuracy, radio compatibility or device wake timing. These require separate integrated target/device checks. Previous evidence in `.agent/local/ui-visual/` and `.agent/local/ui-refresh/` is from earlier layouts and must not be used to validate the nine-cell/transport refinement.
