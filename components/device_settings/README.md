# Portable settings and display-idle policy

This fixed-storage C11 component validates device preferences, encodes a versioned record and routes local wake gestures. It has no NVS, board, LVGL, radio, heap, thread or clock dependency. One coordinator owns each policy instance. Settings and display state do not own or modify HID mode, lock, link, ownership, preview, sending or motion intent.

## Settings record

Defaults are Dark theme, Ctrl host modifier, haptics enabled, brightness 50%, a five-minute screen timeout, Bluetooth input, tap-to-click enabled, Navigation disabled and Bluetooth name `ESP32-INPUT`. Valid stored brightness values, including earlier 70% preferences, remain unchanged. Light/Dark, Ctrl/Command and Bluetooth/USB are the only enum values for those preferences. Brightness accepts 10..100% in ten-point steps; timeout accepts Off or exactly 1/5/10/30 minutes. Tap-to-click off disables pad-generated clicks/drag while preserving movement and explicit mouse buttons. Applying that policy must cancel any touch-owned hold through the coordinator's release path. Preferences do not establish motor/transport availability, host delivery or a live gesture. The coordinator persists transport/name changes only after their owning manager confirms application.

`device_settings_bluetooth_name_valid` is the shared pure validator for settings and BLE adapters. It accepts 1..18 UTF-8 bytes, excluding the terminator; rejects malformed/overlong sequences, surrogates, code points beyond Unicode, NUL/C0/C1 controls and outer ASCII spaces. Internal spaces and valid multibyte names are allowed. The explicit-length input need not be terminated. Runtime settings own a 19-byte terminated name; an unterminated array is invalid.

Encoding uses exactly 56 bytes for schema 5, independent of C layout, padding or endianness. Decoding additionally accepts 16-byte schema-1/2 and 40-byte schema-3 and 56-byte schema-4 records:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `DSET` magic |
| 4 | 1 | Schema version 5 |
| 5 | 1 | Record length 56 |
| 6 | 1 | Theme: Light 0, Dark 1 |
| 7 | 1 | Host modifier: Ctrl 0, Command 1 |
| 8 | 1 | Haptics: disabled 0, enabled 1 |
| 9 | 1 | Brightness percent |
| 10 | 1 | Timeout minutes, or zero for Off |
| 11 | 1 | Preferred transport: Bluetooth 0, USB 1 |
| 12 | 1 | Tap-to-click: disabled 0, enabled 1 |
| 13 | 1 | Bluetooth name length, 1..18 UTF-8 bytes |
| 14 | 19 | Name bytes, NUL and zero padding |
| 33 | 3 | Reserved, must be zero |
| 36 | 2 | X gain, LE uint16 in centi HID units/degree |
| 38 | 2 | Y gain, LE uint16 in centi HID units/degree |
| 40 | 2 | Deadzone, LE uint16 in milli-degrees/second |
| 42 | 2 | Filter milliseconds, LE uint16 |
| 44 | 2 | Settling freeze milliseconds, LE uint16 |
| 46 | 1 | Bit 0 invert X, bit 1 invert Y; other bits zero |
| 47 | 1 | Navigation layout: disabled 0 (default), enabled 1 |
| 48 | 4 | Reserved, must be zero |
| 52 | 4 | Little-endian CRC-32/ISO-HDLC over bytes 0..51 |

CRC uses reflected polynomial `0xedb88320`, initial value `0xffffffff` and final complement. It detects accidental corruption and is not authentication. Decode distinguishes invalid arguments/length/magic/checksum/value from unsupported schema, and leaves the destination untouched on every failure. It never silently replaces data with defaults. The persistence owner decides how to expose missing, corrupt, unsupported or unavailable storage and retain recovery evidence.

Legacy records retain their length 16, CRC over bytes 0..11 and little-endian checksum at 12..15. Schema 1 requires reserved byte 11 zero and derives Bluetooth transport; schema 2 retains its saved transport. Both preserve every old field and default only absent tap/name and motion fields. Schema 3 preserves its saved tap/name values and defaults only motion fields; its CRC remains at 36..39 over 0..35. Invalid flags, lengths, nonzero padding/reserved bytes or unknown versions are rejected even with a valid CRC. Schema 4 retains all motion fields and defaults the absent Navigation preference to off; its byte 47 must remain zero. Migration changes memory only. A later explicit changed preference encodes schema 5; loading legacy data or requesting unchanged logical settings never triggers a format-upgrade write.

Motion defaults are independent X/Y gain 4 HID units/degree, deadzone 0.5 degrees/second, filter 30 ms, freeze 150 ms and no inversion. Durable gains accept 25..10000 centi-units (0.25..100); deadzone accepts 0..10000 milli-degrees/second and filter/freeze accept 0..1000 ms. Runtime motion intent, gravity, calibration and confidence are never persisted.

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
