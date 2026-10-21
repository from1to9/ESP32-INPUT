# Canonical remote-input regressions

Run `python3 tests/remote_input/run.py`. It compiles the production coordinator
adapter with the actual input core, HID engine, interaction model, canonical
registry, argument helpers and protocol sources under address/undefined
sanitizers. Only coordinator observations, action delegates, result publication
and transport acceptance are controlled by the harness; there is no network,
device, alternate HID model or UI framework.

Cases cover default/explicit lease producers, acquire neutral barriers, renew,
expiry, scope/session/lease/epoch/target/UI gates, shared mouse/key/shortcut
reports, held state and reserved release, drag deadmen, atomic preview edits,
selection/group semantics, 64-character multi-batch preview send, exact started
and completed-pair accounting, failed key-up without replay, virtual gesture
identity/tap/drag/cancel/timeout, completion backpressure and bounded ordinary
capacity with an independent safety lane. Neutralization remains admissible
beyond ordinary deadlines/provider loss while old target/epoch work cannot
cancel input on a replacement target. Renewal preserves virtual contact; late
remote revocation preserves fresh local contact. Authorized mode transitions
settle old input and preserve the lease after the new neutral barrier.
Submode transitions also preserve the lease and cancel prior partial text.
Operation ownership is independent of runtime availability: foreign device,
media and lock commands leave input replay/lease state untouched, while an
unavailable owned UI operation fails within the input adapter.

The same shared source-aware entry points drive remote model/motion behavior.
Native wrappers continue to require the local source. Hardware sensor behavior,
LAN/radio latency, host delivery, actual UI hit testing and the parent's provider
composition require their own validation; a queue admission or transport-accepted
report is never described as an observed host effect.

Producer regressions call `app_remote_input_motion_sample` with the actual shared
sample path: DEVICE_IMU produces remote-owned Mouse reports, SELECTION changes
local grid focus/revisions without host reports, and DEVICE_TOUCH accepts
virtual touchpad movement without claiming physical contact. A virtual pad tap
clutches a live IMU stream past the ordinary settling interval and still emits
its normal click. Physical contact retains the parent's immediate local
priority. The coordinator must call the routing API once and skip its old
motion-model/enqueue path when the sample is consumed.
