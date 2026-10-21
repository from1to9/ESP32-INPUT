# USB HID and shared CDC owner

Run `python3 tests/usb_hid/run.py` after resolving the pinned firmware components.
The suite compiles the production `usb_hid.c` and `usb_console.c` with strict
C11 diagnostics, AddressSanitizer and UndefinedBehaviorSanitizer. It uses the
real pinned TinyUSB headers and descriptor macros; hardware, FreeRTOS and
esp_tinyusb calls are controllable doubles below that API boundary.

One `usb_console_start()` installation owns CDC diagnostics and a report-only
HID keyboard/mouse interface. `usb_hid_init()` starts that same owner. Concurrent
startup observes an initializing error; repeated completed calls return the
cached result without installing again. Failed partial startup remains failed
until reboot, preserving any resources referenced by a partially redirected
CDC console. The SDK supplies matching IAD device/qualifier descriptors and a
chip-unique serial when the tracked empty serial setting is applied. The suite
compiles the pinned SDK identity implementation and verifies the actual
chip-derived string, empty failure result and IAD/device/qualifier fields.
Its class-bitmap product ID changes from CDC-only 0x4001 to
CDC+HID 0x4005. The current default profile has one CDC function and one HID
interface; interface/endpoint changes require independent qualification.

The FS configuration uses 64-byte CDC bulk endpoints; HS uses 512 bytes. Both
have one 16-byte HID IN endpoint, with one-millisecond polling encoded as FS
interval 1 and HS interval 4. Notification, CDC OUT/IN and HID IN endpoint
addresses are distinct. LED output uses EP0 SET_REPORT. The canonical report
descriptor is shared directly with `hid_engine`: report IDs 1/2 select 8-byte
keyboard and 4-byte mouse payloads. TinyUSB adds the wire report ID. No Boot
Protocol, HID OUT endpoint, audio/UAC, remote wakeup, host command parser or
automatic-download/reset handler is advertised by this implementation.

Input is disabled initially. The coordinator selects one input transport,
requires fresh zero-release reports after a generation change, and releases the
old transport before switching. `usb_hid_send_generation()` only copies into
one bounded slot and notifies a worker; it never enters TinyUSB's event queue or
endpoint mutex. Adapter admission, actual endpoint submission and transfer
completion have separate lifetime counters. None establishes focus or an
intended host application action. An accepted but cancelled/failed report must
not be automatically replayed.

A 3072-byte worker posts at most one deferred callback to the USB task. The
pinned TinyUSB `usbd_defer_func()` can wait on its private queue, so only that
worker uses it. The independent timer revokes HID after a 100 ms slot/deferred
dispatch deadline, checked every 10 ms. Endpoint backpressure is observed before
the single submission attempt, without an automatic retry after SDK rejection.
There is no allocation per report. Lifecycle, class callbacks and actual HID
submission run on the same USB task; the coordinator shares only short status
critical sections. Target task scheduling and USB bus timing still require
hardware qualification.

A non-claiming TinyUSB class observer sees the real configuration reset path,
including bus resets that do not call `tud_umount_cb()`. It advertises no extra
interface. After reset, the USB task completes HID class reset before another
deferred submission can run. Previously posted tokens remain owned until their
callbacks drain and cannot submit data under a new generation. Completion
attribution relies on this pinned TinyUSB/DCD reset barrier and USB-task FIFO
ordering; completions are not themselves tagged with an application generation.
An ownerless old completion is ignored; mismatched completion data quarantines
the endpoint instead of acknowledging a fresh report.

Suspend or disable cancels unsubmitted work. If a report is already dispatching
or in flight, HID is quarantined because that report cannot safely be recalled.
The snapshot keeps actual mounted/suspended and outstanding state; failure is
not reported as a disconnected or released host. Recovery invalidates the
generation and preserves CDC installation. A real USB reset/detach is required
to clear uncertain ownership; the coordinator then re-enables the selected
adapter and requires a new release barrier. No old report is resent. A queued
hardware transfer may still complete after interruption, and a host may retain
held state; the UI must not claim a successful switch without proven release
or disconnection.

GET_REPORT bounds copies to the requested size. Keyboard state comes from the
last matching completion. Mouse GET_REPORT returns cached button state and zero
relative axes/wheel, preventing repeated motion from control reads. Invalid
instances, report IDs and report types return zero. LED output is exactly one
payload byte with five defined bits. Nonzero SET_IDLE is rejected rather than
automatically resending old input; an unsupported Boot Protocol request latches
a failure instead of changing the report format.

The tests exercise descriptor structure, sizes, unique endpoints and intervals;
single-owner installation; local copy ownership; generation rejection;
queued-versus-submitted-versus-completed state; deferred reset races; late and
mismatched callbacks; suspend/resume; disable during submission; CDC-preserving
recovery; backpressure; timeout/clock boundaries; startup failure; keyboard LED
output; GET_REPORT bounds; zero relative motion on GET_REPORT; generation/token
exhaustion; and 100,000 reset/report lifecycle cycles with bounded allocations.

These are host logic and target-compilation checks, not device or host-stack
qualification. Confirm enumeration at FS and HS, keyboard LEDs, text and mouse
behavior, CDC continuity, real suspend/reset/cable behavior, timeout handling,
power, latency and stack margin on the actual Mosaico and each host. USB audio
requires separate UAC descriptors, codec/resource ownership, streaming and host
qualification; selecting USB input does not make audio available. ESP32 variants
also differ in OTG/PHY and Classic Bluetooth capabilities, so another board
cannot inherit Mosaico qualification.
