# USB audio transport checks

Run `python3 tests/usb_audio/run.py` after resolving the pinned firmware
dependencies. The suite compiles production `usb_audio.c` against the actual
TinyUSB audio headers with strict diagnostics and address/undefined sanitizers.
The original thirteen cases replace RTOS, heap, time and stack FIFO boundaries.
A fourteenth case preserves existing admission on profiles that do not require PTI.

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

The suite also runs thirteen failure-preserving generator checks and a separate
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
runner retains the prior class without the new observation hooks and with the
old read-length assignment; it verifies that independently reviewed full source
hash before reproducing both failures.
All five final real-driver cases pass; the three original-class defects and two
isolated finite-tail failures remain expected, explicitly identified aborts.

The generated class preserves upstream licensing and uses the exact reviewed
preimage, patch and postimage hashes in
[`tinyusb-audio-streams.json`](../../patches/tinyusb-audio-streams.json).
The build substitutes the audio class and DCD sources in the existing component target, preserving
its options; managed source remains untouched. Unknown source, lock, patch or
existing output content fails verification and is preserved. Every build checks
both generated copies again. Local comparison manifests retain before/after input
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


The standard suite also runs a six-row native preprocessing matrix using the
actual forced-include order. SDK macros arrive through `sdkconfig.h`, not command
line configuration overrides. Only nonzero ESP32-S31 plus DMA selects PTI;
Slave, another target, explicit zero values and a non-ESP host preserve the
existing default. This is configuration evidence, not target compilation.

`run_dwc2.py` executes the complete pinned/generated DWC2 DCD, generated audio
class, real FIFO and adapter, using the production descriptors and real
`audiod_open`/SET_INTERFACE/clock-request parsing. USBD submissions/control
responses and physical MMIO/IRQ/cache boundaries are substituted. The model
applies odd/even command effects and transfer/interrupt acknowledgement at DCD
call/IRQ boundaries. It is not a full USBD core, USB host or physical simulation;
DMA/cache timing, power transitions, every register effect and actual host phase
remain unqualified. The register contract comes from the pinned SDK's
`components/soc/esp32s31/register/soc/usb_otghs_struct.h`, hashed in the evidence.

The same positive OUT/PCM oracle is used for the baseline and candidate:
32 completions, 3,072 bytes and three exact varied 960-byte frames. The interval
comes from the production descriptor. One hundred outcomes cover FS control,
all eight HS phases, frame wrap and prompt/one/three-microframe delayed IRQs.
The baseline has 17 explicitly identified failures: the register test plus the
16 prompt HS phase/wrap cases strand after 96 bytes. Odd delayed baseline cases
can progress. The candidate passes all 50 outcomes. This establishes a supported
programmed-phase failure and configuration candidate, not the cause or repair
of the recorded physical Speaker timeout.

The existing DCD PTI option is selected only for the S31 DMA profile; endpoint
intervals, PCM rates, queue limits and deadlines are unchanged. An explicit audio
lifecycle callback checks observed internal-DMA capability, DMA enable, descriptor
DMA off, Ignore Frame Number readback and thresholds off at init/reset. Unknown
or invalid mode blocks this profile's audio admission. New reset epochs revoke
old audio tokens through the existing deferred flush boundary, including before
the deferred attachment callback; initial callbacks do not allocate or start
audio. Other profiles retain existing admission. HID policy is unaffected.

Candidate scenarios additionally cover feedback calculation/submission/completion
values and epochs, successful versus failed zero-byte IN events, real OUT drop
handling, ordinary ZLP progress, rejected DCD rearm with no extra retry, unknown
and individually rejected modes, first-failure/lifetime retention, stale tokens,
flush barriers, stop/alt0/reopen/reset and capture consent. A stalled capture
host retains 392 unread bytes and the partial application frame; resumed real
DCD/class completions send exactly one varied 960-byte frame including its
94-byte tail. Explicit failed IN events are injected at the DCD reporting
boundary; the test does not pretend PTI's masked incomplete-IN interrupt fires.
The existing composite descriptor/CDC/HID and earlier finite-tail tests remain
in the standard repository checks.

Transport observations use fixed internal state and a separate copy-out snapshot,
keeping the old USB snapshot's return size unchanged. DCD counters distinguish
attempt/accepted/rejected returns from successful/failed completions, successful
bytes and successful empty packets for 0x04, 0x84 and 0x85. Class OUT entry and
actual existing rearm returns are counted separately. Feedback calculated,
submitted and successfully completed values have independent known/length/epoch
fields; they are not host acknowledgement. Counters saturate and retain failures
across reset. Unknown endpoints are excluded. No observer allocates, logs, reads
PCM, retries an operation or calls USB/FIFO APIs.

Register samples are cached only inside active init/reset, OUT submission and
existing IRQ contexts. A bounded modulo-128 IRQ phase samples the first observed
pending OUT for an activation; it neither creates interrupts nor establishes a
stall. Samples include point/reset epoch/activation and raw DSTS before/after,
are non-atomic, can be stale and never imply a current powered endpoint. Only
non-destructive registers are read: no RX status-pop or FIFO reads. The snapshot
getter only copies cached metadata, so clock-gated peripherals are not accessed.
Lifecycle mode validation/revocation is explicit production behavior and remains
separate from this neutral observation path.

Each DCD run pins inputs before generation, asserts generation-input stability,
checks both generated postimages, and hashes both compiled generated files
before/after. Exact compile/link commands, scenario raw-log hashes, baseline
failure names and model limitations are retained locally. Managed sources and
unknown/concurrent generated artifacts are preserved; `--check` performs only
read-only verification and never recreates a missing file. No suite accesses a
device, serial connection or radio.
