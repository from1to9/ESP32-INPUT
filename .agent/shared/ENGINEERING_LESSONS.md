---
scope: repository-shared
portability: cross-checkout
status: active
verified_at: 2026-10-23
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
