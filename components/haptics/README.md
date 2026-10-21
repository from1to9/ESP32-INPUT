# Portable haptic policy

`haptics.c` owns deterministic feedback admission, pulse phases, cooldown,
cancellation and motor fault state. It has no ESP-IDF, LVGL, heap, task, queue or
storage dependency. A single owner serializes calls and supplies monotonic
`uint64_t` milliseconds and a bounded motor-write callback. See
[haptics.h](include/haptics.h) for the public contract and
[host regressions](../../tests/test_haptics.c) for fault injection at that boundary.

[haptics_dispatch.h](include/haptics_dispatch.h) provides the shared control/
hardware dispatch boundary. Control changes and the generation check plus
complete driver write use the same adapter-provided lock. Every nonzero phase,
including the second error pulse, carries the worker's original generation. A
disable/enable pair cannot turn an old token into permission for a new pulse.
Writes rejected by this gate return `HAPTICS_WRITE_CANCELLED`; the policy
cancels its pattern without marking the motor failed. Driver acceptance,
cancellation and hardware failure remain distinct results. Zero writes bypass
the control gate so cancellation cannot prevent a safety stop. Generation
exhaustion fails closed rather than wrapping into an old token.

| Event | Requested motor output | Duration |
| --- | --- | --- |
| Confirm | 35% | 24 ms |
| Mode | 55% | 45 ms |
| Lock | 65% | 65 ms |
| Error | 65%, off, 65% | 40 ms on, 65 ms off, 60 ms on |

These values are initial policy values, not measured perceptual strength. PWM
duty is not a calibrated force. Qualify starting torque, comfort, temperature
and motor lifetime on the actual board before changing the values.

Every accepted pattern reserves a further 200 ms cooldown. Requests received
while playing or cooling down are dropped; there is no deferred queue. Polls
advance from the original start time, skip expired phases and preserve an off
interval after a late stop. If the worker misses the error pattern's off phase,
it stops instead of merging two pulses. A late observed first stop cannot
compress the 65 ms off gap. Poll at least every 10 ms; physical stop precision
depends on the firmware scheduler and hardware driver.

Initialization verifies motor-off before enabling feedback. Unsupported hardware
does not invoke the callback. Disable cancels every phase and writes off before
the portable call returns; re-enabling does not restore work or shorten the
cooldown. A failed write attempts an immediate safety stop, latches unavailable
and disables admission. If off cannot be confirmed, polls retry off at most once
per 20 ms. Accepted off retries do not clear the fault. Explicit recovery must
follow hardware repair/reinitialization, verify off and leave admission disabled.
Clock rollback and unrepresentable end times also fail closed. The policy cannot
guarantee a physical stop when the driver keeps rejecting off commands.

An adapter should run this policy in a low-priority worker. Board-facing disable
invalidates its mailbox immediately and wakes the worker for motor-off; it must
not wait for the pulse duration. If a motor write wins the dispatch lock, that
write commits before cancellation; if cancellation wins, no stale nonzero write
reaches hardware. Preference persistence belongs to the application settings
adapter; this portable policy never accesses NVS.

## Host regression command

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I components/haptics/include components/haptics/haptics.c \
  tests/test_haptics.c -o /tmp/esp32-haptics-test
/tmp/esp32-haptics-test
```
