# Bounded local touchpad latency observation

`CONFIG_APP_TOUCH_LATENCY_DIAGNOSTICS` is a diagnostic-image option, default
`n`. Enable it in the local `firmware/sdkconfig` and build with
`python3 tools/idf.py build`. It adds no public control API, timer, task,
permission, input policy or scheduling change. Management owns installation
and device collection.

The image logs `TouchTrace armed` at coordinator startup. The window starts
only when the coordinator accepts a nonzero, current local pad movement with
Bluetooth HID ready and its release barrier clear. Startup, menus, stationary
contacts and unavailable input leave the window unused. The trigger's gesture,
source time and generation are retained. Its earlier scan/enqueue occurred
before the window, so source/enqueue counts can be smaller than consumed or
motion counts by that initial event. Only one window is available per boot.
With the optional coordinator window below, the same first-movement trigger
waits until that window is active.

The single tester action is: **once Mouse / Bluetooth shows Input ready, use
the body touchpad normally for 20 seconds**. Keep that input mode and connection
for the capture. No circle pattern, audio playback or USB operation is needed.

After 20 seconds, the existing 10-second health-log pass emits one frozen
`TouchTrace` summary (13 lines). A Bluetooth generation change closes it early
with `link_changed=1`. Keep the existing boot/build identity and the same
Windows observation with these lines. There is no per-frame serial output,
coordinate recording, allocation, sensor reread or HTTP polling requirement.

## Reading the summary

- `scans`, `single_pad`, `invalid`, `overflow`, `source_missing` and enqueue
  drops separate valid board observations from unavailable/lost input. An
  unchanged contact is still observed here, unlike the change-only journal.
- `primary` and its drops are BEGIN/MOVE/END-or-CANCEL counts. MOVE includes
  existing stationary heartbeats. `consumed` is contact/primary consumption.
  `queue_observed_peak` is a sampled queue depth, not an atomic high-water mark.
- `motion`, `merged` and `rejected` describe nonzero displacement enqueue
  attempts. Merge includes cancellation of opposite pending displacement.
- `steps_idle_blocked_sent_failed_exhausted` preserves the engine's actual
  return states. `send`/`send_rejected` count movement callbacks and their
  transport results; release-only retries remain visible in the step counts.
- `sdk_started`, `accepted`, `rejected` and the last SDK error describe actual
  Bluetooth movement submissions. `complete_ok`/`failed` only count callbacks
  matched by the transport's existing connection/handle reservation. `early`
  counts completion before SDK return. A completion error stays an error.
  At window close, `outstanding` and `return_unobserved` preserve unfinished
  requests; an absent completion is never success. Counts need not balance
  across the capture boundary.
- `busy_span_us` is the longest *observed* congested span at coordinator polls,
  with its last observation time. It is not exact radio occupancy.

Each metric reports its count, maximum, absolute monotonic `at_us`, source
reference and histogram. The exclusive bucket sequence is <=1, <=5, <=10,
<=20, <=50, <=100, <=250 and >250 milliseconds. These are observation bins,
not input thresholds. Empty metrics have `n=0`; their zero maximum is unknown.

`scan_interval` includes adjacent source sequences with valid single pad
contacts on both sides. `primary_age`/`contact_age` measure source-to-consumer
age; `motion_enqueue_age` measures source-to-engine age. `send_age` uses the
actual head action's oldest buffered source time, just before calling the
transport; unbuffered sources increment `age_unknown`. `submit_duration` and
`complete_duration` are measured from SDK submission entry. `submit_interval`
is movement-submission spacing and can include stationary time; use it with
input ages, idle/merge counts and the Windows observation. `source` is a scan
sequence for scan intervals, source milliseconds for ages, and a capture-local
request ID for SDK metrics. Board/primary clocks have millisecond precision;
SDK entry/return/completion use the existing microsecond time source.

SDK acceptance or its completion callback does not establish Windows receipt,
radio delivery time or visible cursor motion. The summary locates waiting
before/inside submission and pending completion; downstream attribution still
requires the same-window host evidence.

## Cost and verification

The ESP32-S31 diagnostic objects add 852 bytes of observer state and 8 bytes to
the Bluetooth state in internal memory, plus a 776-byte frozen summary in the
existing external-memory section. No heap storage is used. During capture,
each movement submission adds one coordinator timestamp and two SDK-boundary
timestamps, plus one for its matched completion. The native BLE fixture checks
59 additional reads for 20 SDK attempts and 19 successful completions; it
excludes the separate coordinator timestamp. Existing source/consumer clocks
are reused. Queue depth adds one query per observed producer callback.
Updates use short task critical sections; none logs or calls the SDK while
holding the observer lock. Target CPU time and real lock/timestamp cost have
not been measured. Native clocks and no-op lock substitutes are not a device
performance bound. After close, diagnostic callbacks take only their inactive
checks. Normal builds compile out the hooks and omit the observer object.

Run `python3 tests/touch_trace/run.py` for activation, fixed-window/failure
boundaries, synchronous early completion and identical on/off production
coordinator/BLE reports. The two composition fixtures retain their named
hardware boundary substitutes; they do not measure radio or host timing.
`python3 tools/check_stack_budget.py` includes the observer's main, LVGL and
BTC paths when this configuration is enabled. Task stack accounting retains
its SDK reserve and does not qualify interrupts or device timing.

## Coordinator window after audio playback

Enable `CONFIG_APP_MOUSE_PHASE_DIAGNOSTICS=y` in the diagnostic image.
For a paired Bluetooth retest, also enable `CONFIG_APP_TOUCH_LATENCY_DIAGNOSTICS=y`.
After boot, start serial and Windows pointer capture. Leave Mouse Motion off.
Play audio in Audio mode, reproduce any audible interruptions, then stop
playback and select Mouse locally. Begin moving a finger across the pad as
soon as Mouse appears and keep moving for 20 seconds. Record the Windows
time of any stall and keep the recording aligned with the serial timestamps.
Keep serial capture running for another 15 seconds for both summaries.

`MousePhase armed` indicates that the image is waiting for the first
successful local Audio to Mouse selection. That selection starts the window
even if the link is unavailable or no pad event arrives. The diagnostic
trigger observes mode selection. Verify playback and stop separately.
One header and ten two-second rows are emitted after the window, one line per
coordinator turn. The header carries the exact window and the first observed
boundary at or after its end. If the coordinator never resumes, no summary
can be printed.

- Phase maxima use monotonic wall time. A duration belongs to the interval
  containing its start and is clipped at the capture deadline. `pause` is
  the time from the previous turn's end to the next turn's start, including
  the normal delay. Phase durations also include preemption and waits.
- Each row counts consumed contacts and pad events, admission decisions,
  nonzero buffered movement sends, and observed cancellation paths. Events
  use the last phase boundary, so a long phase can span more than one row.
  Several counters can describe the same event. `send` reports the transport
  adapter's return. Use `TouchTrace` for matched Bluetooth completions and
  Windows evidence for host arrival and cursor behavior.
- `states` is the union of bits sampled just before HID service in the row.
  The header lists bit names from least to most significant. A row with
  no sampled turns can result from a pause spanning that interval.

The optional observer uses fixed coordinator-owned storage and no additional
task, timer, lock, queue, or allocation. It adds up to seven clock reads per
active turn and one clock read at the successful trigger. Default builds
omit the object and hooks. The target stack checker includes its phase and
reporting paths when enabled. The focused native check is
`python3 tests/coordinator/run.py --mouse-phase --case mouse_phase_observes_local_transition_and_stall_once`.
