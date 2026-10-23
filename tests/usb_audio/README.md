# USB audio transport checks

Run `python3 tests/usb_audio/run.py` after resolving the pinned firmware
dependencies. The suite compiles production `usb_audio.c` against the actual
TinyUSB audio headers with strict diagnostics and address/undefined sanitizers.
The original thirteen cases replace RTOS, heap, time and stack FIFO boundaries.

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
separate evidence. Running this suite does not access or flash a device.

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

The suite also runs seven failure-preserving generator checks and a separate
comparison using the complete pinned TinyUSB class, its real FIFO implementation
and production `usb_audio.c`. Only DCD/USBD submissions and cached endpoint setup
are substituted at the stack boundary; enumeration is not emulated. The original
DMA class must reproduce an empty OUT completion consuming the armed transfer
without rearming it. The generated class must rearm, append no PCM for empty
packets and subsequently admit a complete real frame through the consent gate.
Consumer replacement, partial-frame purge, stale identity, bus reset, rejected
rearm and an explicitly fault-injected positive FIFO failure remain covered.

Independent full/high-speed capture cases reproduce the original IN overwrite:
20 worker calls accept 1,920 bytes while only 392 bytes remain and no IN transfer
has completed. The generated class configures function 1's IN FIFO as
non-overwritable before use. The same calls then accept only 392 bytes, preserve
the oldest unread PCM and retain the partial frame and bounded application queue.
Actual IN completions exercise the initial empty transfer and 94/96/98-byte
flow-control packets with bounded worker jitter. At every step, accepted bytes
equal completed bytes plus the pending DCD packet plus FIFO contents, all checked
against varied source PCM. The run completes 3,836 of 3,840 accepted bytes; four
bytes remain in the real FIFO awaiting more samples. No tail is padded or counted
as completed. Rejected admission can retry the same sequence, and unchanged age
expiry, stop flush and reset/reinitialization policy are covered.

Two further full/high-speed cases send a finite single 960-byte frame. With the
prior OUT/overwrite repairs applied, the real class chose a nominal 96-byte
packet while its flow-control blackout remained active and only 94 bytes were
available. Ignoring the actual FIFO read length scheduled two stale bytes. The
final DMA path schedules the returned read length. Both cases now complete
exactly 960 varied PCM bytes including the short 94-byte tail, followed only by
empty transfers. The callback byte total equals actual completed bytes. The
runner derives the prior class by reversing only this assignment and verifies
its independently reviewed full source hash before reproducing both failures.
All five final real-driver cases pass; the three original-class defects and two
isolated finite-tail failures remain expected, explicitly identified aborts.

The generated class preserves upstream licensing and uses the exact reviewed
preimage, patch and postimage hashes in
[`tinyusb-audio-streams.json`](../../patches/tinyusb-audio-streams.json).
The build substitutes one source in the existing component target, preserving
its options; managed source remains untouched. Unknown source, lock, patch or
existing output content fails verification and is preserved. Every build checks
the generated copy again. Local comparison manifests retain before/after input
hashes and both outcomes under `.agent/local/usb-audio-out/`.

USB observations count post-rearm class callbacks separately from worker FIFO
reads/writes, queued/partial samples and actual complete playback frames. A failed
rearm does not reach these completion hooks; absence of callbacks is not a known
zero-error result. Neither a host output callback nor FIFO acceptance proves
physical delivery. The capture conservation check covers successful simulated
completions: the class removes IN bytes before requesting the next DCD transfer,
so a rejected IN rearm remains a separate loss/accounting limitation. OUT FIFO
overwrite policy is unchanged. These checks do not qualify all-direction lossless
transport or microphone correctness. The observed physical capture overflow remains unexplained, and a
non-reading output-only host alone does not establish its cause. Physical empty
OUT packets, USB compatibility and audible playback still require device evidence.
