# Board adapters and porting

The application talks to [board_port.h](../firmware/main/board_port.h).
[board_mosaico.c](../firmware/main/board_mosaico.c) is the Mosaico implementation;
GPIOs, power rails, revision detection, BSP handles, motor drivers and physical
button registration remain inside that adapter and the pinned official BSP.
Do not add board pin overrides to application orchestration or UI code.
Product behavior remains defined by [requirements.md](requirements.md).

## Current initialization contract

Call `board_init(lock_callback)` once, before the other board APIs and before
starting or advertising BLE. A failed attempt is not retryable in the same boot:
the BSP can retain partial display resources, so recover by correcting the fault
and rebooting. Do not advertise a usable input device after initialization fails.

| Capability | Required for this UI | Mosaico evidence |
| --- | --- | --- |
| Board identity | Yes | BSP eFuse revision detection; only v1.0/v1.2 accepted |
| Display | Yes | BSP returns an LVGL display and its startup lock succeeds |
| Touch | Yes | BSP returns a registered LVGL touch input device |
| Physical lock | Yes | AI button creation and physical edge callback registration succeed |
| IMU | No | Official BSP starts a 100 Hz stream; cached finite samples pass freshness checks |
| Haptics | No | PWM motor initialization, verified off and worker allocation succeed |
| Battery telemetry | No | Checked BQ27220 register reads complete and gauge initialization is ready |
| Brightness | No | Initialized CO5300 and the board power worker are available; applied level is separately acknowledged |

The BSP can continue display initialization after a touch failure. This adapter
checks the touch result independently and fails initialization if absent. A
successful button registration proves software setup, not physical switch
operation; qualify the switch on the actual board. The IMU worker owns sensor acquisition and publishes gyro degrees/second, acceleration in g and a monotonic completion timestamp. Gyro and acceleration use separate BSP reads within one bounded acquisition batch. Duplicate timestamps must be skipped. Samples older than 50 ms, non-finite values, acquisition errors or a batch exceeding 20 ms fail closed and drop capability until reboot. The adapter does not calibrate axes or infer pointer correctness; those are portable model and physical qualification responsibilities.

Register `board_button_events_register()` before `board_init()` for timestamped AI down/up events. Raw mode suppresses the legacy long-press callback; the coordinator alone classifies a 280 ms single/double-click window and a 900 ms hold. The fallback adapter hold is also 900 ms. Touchscreen controls must not synthesize a trusted physical unlock event. Partial button sequences after queue overflow are discarded, and a new complete physical hold is required after release. Preserve this physical recovery path on every supported board.

The display boundary intentionally exposes `lv_display_t` because the current UI
uses LVGL. `board_display_lock()` uses milliseconds, with `UINT32_MAX` meaning an
indefinite wait; other values above `INT32_MAX` are rejected. Match unlock calls
to successful lock calls. A headless board requires a separately designed UI
backend and its own safety/recovery controls; setting `display=false` does not
make the current application headless.

## Haptics and asynchronous ownership

The [portable haptic policy](../components/haptics/README.md) owns patterns,
cooldown, cancellation and failure transitions. It has one writer: a dedicated
Mosaico FreeRTOS worker at idle priority + 1, with a 3072-byte stack and no dynamic
event allocation. Task creation allocates the task/stack once at initialization;
there is no allocation per event. The worker advances on notifications or a
10 ms wait deadline. Stack margin and real scheduling latency require hardware
measurement.

The board interface is task-context only. Callers publish at most one pending
event in a short critical section; they do not wait for motor pulses or write
the motor. Additional pending requests are dropped. Events older than 100 ms are
dropped on dispatch. Disable immediately invalidates pending work and wakes the
worker to stop the motor; the physical off write happens at its next scheduling
opportunity. A disable/enable pair also cancels the old pattern, even if both
controls arrive before the worker runs. Re-enabling does not replay old work.
This is scheduling-dependent cancellation, not a hardware emergency cutoff.

The shared dispatch lock covers both control-generation changes and each
nonzero generation check plus the complete hardware write. There is no unlocked
gap between permission and dispatch, including the second error pulse. A write
that already owns the lock commits before cancellation; cancellation that owns
the lock first rejects that old write, even after re-enabling. The portable
dispatcher distinguishes this rejection from motor failure and always permits
off writes. Generation exhaustion fails closed without wrapping.

Only the bounded initialized motor fast path runs under the dispatch critical
section. The pinned S31 LEDC driver has no duty-update delay, and no fade is
registered on the BSP's exclusively owned motor channel. A compile-time check
rejects a PWM target without `SOC_LEDC_SUPPORT_FADE_STOP`. Motor initialization,
resource allocation, policy execution and adapter logs stay outside the lock.
Do not add runtime fades or share/reconfigure the motor channel; doing so would
invalidate the bounded dispatch contract and requires redesigning this boundary.

Feedback is session-only, enabled by default when available, and can be switched
off globally. There is no NVS dependency. Emit confirm, mode, lock and error
events for discrete local actions. Do not emit movement, per-report or held-drag
feedback. The adapter requires `CONFIG_BSP_MOTOR_ENABLE_PWM=y`; the BSP's
GPIO-only fallback cannot provide mild strength and is reported unavailable.
Persistent motor write failures latch haptics unavailable until reboot. The
worker keeps attempting off at the policy's bounded interval while off is
unconfirmed, with logs only on a failure edge. Motor initialization or worker
allocation failures attempt an off write and report unavailable; an unconfirmed
stop cannot be guaranteed by software after driver/resource failure.

Use `board_haptics_active()` in the motion-output gate, alongside touch/menu,
lock, transport and mode gates. It includes pending feedback, active output and
unconfirmed stop attempts. Flush motion integration while suppressed to avoid
replaying vibration-induced movement. The production coordinator applies this gate to the actual IMU stream and to local motion selection.

## Battery telemetry and brightness

`board_power_read()` is a nonblocking cached snapshot. It always fills the
result, including unknown state and invalid measurement flags on failure.
Successful register communication is distinct from a connected cell: inspect
`battery_present` rather than treating the `battery` capability as presence.
The capability represents usable gauge telemetry. A gauge can answer while its
initialization is incomplete; that snapshot reports `gauge_available=true`, an
error and unknown battery state until initialization completes.

The Mosaico has a BQ27220 on the official BSP's shared I2C bus and address.
The adapter uses ESP-IDF checked transactions because the pinned BSP battery
wrapper's scalar getters can convert transport failures into zero values and
then return success. It reads the standard operation-status, battery-status,
voltage, signed-current, temperature and state-of-charge commands. There are no
profile, calibration, reset or gauge-control writes. In particular, it does not
apply the BSP's interim 80 mAh profile to an unqualified connected cell.

| Reading | Interpretation |
| --- | --- |
| Battery presence | Gauge `BATTPRES` flag, only after gauge initialization completes |
| Percentage | Gauge-reported estimate in 0..100 using its existing profile; no voltage lookup or claimed independent calibration |
| Charging | Positive measured battery current is yes; negative is no; zero, absent or unavailable is unknown |
| Full | Gauge `FC` flag for a present battery; this is not independent charger-termination evidence |
| External power | Unknown: this BSP exposes no independent VBUS or charger-present signal |
| Voltage/current/temperature | Checked gauge readings with explicit per-field validity; invalid percentage data is not truncated or clamped |

The USB host's enumeration/mount state must not be used to infer that external
power is absent. Screen policy must not depend on proving a plugged-in state
that this adapter cannot observe. A valid 0% estimate, no cell, an uninitialized
gauge and a failed read remain distinct results.

A single low-priority worker polls the gauge at most once per second. Each I2C
call has a 3 ms timeout; the complete batch has a 20 ms acceptance budget.
Timeouts include scheduling and bus contention in the observed batch deadline.
The worker releases the bus after each checked word and does not hold a critical
section across I2C. The 100 Hz IMU worker has a higher priority. Late scheduling
does not cause catch-up polls. After five seconds without a fresh result the
cached readings become unknown. Transient read failures can recover on a later
successful poll. Bus/device allocation failures remain unavailable until reboot.
Device measurements must still qualify real bus contention, sample jitter and
the gauge's existing cell profile.

`board_brightness_set(0..100)` queues the latest request for that same worker;
`ESP_OK` means accepted/coalesced, not a completed panel write. Inspect
`board_brightness_status()` for pending state, driver error and the last accepted
applied level. Repeated values coalesce, changes are limited to one hardware
attempt per 20 ms, and changing brightness does not trigger extra gauge polls.
The worker takes the display lock for a bounded 100 ms wait before calling the
official BSP brightness API. A failed write leaves the applied level unknown;
an explicit retry of the same value is accepted. Initial brightness is unknown
until the first successful command, preserving the retained boot display value.

Zero writes CO5300 brightness register 0x51 to zero; a value in 1..100 restores
illumination. This path does not call display sleep/deep standby, pause LVGL,
disable touch or suspend Bluetooth/coordinator work. It claims panel driver
acceptance, not measured luminance or a full power cutoff. Timeout, wake,
settings persistence and restore-level policy belong to the coordinator.

The worker allocates a 3072-byte stack once at boot, at idle priority + 1. It has
one pending brightness value and no per-event allocations. If worker allocation
fails, both monitored telemetry and brightness requests report unavailable;
device-handle cleanup failures preserve the handle and an error rather than
reporting successful cleanup. Qualify stack margin and powered operation on the
actual device. The board implementation directly uses `esp_driver_i2c` in
addition to the BSP, `esp_timer`, FreeRTOS and existing components.

## Adding another board

1. Keep the public board interface stable and add one adapter source that owns
   the new board's BSP, pins, revision identification and resource lifetime.
   Select exactly one adapter in `firmware/main/CMakeLists.txt`. Include the
   portable `haptics` component through `firmware/CMakeLists.txt` and add it to the
   main component requirements, alongside that board's BSP and needed ESP-IDF
   components. The Mosaico implementation directly uses `esp_timer`, FreeRTOS,
   the button component and the BSP's LVGL backend; declare those dependencies
   in the firmware integration rather than importing them into portable policy.
2. Establish the required display/touch and trusted physical lock callback
   before transport startup. Propagate failures and reject unknown revisions.
   Detect optional hardware from successful initialization/probes; never return
   a fabricated success or infer availability from a product label.
3. Reuse the portable haptic policy with a bounded strength/off callback when a
   motor is present. Serialize all motor writes, preserve cancellation and
   backpressure, and expose failed capability status. If no motor exists, return
   disabled/unavailable and do not queue feedback for later replay.
4. Pin and document the SDK, BSP, target and configuration independently. Review
   PWM timers/channels, USB PHY, display buses, memory, radio and audio resource
   ownership. Do not reuse Mosaico pin definitions on another revision or board.
5. Run host regressions, compile for the actual target, then qualify each board
   and host pairing with recorded versions. Passing the portable tests does not
   establish electrical, timing, USB, radio, touch or host compatibility.

Bluetooth capabilities vary across ESP32 variants. Qualify BLE HID separately
from Classic Bluetooth HFP/A2DP and from USB UAC/HID composition. A board adapter
or BLE-capable part does not promise Classic audio support. Do not advertise an
audio profile until the target radio, software stack and host combination have
been proven to support it.

## Independent qualification

Host regressions cover pulse timing, late scheduling, 100,000 requests across the
32-bit millisecond boundary, cooldown/flood suppression, active/gap cancellation,
unsupported hardware, failed on/off writes, stop retries, explicit recovery,
clock rollback and the `UINT64_MAX` boundary. Run `python3 tools/check.py` for
portable tests and `python3 tools/check_repository.py` for text/link hygiene.
Deterministic dispatch interleavings also cover cancellation immediately before
the first pulse and second error pulse, disable/enable with an old worker token,
a motor write that commits before a competing control request, driver failure
through the gate and control-generation exhaustion. The driver fixture asserts
that hardware access remains inside the same lock as the generation check.

Record target build evidence separately from device tests. On each physical
revision test required-hardware failure before BLE, a failed/missing optional
motor and IMU, physical lock/unlock under load, switch bounce and a held button,
touch operation, motor stop on disable, rapid disable/enable, repeated error
events, worker stack margin, motion suppression and actual off latency. For
continuous powered operation, also measure temperature, resource usage, idle
behavior and a long fault/reconnect run; host event-count tests are not a
hardware endurance result. Never qualify another ESP32 board by inference from
Mosaico behavior.

The pinned BSP does not implement `BSP_USB_CONSOLE`, `BSP_USB_CONSOLE_AUTO_INIT`
or `BSP_USB_AUTO_DOWNLOAD`. Do not add those symbols as if they enabled runtime
CDC. Runtime USB console support belongs to a separate USB owner, using the
actual supported USB device component and descriptors. Preserve manual BOOT
recovery, and qualify runtime console enumeration independently of ROM download
enumeration and of any later USB HID/audio composition.
