# USB audio transport checks

Run `python3 tests/usb_audio/run.py` after resolving the pinned firmware
dependencies. The suite compiles production `usb_audio.c` against the actual
TinyUSB audio headers with strict diagnostics and address/undefined sanitizers.
Only RTOS, heap, time and stack FIFO boundaries are replaced.

Thirteen isolated cases cover exact mono PCM16 frames, short writes, queue
saturation, expired samples, monotonic sequence rejection, disconnect during
FIFO submission, same-format reconnect identity replacement, stop/flush
acknowledgement, fragmented playback input, clock-control requests, unsupported
formats, same-alternate stream replacement, suspend/resume selection retention,
allocation/worker failure and 10,000 repeated reconnections without
new allocations. The existing [USB HID suite](../usb_hid/README.md) separately
checks the combined real full/high-speed descriptors, unique endpoints, chip
serial and the new class-bitmap PID while optional audio allocation fails.

The audio function shares the existing stack and exposes one mono 48 kHz,
16-bit format. Host interface selection does not grant microphone permission.
Application queues use fixed PSRAM allocations; stack FIFOs remain bounded.
Only the dedicated worker enters TinyUSB FIFO APIs. Stop waits for its flush
barrier, while already submitted host/controller packets cannot be recalled.
USB connection IDs identify observed attachment generations, not authenticated
host names. Audio and HID readiness remain independent.

These are local source-level checks. Manager/consent integration, target link,
USB enumeration, acoustic quality, timing and OS compatibility require their
separate evidence. This delivery explicitly defers device tests and flashing.

Playback feedback uses the full unconsumed sample span, including manager-admitted
frames, and advances only on a successful codec-consumption receipt. A 120-second
feedback-following host simulation covers full/high speed, independent codec-rate
error and scheduling jitter without overflow. Endpoint close is a separate
revocation boundary; an invalid alternate or failed reopen cannot remain ready.
Dropped frames retain sequence/sample-offset gaps. These are source-level
regressions, not USB enumeration or acoustic/timing qualification.

Playback consumer admission is separate from route preparation. A binding change
invalidates old queued/partial samples and waits for an OUT FIFO flush without
closing the independently authorized microphone. Produced frames retain the
consumer generation captured at ingress; a rebind during the actual FIFO read
drops those bytes. Tests cover a full queue, stale close/consumption receipts,
sequence restart, an interrupted read and microphone continuity across rebinds.
