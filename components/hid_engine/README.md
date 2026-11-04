# Portable HID engine

This C11 component wraps `input_core` with a fixed 32-action queue and an original composite HID Report Map. It has no SDK, radio, clock, heap or UI dependency. The single coordinator owns the engine; producers capture `state.epoch` and `state.owner`, and adapters treat all public fields as read-only. Call only `hid_engine_*` mutation APIs for the embedded state. Calls, including the send callback, must not be reentrant or concurrent.

The public contract is in [hid_engine.h](include/hid_engine.h). Keyboard input uses Report ID 1 and eight payload bytes; its LED output uses the same ID and one byte (five LED bits and three padding bits). Mouse input uses Report ID 2 and four bytes: three button bits, X, Y and wheel. The axes are relative signed values in -127..127. Report IDs are supplied separately to the callback, never prepended to a HOGP characteristic payload. There are no boot reports or protocol switching APIs. The transport owns GATT report references, subscriptions, LED output handling and report-only HOGP configuration.

## Queue and output

Initialize, provide connected/readiness state, then step twice to submit zero keyboard and mouse reports. Both must succeed for the current epoch before enqueue is permitted. Readiness means the transport can accept both IDs, including encryption/subscription requirements owned by the adapter. Mode, owner, lock, cancellation, disconnect and readiness changes clear the queue and require a fresh release barrier. Reconnect always resets ownership to local and motion intent to off. Lock rejects remote ownership, mode changes and all producer output. Only an authorized local coordinator action may call the unlock API; this library is not a remote authorization layer.

Each movement or scroll consumes one slot. A click, explicit key tap or printable US ANSI character consumes one slot until its matching zero release succeeds. Release is therefore reserved even when the queue is full. Text is limited to 32 bytes per batch, copied on enqueue and accepted atomically; insufficient capacity or any unsupported byte rejects the whole request. No Enter is appended. Explicit key taps accept a modifier bitmask and one usage, including explicit Enter/Delete/Backspace. Drag, sustained key hold, multi-key rollover chords and non-US layouts are outside this minimal API.

One step invokes the send callback at most once and reports transport submission, never host delivery or UI success. The callback must copy the payload synchronously and return whether it accepted submission. The caller must pace steps using the transport scheduler and promptly process link/lock events on the same coordinator. There is no internal busy retry, delay or clock. No output is buffered across readiness loss or reconnect.

`text_accepted` counts successful text key-down submissions in the last accepted batch. `text_completed` counts successful matching zero key-ups; `text_pending` counts queued characters still awaiting completion. Progress survives cancellation and failures, while pending work becomes zero. The caller must retain its own preview and remove only the accepted prefix after interruption; neither callback success nor these counters establish host delivery. A new accepted batch resets the progress counters. Only one text batch may be outstanding; explicit key taps can share its queue and are excluded from text progress.

## Congestion deadline

Each coordinator tick must call `hid_engine_observe_transport(engine, now_ms, congested)` before admitting actions or calling `step`. Supply monotonic milliseconds and the current transport congestion snapshot. Congestion is separate from link readiness: it blocks every enqueue and report submission, including releases, without calling the sender or spending its failure budget. A short episode preserves readiness and the core release gate; `input_can_emit` alone does not include congestion, so all report output must pass through the engine. The coordinator may keep calling `step`; it returns `BLOCKED` until congestion clears.

An episode shorter than 250 ms preserves queued work and its epoch, including a pressed key's required release. At 250 ms or later, the observation cancels all queued work once, advances the epoch, disables motion intent and requires fresh zero keyboard and mouse reports. Continued congestion rejects admission even with the new epoch, so the queue cannot refill after cancellation. Mode, lock, link and explicit cancellation transitions do not renew the episode timer. The first uncongested observation checks the elapsed deadline before resetting the timer/latch, so a delayed resume event cannot bypass expiry. A later episode gets its own deadline. On resume, the zero-release barrier precedes new output, and no cancelled action or text suffix is replayed.

A backward timestamp invalidates work immediately and starts the same release barrier. During continuous congestion that invalidation happens at most once per episode. The supplied timestamp becomes the next baseline, allowing recovery from a clock reset without unsigned deadline arithmetic replaying work. The function returns true only when that observation invalidated the epoch and requested fresh releases. Timely observation is the caller's responsibility; the engine cannot infer unobserved transport transitions or elapsed time by itself.

The coordinator may register one synchronous `hid_engine_set_cancel_callback`
after initialization. `hid_engine_cancel` invokes it before advancing the input
epoch, including automatic congestion, clock and send-failure cancellations.
The callback must not reenter the engine. Ordinary observations do not invoke
it, and initialization clears the registration. This provides an owning
qualification boundary for dependent queued work without duplicating timeout
rules in its consumers. Other state setters retain their explicit boundaries.

## Failure recovery

A failed normal submission immediately invalidates the epoch, discards remaining actions and starts a zero-report barrier. Only zero reports are retried; failures within that barrier retain its epoch and release progress. Three total send failures before completing both zero reports exhaust recovery, clear `ready` and keep `release_pending` set. Successful completion resets the failure budget. Successful keyboard release followed by a failed mouse release retries the mouse release without spending another key-down.

On retry exhaustion the coordinator must recover or disconnect the transport, and may explicitly rearm with `set_link(connected=true, ready=true)` after recovery. Old actions remain discarded. An unavailable or failed link cannot establish host key release; pending release is never acknowledged locally to conceal that uncertainty. Mode/lock changes do not reset the retry budget. The adapter must not continuously claim readiness to defeat the bound.

The Report Map follows the [USB HID report descriptor format](https://www.usb.org/sites/default/files/hid1_12.pdf) and is intended for the [Bluetooth HID over GATT Profile](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile/). Host tests validate its report sizes, usages and flags. They do not establish BLE, pairing or host interoperability.

## Host regression command

From the repository root, compile to a temporary location outside tracked source:

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I components/input_core/include -I components/hid_engine/include \
  components/input_core/input_core.c components/hid_engine/hid_engine.c \
  tests/test_hid_engine.c -lm -o /tmp/esp32-hid-engine-test
/tmp/esp32-hid-engine-test
```

The host regression also runs 100,000 deterministic lifecycle cycles using fixed engine and sender storage. They repeatedly exercise queued movement, accepted key/button presses, mode/lock/ownership cancellation, disconnect/reconnect, failed key-up recovery, short congestion and deadline cancellation. Every cycle checks bounded storage, completed releases, local ownership, cleared pending work and idle output; this is portable lifecycle evidence rather than an elapsed-time device soak.

`hid_engine_set_gyro_owned` supports the coordinator's explicitly authorized local or remote producer through the same input-core readiness/epoch/owner gates. The original function remains the local-source wrapper. It does not create a lease or bypass release/congestion checks.

`hid_engine_enqueue_mouse_key` admits one composite-keyboard tap while retaining
Mouse mode. It uses the same report-ID-1 down/up and neutral-release recovery
as explicit keyboard keys, including cancellation, owner/epoch, transport and
congestion checks, and rejects active or queued drag. The application uses it
for Navigation Back (Command+[); this never changes a keyboard draft or creates
a text batch. Home uses the existing third-button mouse click (value 4).
