---
scope: repository-shared
portability: cross-checkout
status: active
verified_at: 2026-10-26
evidence_kind: reviewed-source-host-target-and-limited-device-evidence
privacy_reviewed: true
contains_local_runtime_facts: false
---

# Engineering lessons and unresolved defects

Use this index before changing startup, hardware scheduling, memory placement or
product UI. The [knowledge contract](KNOWLEDGE_CONTRACT.md) governs promotion;
[validation](../../docs/validation.md) owns image-specific results and failures.
These lessons describe the pinned Mosaico ESP32-S31 profile. Recheck assumptions
when the SDK, BSP, board, compiler, configuration or relevant ownership changes.
Source synchronization does not establish device or release qualification.

## 1. Stack budgets must follow the compiled call chain

**Confirmed defect:** large coordinator snapshots and nested target requests
exceeded the main task's stack budget even though compilation succeeded. A large
startup frame could remain allocated for the non-returning runtime loop. Native
tests did not reproduce the target ABI's frame sizes.

Separate startup and runtime frames; give large observations bounded storage
owned by one task or serialized operation. Incoming, retained and pending data
need separate lifetimes. A shared workspace must not overwrite an accepted
request during callback reentry. Clear private transient copies after their
consumer has copied them.

Check compiler split/clone bodies and deeper adapters, not just public function
frames. Keep SDK/library/RTOS/interrupt reserve explicit. A known-path sum is not
a whole-program upper bound, and one task's watermark says nothing about another.

Evidence: [stack checker](../../tools/check_stack_budget.py),
[coordinator regressions](../../tests/coordinator/README.md) and
[runtime memory ownership](../../design/runtime-memory.md).

## 2. Available PSRAM does not satisfy internal DMA allocations

**Confirmed mechanism:** the display's logical boot-to-product handoff could
complete while SPI transfers failed to allocate internal DMA bounce buffers.
The physical panel then retained boot pixels over the product interface.

Budget queued transfers as concurrent resident payload plus metadata. The
board-specific queue cap preserves complete frames and the final callback; it
does not justify dropping chunks, bypassing TE or weakening redraw checks.
Match the intended display host and chip select so unrelated devices retain
their own policy. Keep the wrapper's exact SDK/BSP source anchor checked.

Measure both free totals and largest blocks for internal 8-bit and DMA memory.
Moving a failed allocation into PSRAM avoids a future peak; it does not reclaim
memory that was never allocated. A stopped or failed worker also cannot stand
in for a healthy concurrent workload. Charge its live resources conservatively
without claiming the old task's storage reclamation was measured.

A default-off diagnostic can still break the enabled image's resource budget.
Its static buffers remain resident after playback ends or their bytes are wiped.
Keep task-only diagnostic payloads in supported external BSS; keep callback
locks, task handles and cancellation state in suitable internal storage. Compare
linked object placement and boot-entry heap boundaries, then measure the largest
DMA block. Relocating payload bytes does not prove equal contiguous recovery.
The [local speaker tests](../../tests/local_speaker/README.md) distinguish these
placement claims from native behavior checks and physical sound.

Evidence: [display tests](../../tests/display_resources/README.md),
[board display policy](../../firmware/main/board_display_resources.c) and
[memory plan](../../design/runtime-memory.md).

## 3. Allocation capabilities and alignment are separate contracts

**Confirmed defect:** generic external 8-bit storage could provide weaker
alignment than opaque engine and adapter objects require. Invalid published
audio state also exposed initialization and snapshot-ownership failures.

Expose the required alignment alongside opaque object size. Allocate typed
storage using that requirement, and reject misalignment before dereferencing.
Check initialization results before publishing known state or dispatching work.
An absent engine is different from an initialized engine reporting a hardware
wait or fault; cleanup must preserve that distinction.

Unregistering a callback does not retract one already copied by another worker.
Retain completion ownership until that callback can no longer wake or access
freed storage. Preserve explicit memory-capability and allocation-failure policy.

Evidence: [audio engine tests](../../tests/audio_engine/README.md),
[audio owner](../../firmware/main/app_audio.c) and
[control runtime tests](../../tests/control_runtime/README.md).

## 4. Recoverable sample loss must not permanently destroy acquisition

**Confirmed defect:** an acquisition delay followed by expired cached data
permanently stopped the IMU worker. Touch input could still work, so a responsive
screen did not establish a working motion sensor.

Keep recoverable communication/freshness failures separate from structural
faults such as clock rollback, invalid configuration or invalid sample ordering.
Use bounded retries and require a new monotonic, fresh acquisition to restore
availability. Never refresh the timestamp of cached data, accept late frames or
emit catch-up movement. Reject an in-flight candidate acquired before a lost
generation; repeated consumers observing the same outage must not starve recovery.

Retain the first fault and subsequent observations. Reset motion references and
partial calibration across a gap; recovery must not silently re-enable Motion.
The single combined Bosch read preserves the real driver corrections and fixed
configured ranges. The shared worker gives IMU acquisition first opportunity and
samples the gauge at most once per second; brightness remains separate because
it can wait on the display lock.

**Still open:** recovery and continuous post-startup samples were observed, but
a startup acquisition timeout remains. Its origin is unknown. Flash/PHY work,
asynchronous audio probing and shared-bus scheduling are candidates, not proven
causes. Do not insert a fixed delay, clear history or raise the 20-ms batch and
50-ms freshness limits to turn that failure into a pass.

Evidence: [board lifecycle tests](../../tests/board_port/README.md),
[motion parser](../../tools/device_smoke.py),
[parser regressions](../../tests/test_device_smoke.py) and
[device record](../../docs/validation.md#sensor-recovery-and-unified-keyboard-candidate-2026-10-22).

## 5. Peripheral timeouts and replayed logs need precise interpretation

A per-call I2C timeout is not necessarily an end-to-end wall-clock bound: lock
waits, transaction waits, scheduling and interrupt availability may contribute
separately. Successful gauge packets can still exceed the whole-batch deadline.
Packet failure and batch lateness require different diagnostics.

Periodic boot-memory records replay stored observations. Their printing time is
not the original stage time. Likewise, repeated first-fault records can refer to
one historical event rather than new failures on every log interval. Capture
early startup or retain real stage timestamps before assigning a causal phase.
Initialization returning is not evidence that an asynchronous worker or codec
probe has completed.

Evidence: [board implementation](../../firmware/main/board_mosaico.c),
[runtime diagnostics](../../firmware/main/main.c) and
[audio board owner](../../firmware/main/board_audio_mosaico.c).

## 6. Configuration and admission gates must survive a clean build

**Confirmed defect:** an earlier local generated configuration had smaller Wi-Fi
buffers than a fresh build from tracked defaults. Resource observations from
that checkout therefore did not qualify the reproduced image.

Retain intentional limits in tracked defaults and verify effective generated
configuration, dependency pins and linked placement. Native tests can have empty
placement attributes, so they cannot prove where target storage resides. Do not
lower scan admission thresholds to disguise insufficient memory. Successful
radio initialization, LAN join and connected scanning are separate observations.

Restoring a local configuration with its old timestamp can leave generated
headers and linked objects on the previous profile even when the source config
text has changed. Force configuration regeneration after such a restore, then
verify the generated macro, compile-unit inventory and linked entrypoints.
A build identity hash of the config file alone does not prove compiled option
selection. Reject inconsistent packages before device writes and retain the
failed artifact as evidence.

Evidence: [tracked defaults](../../firmware/sdkconfig.defaults),
[pinned toolchain](../../toolchain.lock.json) and
[Wi-Fi tests](../../tests/wifi/README.md).

## 7. UI alignment must preserve interaction and idle behavior

**Confirmed regressions:** a long header label clipped after adding the Keys
entry, and repeatedly applying an unchanged font invalidated otherwise idle
frames. A visually plausible page could therefore still waste refresh work.

Use the approved layout and actual 480 x 480 LVGL renders in both themes. Check
text bounds, baselines, long host names, capture warnings and unavailable states.
Keep geometry/font setters out of unchanged-frame paths. Measure navigation
growth, live objects and idle allocation/refresh behavior without removing old
scenarios or weakening ceilings.

Test that UI actions reach the production coordinator and correct HID down/up
reports. Live host keys and Copy/Paste/Undo must leave the local draft untouched,
honor Ctrl/Command selection and reject stale, disconnected, locked or sending
actions. Layout evidence alone does not prove host receipt or physical touch.

Evidence: [UI tests](../../tests/ui/README.md),
[UI resource budget](../../design/ui-resource-budget.md) and
[interaction contract](../../docs/interaction.md).

## 8. Keep recovery, source publication and device acceptance distinct

Bind each review and test to exact source inputs and artifacts. Preserve failed
captures and negative controls. A recovered final state does not erase an earlier
fault in the same boot; a missing observation is not a zero or a success.
Legacy logs without Motion fields may remain readable but cannot qualify a new
motion-enabled image.

USB device enumeration does not prove acoustic audio; LAN readiness does not
prove authenticated services; logical UI completion does not prove physical
pixels; HID queue acceptance does not prove host input. Short healthy captures
do not satisfy the powered endurance gate. The sensor's actual stack watermark,
startup timing cause, physical host behavior and broader coexistence still need
their documented evidence.

Publish reviewed source when authorized while keeping these limits visible.
Keep credentials, recordings, factory recovery images and raw device logs in
ignored local storage; they are not part of an all-source synchronization.

## 9. Observe producer and consumer timing separately

A fresh IMU cache and a live acquisition task do not prove that the coordinator
consumes consecutive samples on time. The current interaction model rejects
consumed-sample gaps over 50 ms, invalidates its gravity reference and clears the
calibration accumulator while preserving an already established bias. Therefore
`calibrated=1` with a zero accumulator does not alone establish a broken sensor.
Reference acquisition, output suppression, generated deltas, queue admission
and host receipt need separate evidence when touch works but Motion stops.
This source-supported failure path is a diagnostic lead, not a confirmed cause
of the reported physical failure.

Keep time domains explicit. In the pinned SDK, log prefixes use a cycle-derived
base plus RTOS ticks, while application uptime and IMU timestamps use
`esp_timer_get_time()`. Do not align their numeric values directly or apply one
boot's timestamps to another boot. A delayed replay of startup observations
adds a separate distinction between event time and print time. Use retained
timestamps from the same clock before attributing a delay to a startup phase.

Evidence: [interaction model](../../components/interaction/interaction.c),
[coordinator](../../firmware/main/main.c), [SDK pin](../../toolchain.lock.json)
and the [dated device record](../../docs/validation.md#repeat-device-test-and-physical-failures-2026-10-23).

## 10. Qualify sensor axes against an explicit physical pose

A proper rotation applied equally to acceleration and gyro proves internal
frame consistency, not the sensor's mounting relative to readable screen text.
Factory bubble or Euler-angle labels are not a substitute for a documented
body basis. Ground the transform in explicit screen-up and screen-facing poses,
retain settled observations, and distinguish them from transition windows.
If the operator corrects the action sequence, withdraw its earlier pose labels
without deleting the raw observations. Do not infer vector norms or paired
sensor behavior from independent window extrema.

Correct mounting at the board boundary for both sensors. Pointer inversion,
gain tuning and repeated calibration must not compensate for an incorrect
physical basis. A common proper rotation leaves reference-estimator norms and
dot products invariant, so an axis correction and reference-loss repair require
separate evidence. See the [physical observations](../../docs/validation.md#reference-loss-and-physical-axis-observations-2026-10-23).

## 11. Reproduce the first rejected observation before changing the model

Retained first-rejection and terminal frames can establish which predicates ran
and why a reference expired. A later timing gap cannot explain an earlier
failure. Short-circuited checks are unobserved, not passed; independently retained
frames do not reconstruct missing intermediate motion. Preserve successful
parser checks separately from a user's failed pointer experience.

A coherent angular reference policy can still reject real translational
acceleration. Changing the desired input model is a product revision, not proof
that the former model was repaired. Do not hide the old failure by raising its
thresholds. See the [candidate replay](../../docs/validation.md#grip-candidate-device-result-and-causal-replay-2026-10-23).

AHRS libraries estimate orientation, not mouse intent or trustworthy velocity.
Check the pinned implementation's timing: an exposed sample-period setter may
retime integration while startup/recovery still count samples. A live producer
with a latest-value consumer does not preserve every acceleration impulse.
Offline future-endpoint correction cannot be copied into a causal input path;
inferred stillness does not distinguish every constant-speed movement. Preserve
these limits when adopting the [new model](../../docs/motion-estimation.md).
A completed benchmark or sanitizer pass is distinct from its movement criteria
passing; retain the failed cases and label privileged analytic inputs explicitly.

## 12. Keep diagnostic delivery faults out of hardware authority

A diagnostic queue may report stale samples, overflow, canceled acquisitions
and exhausted delivery identities without changing hardware capability or the
production recovery generation. Reusing a legacy read helper that latches a
hardware fault can make an observer change the failure it measures. Test both
directions: the diagnostic drain leaves source state unchanged, and the later
production read still independently applies its existing failure policy.

Fence the producer ticket before acquisition as well as queued storage. If a
discontinuity is acknowledged before an older acquisition completes, retain its
actual cause; do not relabel source loss as an intentional user fence. Accepted
publication sequence differs from hardware conversion sequence. A complete raw
export, a continuous accepted-sample segment and useful physical input are
separate claims. See the [capture contract](../../docs/motion-capture.md).

An actuation queue may intentionally discard pending samples at a source
boundary. A raw observation journal must preserve already accepted observations
and append a typed rejection event instead; otherwise diagnostics lose the data
needed to explain that boundary. Separate unavailable conversion data from
accepted publications dropped by delivery. Retain the attempt's ready bits and
timing atomically: later periodic status cannot reconstruct each rejection.
Likewise, a latest-value consumer's zero gap alarms do not prove every producer
sample was consumed. See the [live sensor API](../../docs/sensor-api.md) and
[ordered capture evidence](../../docs/validation.md#ordered-capture-deployment-and-repeated-waiting-2026-10-23).

## 13. Pairing combines bond storage, target policy and radio acknowledgement

Deleting bond records does not necessarily clear a separately persisted selected
target. Opening a pairing window may require both an acknowledged disconnect
and a completed target-policy update. Native controls must follow the same owner
contract as remote controls, including exact bond identity, revision checks,
asynchronous deletion receipts and durable policy verification. Preserve the
remaining hosts and their ordering when deleting one host. Forget all must not
silently open a new pairing window.

An accepted pairing request is not proof of advertising or host discovery.
Keep preparation, actual advertising, timeout and failure visible on the same
page. A host-side Forget action is independent from device-side stored state.
Exercise these transitions against the actual BLE owner and storage adapter;
a test double that accepts every pairing request misses policy interlocks.
See the [Bluetooth investigation](../../docs/validation.md#bluetooth-discovery-and-usb-audio-investigation-2026-10-23).

## 14. Audio implementation, connection and first frame are separate evidence

A compiled profile may be implemented without a negotiated audio connection.
When multiple profiles share one build-time support flag, an unsupported-profile
fallback test does not cover choosing between the real supported profiles.
Exercise an available A2DP speaker with unconnected HFP, both marked implemented;
preserve an already usable profile and require actual HFP capture for a microphone.
Never infer audio readiness from HID pairing or from the connection selector.

USB enumeration and host render callbacks do not prove device receipt, a complete
codec write or audible playback. A brief startup failure can fall entirely between
aggregate health reports. Keep job first-frame evidence and terminal stop reasons,
then correlate them with bounded cached transport/codec observations. Observation
must not start a stream, change its deadline, grant microphone access or expose
PCM, transcripts or peer identity. See the [audio investigation](../../docs/validation.md#bluetooth-discovery-and-usb-audio-investigation-2026-10-23).

Silent PCM can progress at the exact nominal byte rate. Use signed, varied
sample values and an explicit all-zero control when testing the handoff; include
caller-buffer erasure and asynchronous result retirement. An I2S copy completing
before the engine consumes its result is not a missing frame. Preserve both
observations in a test instead of stopping at the first completion.

An audible local tone narrows the hardware investigation but bypasses the USB
payload and application engine. Compare its transient gain with the normal
engine's acknowledged gain before drawing conclusions. Offline format conversion
and exact host-model payload checks do not establish live host output or device
sample content. Keep missing payload/level evidence explicit rather than making
speculative transport, gain or cache changes. See the
[offline follow-up](../../docs/validation.md#usb-silence-offline-follow-up-2026-10-26).

## 15. Relative input must survive ordinary transport backpressure

An in-flight USB report or pending BLE confirmation is temporary backpressure,
not a disconnected input owner. Advancing touch coordinates while dropping that
sample loses relative distance. Clipping a large displacement to the wire field
without retaining its remainder loses distance even on an otherwise idle link.
Test emitted displacement totals through the production coordinator, interaction
logic and transport completion boundaries, including contact start while busy.

Retained motion needs finite capacity and an age bound measured from the original
sample. New samples, partial sends and brief ready intervals must not renew old
motion indefinitely. Preserve button, ownership, host and release ordering; cancel
on genuine authority loss. Admission failure must leave a remote command safely
retryable or explicitly canceled, never report success for discarded movement.
Test explicit cancellation independently from normal contact end and lease
expiry: an unheld contact can still own queued movement after a partial report.
Ending a contact may drain accepted movement; canceling it must discard the
remaining movement through the owning neutral-release lifecycle.
These conservation checks do not establish physical pointer latency or smoothness.

## 16. Shared audio clocks are independent from speaker permission

On the pinned Mosaico SDK, initializing both I2S directions makes the later
initialized RX a slave of TX. The ES8311 is also a slave. Enabling RX alone can
succeed while every PCM read times out because the master is stopped. A codec
probe that prepares and closes hardware never exercises this data path. Host
substitutes must model the clock dependency rather than synthesize received
frames whenever RX is enabled.

Manage clock ownership separately from microphone and speaker grants. A clock
needed by capture must not imply speaker playback, DAC unmute or amplifier
permission. Stopping the speaker must preserve an active microphone's clock
while preventing buffered playback from resurfacing on a later start. Exercise
cold single-role startup, both duplex orderings, independent mute and failure
cleanup through the production adapter and pinned codec implementation.

A retained generic SDK error identifies a failed operation class only when its
operation is recorded. Keep the originating failure through successful cleanup
and distinguish it from subsequent cleanup errors. Host regressions establish
the lifecycle defect; physical first-frame and acoustic checks remain separate.
See the [failed device record](../../docs/validation.md#combined-candidate-device-failure-2026-10-23).

## 17. A retention gap response must permit progress with a live producer

Returning only `oldest - 1` after a journal overrun can livelock: the producer
advances before the next request, making that cursor stale again. A larger ring
or faster retry does not repair that contract. A static fixture that reconstructs
the same ring on every request cannot expose it.

Copy the missing-prefix metadata and a bounded page of retained observations
from one owner snapshot. Advance the cursor only through verified delivered
records, retain exact loss accounting, and make wire compatibility explicit.
Test a persistent journal whose producer advances between every consumer page,
including a consumer slower than production. Positive delivery, continuous
delivery and usable physical data are distinct results; do not hide loss by
jumping to the latest record or treating a completed capture as qualified data.
See the [sensor API contract](../../docs/sensor-api.md).

## 18. Decode errors in their owning API domain

Equal numeric errors need not have equal meanings. In the pinned SDK, audio
`esp_err_t` 263 means timeout, while BLE advertising completion 263 means
`ESP_BT_STATUS_HCI_MEMORY_FULL`. The host HCI command path can synthesize that
Bluetooth status when allocating a command buffer fails, before contacting the
controller. The status name alone does not establish which resource failed.

Retain the first failure together with the operation and preceding lifecycle
events. Label resource snapshots with their actual observation point; free heap
measured later cannot explain the earlier allocation. Diagnostic trace loss or
unknown observations must remain explicit. Keep tracing separate from retries,
recovery, admission policy and fault clearing. See the
[device failure record](../../docs/validation.md#combined-candidate-device-failure-2026-10-23).

## 19. Successful cleanup does not turn a failed media job into success

A playback first-frame timeout or partial capture overflow can end between
periodic global health reports. The route may subsequently report OFF/OK after
successful cleanup while its job still records the failure. Parse the retained
job result independently and keep its first observed failure across later
cleanup, empty slots and new jobs. Missing job reports remain unobserved.

Do not classify every terminal ERROR/PARTIAL state or nonzero result as a fault.
The owning engine also uses those states for explicit cancellation and normal
duration/byte limits. Match the documented reason/result combinations, retain
pending cleanup separately, and never allow an uncertain cleanup result to hide
behind a normal stop reason. Repeated snapshots count as reports, not distinct
jobs or failures when the log has no job identity. Compare the same captured
failure stream before and after a parser repair; do not remove error lines or
weaken the surrounding health gate.

Evidence: [health parser](../../tools/device_smoke.py),
[regressions](../../tests/test_device_smoke.py) and the
[device record](../../docs/validation.md#clock-repair-deployment-and-remaining-stream-failures-2026-10-23).

## 20. A protocol close callback may precede physical connection release

In the pinned Bluedroid implementation, closing GATT can notify the application
after requesting an ACL disconnect but before that connection is released.
Starting replacement advertising from that logical callback can therefore race
the old link. Trace the owning SDK call chain and serialized callback/message
execution, rather than relying on a later printed disconnect line's timestamp.
Use the public physical-disconnect operation with the observed live address,
including a privacy alias, instead of a saved bond identity.

A physical-close reservation belongs to the connection generation, ID and live
address. Authentication revision can change on that same connection; using it
to skip an already reserved close can strand the reservation forever. Cover
authentication completion between reservation and submission, immediate SDK
rejection, and a replacement link reusing the old ID/address. API enqueue
acceptance is not physical completion: if the later callback never arrives,
preserve the pending/failure state and do not start replacement advertising.
See the [Bluetooth lifecycle regressions](../../tests/ble_hid/README.md).

## 21. Test stream progress and capacity with the actual class driver

A FIFO substitute can miss a class-driver stall. The pinned TinyUSB DMA OUT
path verifies a positive FIFO write before rearming reception; a valid empty
packet returns zero from that write and leaves the endpoint unarmed. An empty
packet must preserve receive progress without fabricating a PCM frame. Keep
real positive-write and endpoint-submission failures observable.

A requested packet size is not the number of bytes a FIFO actually returns.
Flow-control blackout can select a nominal packet larger than the finite tail;
schedule only the bytes read, never stale buffer contents. Test a single finite
frame and subsequent empty completions as well as sustained multi-frame traffic.

The pinned upstream driver configures its IN FIFO to overwrite. A successful write
therefore need not mean either preserved queued data or transmission to a host.
Exercise the actual FIFO and completion callbacks with distinct byte content,
short and zero writes, a stalled consumer and resumed consumption. Account for
application queues, FIFO contents, scheduled transfers and completed transfers
separately; include tails that flow control has not yet scheduled. Do not infer
host delivery from accepted bytes, or infer error-path conservation when a
driver removes bytes before a failed endpoint submission. These checks do not
establish acoustic quality or all-direction lossless transport.

Evidence: [USB audio regressions](../../tests/usb_audio/README.md) and the
[reviewed upstream patch manifest](../../patches/tinyusb-audio-streams.json).

## 22. Authentication failure codes can originate in local policy

Decode a raw status in its owning SDK domain and trace every producer before
assigning a cause to the remote host. In the pinned Bluedroid path, rejecting
`esp_ble_gap_security_rsp` can produce `SMP_REPEATED_ATTEMPTS`, which BTA converts
to decimal 86. A peer failure can produce the same final code. It is not enough
to infer a host cooldown, incompatible security mode or corrupted stored keys.

Pairing admission is decided for a particular physical connection and can
outlive the visible pairing window. Observe the original connection decision,
the worker's current generation and admission predicates, and the eventual
authentication result separately. A queued security response is not an
authentication outcome. Keep diagnostics bounded and behavior-neutral, without
key material or raw peer identifiers; preserve the distinction between current
SDK security records and durable storage.

Evidence: the pinned SDK call chain and
[deployment record](../../docs/validation.md#stream-repair-deployment-and-remaining-transport-failures-2026-10-23).

## 23. Controller submission is not a completed USB transaction

A passing audio-class/FIFO test does not exercise controller scheduling when
its endpoint boundary immediately accepts every transfer. The pinned DWC2
driver programs the opposite current frame parity for an isochronous rearm.
At high speed, an eight-microframe host interval retains the same parity; a
phase mismatch can leave an accepted OUT request pending. A register-boundary
test can establish this programmed condition, but its model of bus behavior
remains an assumption until confirmed on the device.

Keep the production descriptor, packet contents and positive progress oracle
identical when comparing a baseline with a candidate. Derive the host interval
from that descriptor. Retain the expected baseline failure instead of changing
the success criteria for the old mode. Cover feedback, capture, failed or empty
completions and endpoint replacement, as well as successful playback.

Count submission, rearm acceptance and successful completion separately. A
class callback may discard the completion result or run only after rearming,
so its count cannot substitute for a successful bus transaction. Observe
effective controller mode rather than inferring it from a compile flag. Read
registers only in a valid powered context, identify the sampling point and
epoch, and avoid destructive status reads. Keep mode changes and reset-driven
session invalidation in the owning lifecycle path; collecting diagnostics
must not perform either action.

Evidence: the pinned DWC2 source and
[USB audio regression contract](../../tests/usb_audio/README.md).

## 24. A pending role must not stall an independent active role

A newly requested microphone can wait for display permission without owning a
codec command. Returning before servicing an already active speaker during that
wait stops playback consumption while transport ingress continues. Native
regressions reproduced playback overflow on the seventeenth engine frame.
Continue the existing role only when no real codec operation or safety closure
is pending; do not bypass microphone permission or serialize unrelated waits.

Backpressure must extend through every queue boundary. Checking manager ingress
capacity does not establish space in the engine ring. Its serialized owner must
check downstream credit before relinquishing the ingress frame. Preserve exact
sequence, payload and cancellation ownership across a full backlog; do not
enlarge queues, discard frames or weaken terminal overflow checks to hide a
missing capacity boundary. Capacity is storage information, not authorization.

Keep copied permission context separate from audio-owner availability. A known
display observation must not turn a missing audio snapshot into an apparent
capture-off result or generation zero. Diagnostics reuse the coordinator's
existing observations and label missing data and unknown age explicitly.

Evidence: [engine regressions](../../tests/audio_engine/README.md),
[observer regressions](../../tests/audio_diagnostics/README.md) and the
[bounded deployment investigation](../../docs/validation.md#usb-transport-deployment-and-capture-startup-investigation-2026-10-26).
The native fixes do not establish the device's actual indicator failure cause
or acoustic behavior.

## 25. Preserve completion evidence before its producer replaces it

A status query must not be the only opportunity to retain a completed display
transfer. The LVGL owner already reads the Board receipt before starting the
next refresh. If it ignores that proof and the next begin clears the records,
an empty refresh can erase the first confirmation before a slower observer
reads it. Tests that always query status immediately after completion conceal
this ordering.

Exercise complete transfer, delayed observation and intervening empty refresh
as one sequence. Preserve the original completion time and refresh identity;
empty work is not new evidence. Shared owner/status validation must retain all
coverage, token, area, error and revocation checks, and stale copied observations
must not replace newer proof. Software reproduction establishes this retention
defect, not the schedule that caused a particular physical timeout.

Evidence: [presenter checks](../../tests/presenter/README.md) and
[device observation](../../docs/validation.md#capture-indicator-confirmation-failure-2026-10-26).

## 26. Separate audio content from transfer counts and requested gain

A sustained PCM byte rate can carry silence. To localize a silent-output report,
observe nonzero counts and absolute peaks at separate owning boundaries, then
compare generation-tagged observations. Count only the valid prefix reported
by a synchronous write; a successful report is not DMA consumption or analog
output. Preserve unknown observations and failed/invalid-prefix totals.

Reuse existing gain and mute read acknowledgements. A requested slider value
or a transient local-test gain does not establish the normal engine's actual
setting. Use bounded metadata in external memory and scan only task-owned
copies outside interrupt locks; do not retain recordings or add getter I/O.
A later audible test without matching earlier measurements cannot retrospectively
prove why an earlier attempt was silent.

A live microphone meter needs both capture permission and a fresh, matching
generation. Zero PCM is measured silence; no samples, failed/partial reads and
expired windows are different states. Invalidate retained levels on mute, stop
and replacement, and recheck ownership before publishing a completed read.
Keep meter observations out of action revisions so continuous telemetry cannot
invalidate an already held Stop or Mute. A worker can publish after the main
loop sampled its time; compare freshness against a monotonic read taken after
copying the observation, rather than inventing a future-time tolerance.

Reusing a static LVGL label avoids text-buffer churn but does not imply zero
allocation during visible text changes: the pinned public label setter queues
a short-lived layout event. Bound that transient work and require stable live
allocations after layout; unchanged and hidden snapshots must still do no work.
This does not qualify target memory headroom or microphone sensitivity.

Evidence: [PCM observer checks](../../tests/board_audio/README.md),
[USB observer checks](../../tests/usb_audio/README.md), and
[bounded playback evidence](../../docs/validation.md#audible-usb-playback-and-startup-control-churn-2026-10-26).

## 27. Reserve control progress in a continuously busy audio mailbox

An asynchronous PCM transfer can finish successfully on every iteration while
a control request never gets a turn. In the production worker, retrying volume
before the progress pump was insufficient: each pump retired the old write and
queued the next one before the next control retry. The unchanged command
deadline then stopped a healthy stream. Reserve the control turn, retire the
exact pending I/O or unmute request, and defer new PCM until that control
completes. Safety cancellation and generation checks still take precedence.

Exercise the actual manager and engine together with an asynchronous codec and
continuous ingress; a synchronous engine-only codec hides this interleaving.
Check ordered PCM content, bounded control completion and privacy interruption,
not just successful enqueue or increasing byte counts. Queue acceptance is
pending, not completion. Use the product's failure-only notice policy: progress belongs in button/job
state, and successful results remain silent. Propagate notice removal to the
coordinator's global status without erasing messages from another owner.

Evidence: [worker/engine regressions](../../tests/audio_engine/README.md),
[control regressions](../../tests/media_controls/README.md), and
[device failure record](../../docs/validation.md#remaining-live-control-timeout-after-usb-readiness-repair-2026-10-26).

## 28. Separate FIFO admission from USB packet cadence

One packet-sized write followed by a nominal one-millisecond task delay does
not sustain one packet per millisecond after scheduler and processing overhead.
A continuous capture stream can fill its fixed application queue even while
USB completion counters keep advancing. Let each worker pass admit a bounded
batch to the finite FIFO, stopping on short/zero writes and rechecking lifecycle
identity between chunks. Keep packet limits, suffix ownership, queue capacity,
age rejection and privacy invalidation unchanged.

Use the real pinned class/FIFO with a continuous producer, bus consumption and
missed worker wakes. Compare every byte in completed packets, in-flight packets,
the FIFO and the application queue; successful enqueue counts alone are not
conservation. Preserve low-fill and finite-tail oracles when improved batching
changes which scheduling gaps reach those branches. Such tests establish a
throughput defect and repair, not the cause of audible crackle or a UI freeze.

Evidence: [USB audio regressions](../../tests/usb_audio/README.md) and
[duplex failure evidence](../../docs/validation.md#failure-only-notice-deployment-and-usb-duplex-failure-2026-10-26).

## 29. Separate shared-bus admission from peripheral transfer failure

Preserve the original SDK error before translating it into a public result.
Trace that error through the exact pinned driver and active configuration:
a synchronous transaction can time out acquiring a shared bus before sending
any register bytes. The error alone does not identify the competing peer or
prove an electrical failure. Tick conversion must also be verified rather than
assuming that a short millisecond budget becomes zero.

Give muted initialization a finite budget consistent with shared-bus peers,
without extending runtime or safety operations. Vendor initialization may keep
calling control callbacks after a failure; latch failure/cancellation before
admitting another wait, restore the short budget before cleanup and retain
uncertain teardown. Test the old and repaired production sources with the same
finite-contention fixture, plus exhaustion, transfer errors and cancellation.
An SDK timeout argument is not necessarily a whole-operation latency bound.

Keep the one-time muted boot obligation distinct from a cancellable user-job
prepare. A synthetic cancellation inside a vendor enable operation can leave
an acquired reference without the matching enabled state, despite zero SDK
errors. Exercise initial engine stop during the real constructor, configuration,
enable and mute callbacks, with time advancing while the call is pending.
Retain the probe's original short budget and acknowledge queued safety work
only after real teardown. Do not erase a genuine transfer failure, guess the
vendor's private references or widen the safety deadline to hide the race.

For a reported control flicker, observe the existing presenter's permission and
blocker transitions alongside transport identity. Do not add a second readiness
read or claim a copied candidate was displayed. Distinguish initial negotiation
from a later persistent fault; changing labels or hiding disabled states does
not repair either cause.

When no-host-stream and unsettled-host-stream observations both deny Start,
present them as one truthful waiting category with stable host instructions.
Calling one transient phase unavailable creates visible flicker without adding
useful guidance. Keep the diagnostic reasons distinct, Ready tied to actual
permission, and real disconnect, faults and active controls immediately visible.

Evidence: [codec budget regressions](../../tests/board_audio/README.md),
[admission observer regressions](../../tests/audio_diagnostics/README.md) and
[initialization investigation](../../docs/validation.md#bounded-codec-initialization-admission-2026-10-26).

## 30. Scope frame sequences to their immutable consumer

One audio resource can survive multiple Mic jobs while Speaker stays active.
A replacement capture consumer starts its own sequence space; comparing its
first frame with the previous consumer's last sequence falsely rejects valid
data. A full resource restart can hide this defect by clearing the shared
counter, so successful cold starts and later recovery are insufficient.

Validate the complete resource/link identity before changing consumer state.
Honor the producer's monotonic consumer generation, reject older consumers even
when their frame sequence is larger, and retain duplicate rejection within a
consumer. A new capture consumer needs a bounded input-only queue/FIFO barrier;
do not flush Speaker or treat unfinished cleanup as completed admission. Keep
replacement, partial writes, delayed callbacks and privacy invalidation in the
same lifecycle model.

Reproduce the stop/restart through the actual engine and transport adapter while
the opposite role keeps the resource alive. Prove the old production path fails
the same test, then check ordered new-consumer data and stale-frame rejection.
Do not change the engine's legitimate sequence reset to satisfy the adapter.

Evidence: [USB audio regressions](../../tests/usb_audio/README.md) and
[Mic restart device failure](../../docs/validation.md#usb-mic-restart-failure-during-continuous-speaker-playback-2026-10-26).

## 31. Separate current-host release from the next-host neutral barrier

An input engine can deliberately retain `release_pending` after disconnect so
the next host receives neutral reports before fresh input. No disconnected host
can acknowledge those reports. Waiting for that flag before pairing, editing a
saved target or deleting bonds creates a circular dependency that an unrelated
USB connection can appear to resolve.

Wait for release and transport completion while the affected Bluetooth host is
connected. After observed link-down, continue the approved management operation
without clearing the next-host barrier or claiming release delivery. Pending
USB input releases belong to USB. Keep generation checks, exact reviewed bond
identity, durable policy receipts, cancellation and transport-switch exclusion.

Exercise both initially disconnected and connected-then-disconnected cases with
the barrier still set, including Add, Forget all and selected/unselected single
host deletion. Keep connected-release/congestion negative cases. Use the real
BLE worker and storage boundary to verify removal and policy reconciliation;
SDK submission alone is not deletion proof.

Evidence: [BLE regressions](../../tests/ble_hid/README.md),
[connectivity controls](../../tests/connectivity_controls/README.md), and
[management validation](../../docs/validation.md#bluetooth-management-without-a-usb-release-dependency-2026-11-03).

## 32. Match asynchronous reads to the verified transport lifecycle

A coordinator-owned read can legitimately return busy on first admission while
its owner prepares a short-lived snapshot. A client that reconnects TLS for
every poll can repeatedly miss that freshness window, even with a healthy LAN.
Do not extend snapshot age or turn busy into success to compensate for transport
cost. Reuse one verified connection per logical controller and poll only exact
busy GET results within finite bounds. Check the certificate again whenever a
new connection is established. Keep mutation delivery and unknown outcomes out
of automatic retry, and close the transport with the owning session.

Separate Wi-Fi association, configured trust, listener readiness, authenticated
reads and actual update completion. Missing initial trust explains an absent
listener but does not prove a failed radio. Preserve incomplete/unsupported
diagnostic reads in local receipts; status collection is not device-health or
rollback qualification.

Evidence: [companion tests](../../tests/control_protocol/test_bridge.py),
[LAN collector tests](../../tests/test_lan_debug.py), and
[collector contract](../../tools/lan_debug.md).
