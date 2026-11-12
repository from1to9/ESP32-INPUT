# User guide

These instructions apply to the ESP-Mosaico development firmware. Check the
[validation status](validation.md) for open device and host issues. For a board
running the factory image, start with the [build and recovery guide](../firmware/README.md).

## Connect a mouse and keyboard host

Open **Menu → Settings** and choose the input transport. The transport applies
to both Mouse and Keyboard.

- **USB.** Connect the board's main USB-C port directly to the host with a data
  cable. Select USB input and wait for the device to report input ready.
- **Bluetooth.** Open **Bluetooth devices → Mouse & Keyboard → Add device**.
  Wait for the pairing window to open and select the displayed input name
  ending in `-IN` in the host's Bluetooth settings. Confirm readiness on the
  board after pairing. Reconnect saved hosts from the same device list.

On iPhone or iPad, use the host's accessibility pointer settings as needed.
For text, use a US ANSI keyboard layout. First test with a disposable text
editor and an unlocked host. A saved pairing and a ready HID link are separate
states. Switching transports cancels pending input. Disconnecting a cable does
not redirect movement or text to Bluetooth.

## Mouse

Move one finger on the pad for relative motion. The separate scroll column
accepts a slide or arrow taps. Use the visible click and drag controls, or the
pad's tap gestures when **Tap to click** is enabled. A pad contact keeps its
pointer action until lift even when it crosses the visible pad boundary.

For Motion, hold the board still in the direction you want to aim and use
**Calibrate sensor** in Input settings. Enable **Motion** and turn the board.
Touch temporarily pauses gyro output. Use **Recenter** to adopt a new direction
or **Calibrate sensor** to measure bias again. A **Waiting** state can indicate
missing or invalid sensor data. See [aiming and recovery](motion-estimation.md).

## Keyboard

Choose **Keyboard** in Menu. The first workspace is **Touch**. Choose a group,
then a character, and check the draft before pressing **Send**. Send never
appends Enter. **Keys** contains actions that go directly to the ready host
without changing the draft. Select Ctrl or Command shortcuts for your host.

The chooser also offers local motion selection, spoken letters and shortcuts.
Keyboard motion moves the selection highlight. Spoken letters produce a
candidate that you confirm into the draft. General dictation is outside the
current scope.

## Audio

Open **Audio** and choose USB or Bluetooth. Selecting a route leaves both jobs
stopped. For Bluetooth pairing, use **Manage devices → Audio → Add device**
and select the displayed name ending in `-AUD` from the host. Input and audio
have separate saved records and readiness indicators. If a saved audio
connection fails, follow the board's pairing prompt. The failure and its
workaround are tracked in [validation](validation.md).

For **Mic**, choose the input endpoint on the host, open a capture stream and
start Mic on the board. Check the microphone indicator and use Mute or Stop
when finished. For **Speaker**, route the host's playback to the device and
start Speaker on the board. A Bluetooth speaker job can wait for the host's
stream. Verify the physical audio at the endpoint you selected.

Speaker playback can continue after returning to **Mouse**, where touchpad and
Motion use the selected input transport. Bluetooth input and audio require
both separate links to be ready. A brief combined Windows test has passed;
see [validation](validation.md) for its limits and remaining issues.

Saved workspace and audio choices can be restored at startup. A previously
enabled Bluetooth Speaker can wait for fresh audio from its saved host;
the microphone always requires a new start. **Stop speaker** cancels the
waiting or active speaker intent. See [startup behavior](bt-audio.md#startup-choices-and-trusted-audio-reconnection)
for reconnect and storage-failure details.

## Reset saved user data

Open **Menu → Settings → About → Reset device** and read the consequence
summary. **Reset and restart** requires a second fresh touch within 30 seconds;
Cancel leaves the data intact. The device stops input and audio, restarts and
clears its saved pairings, Wi-Fi profiles, user settings, LAN controller access
credentials and recordings. A visible failure is not a completed reset.

Firmware, OTA security state, hardware identity, factory calibration and shipped
assets are retained. Remove the old Bluetooth entries on the computer before
pairing again. Wi-Fi and LAN access must also be set up again. This local reset
has a broader scope than the remote factory-reset command; see the
[reset contract](interaction.md#status-connectivity-and-display) for storage and recovery details.

## Local controls and LAN access

Double-press the AI button to open Menu. Hold it for about one second to stop
and lock input. Hold it again to unlock with Motion off. Locked or cancelled
actions require a fresh gesture.

Use [Wi-Fi setup](wifi-provisioning.md) to join a station network.
[Browser control](browser-controller.md) requires a trusted HTTPS identity and
approval on the device. For a native client, follow the [enrollment
procedure](../bridge/README.md#local-enrollment). Pairing a browser or client
does not acquire an input lease. Try the [browser, HTTP API and AI control
examples](automation.md) after enrollment.
