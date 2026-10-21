# Portable settings and display-idle policy

This fixed-storage C11 component validates device preferences, encodes a versioned record and routes local wake gestures. It has no NVS, board, LVGL, radio, heap, thread or clock dependency. One coordinator owns each policy instance. Settings and display state do not own or modify HID mode, lock, link, ownership, preview, sending or motion intent.

## Settings record

Defaults are Dark theme, Ctrl host modifier, haptics enabled, brightness 50%, a five-minute screen timeout and Bluetooth as the preferred input transport. The brightness default is a fallback only: valid stored brightness values, including an earlier 70% preference, remain unchanged. Changing this default requires no record-version migration or boot-time write. Light/Dark, Ctrl/Command and Bluetooth/USB are the only enum values for those preferences. Brightness accepts 10..100% in ten-point steps. Timeout accepts Off or exactly 1/5/10/30 minutes. The haptic preference is independent of whether a motor is present or working; hardware capability is reported by the adapter. Preferred transport is a saved choice, not proof that the transport is available or active; the coordinator releases old input before a successful switch and only then updates this preference.

`device_settings_encode` and `device_settings_decode` use exactly 16 bytes, independent of C layout, padding or endianness:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `DSET` magic |
| 4 | 1 | Schema version 2 |
| 5 | 1 | Record length 16 |
| 6 | 1 | Theme: Light 0, Dark 1 |
| 7 | 1 | Host modifier: Ctrl 0, Command 1 |
| 8 | 1 | Haptics: disabled 0, enabled 1 |
| 9 | 1 | Brightness percent |
| 10 | 1 | Timeout minutes, or zero for Off |
| 11 | 1 | Preferred transport: Bluetooth 0, USB 1 |
| 12 | 4 | Little-endian CRC-32/ISO-HDLC over bytes 0..11 |

CRC uses reflected polynomial `0xedb88320`, initial value `0xffffffff` and final complement. It detects accidental corruption and is not authentication. Decode distinguishes invalid arguments/length/magic/checksum/value from unsupported schema, and leaves the destination untouched on every failure. It never silently replaces data with defaults. The persistence owner decides how to expose missing, corrupt, unsupported or unavailable storage and retain recovery evidence.

Encode always writes schema 2. Decode also accepts valid schema-1 records only when their old reserved byte 11 is zero, migrating the preferred transport to Bluetooth. A nonzero schema-1 reserved byte is rejected even with a valid CRC; it is not reinterpreted as USB. Unknown future versions are rejected. Migration changes the in-memory representation without writing storage here. The next requested encode uses schema 2; loading an old record alone does not request a save.

Equality compares validated fields, never struct padding. `device_settings_apply` returns Changed only for an actual validated preference change. The persistence adapter may coalesce saves on that result; polling or repeated equal values never request another write. This component performs no writes and does not persist defaults just because it booted. Actual durable-save results belong to the NVS adapter.

## Display idle

The configured timeout applies in every power state, including unknown. There is no power-input gate or inferred USB/battery state. This implements display/backlight blanking only: it never requests MCU sleep or suspends BLE/Wi-Fi/HID. User-enabled mouse motion may continue with the screen off; local keyboard focus should pause while hidden. The coordinator retains preview, sends, locks and ownership across display transitions. The UI should explain that mouse control remains active with the screen off.

Initialize with monotonic milliseconds. `device_idle_tick` changes the desired `asleep` state at exact elapsed time greater than or equal to the timeout, unless timeout is Off or a physical contact/wake sequence is held. There is no timer allocation or absolute-deadline addition that can overflow. Gyro, RSSI, network, render and transport polling are not activity and must not call activity APIs. Repeated timeout configuration does not renew the deadline. A changed timeout wakes and starts a full interval; choosing Off wakes immediately.

For any explicit changed setting, call `device_idle_activity` to wake and start a new interval. For real local callbacks, call `device_idle_note_physical_activity` once their timestamp changes: while awake, a newer timestamp renews idle even if the queued BEGIN was discarded by priority handling. It never wakes by itself, changes the observed clock or releases wake capture. Older/repeated timestamps are harmless. A timestamp from a callback after the previous policy tick may be newer than the policy clock; tick waits until that timestamp before measuring elapsed time, avoiding unsigned subtraction. The caller supplies real monotonic callback timestamps and separately routes actual BEGIN events.

A backward clock wakes the display, starts a new interval and consumes old/in-flight tokens. `clock_regressed` reports the latest clock-observing call; the callback-only activity API does not observe that clock. The coordinator must still process actual raw release/resynchronization when queues overflow or priority actions discard intents. Hardware state is outside this desired-state model: a failed backlight wake must remain input-blocked and visible in the owning adapter.

## Wake gestures

Use nonzero, strictly increasing 64-bit tokens independently for Touch and AI. Each BEGIN starts one physical press; MOVE/END/CANCEL and delayed click/double/hold ACTION carry that token. One touch gesture means the supported single-finger contact. Token wrap is rejected, never reused. Only Pass permits normal action handling; Consume and Invalid cannot trigger host input, ownership takeover, motion toggles, typing or lock/unlock. END/CANCEL bookkeeping and safety cleanup must still be processed in the owning coordinator.

A valid BEGIN while asleep wakes and consumes the full press, including held input and delayed callbacks. Inputs beginning while any captured contact remains down are captured too. Touch-only capture ends on release; the next fresh touch can act immediately. A captured AI release starts a 280 ms AI guard. Another AI press strictly inside that interval is consumed in full and its release starts a new guard, so a wake double-click cannot become a mouse toggle. At exactly 280 ms a fresh AI press can act. A fresh touch can Pass during the AI-only guard after all captured contacts release. `wake_capture` includes the AI guard: use it to suppress gyro, and use per-event routes for touch movement.

Per-source token watermarks continue consuming old wake callbacks even after newer gestures begin. Old actions arriving while asleep cannot wake or replay. ACTION is admitted at most once per current token; duplicates and malformed/stale events do not renew idle. A normal CANCEL may Pass for cleanup, but consumes all later actions from that gesture. These rules consume the wake hold itself: a second, fresh physical hold is needed to unlock after waking.

## Focused host regression

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I components/device_settings/include \
  components/device_settings/device_settings.c tests/test_device_settings.c \
  -o /tmp/esp32-device-settings-test
/tmp/esp32-device-settings-test
```

Regressions use independent CRC fixtures, every supported settings combination, single-bit record corruption, valid-CRC invalid fields, no-op apply loops, 1 ms idle boundaries, all-power/default behavior, held contacts, callback-only activity, delayed/stale actions, overlapping wake inputs, the AI guard, clock regression and unsigned-time endpoints. They do not validate NVS durability, actual backlight levels, rendering, radio continuity or hardware gesture timing.
