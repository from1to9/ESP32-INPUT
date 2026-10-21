# Spoken letters

`spoken_letters.c` implements one bounded explicit listening window over an
audio-manager recognition job. It accepts only local mono PCM16LE at 16 kHz,
10/20 ms frames with matching job/audio/consumer generations and continuous
sequence/sample offsets. Old jobs cannot affect replacements. Detection creates
an ASCII candidate; a matching local confirmation consumes it once into the
caller's preview path. This component never sends HID or opens a codec.

The [firmware runtime](../../firmware/main/app_spoken_letters.c) owns one speech worker and eight copied frames in PSRAM.
The manager's capture sink calls `app_spoken_letters_capture` only for an
authorized recognition job; bind the actual job after the manager acknowledges
start. AE_AGAIN retains data only in the manager ring, not an unbounded queue.
Stop that audio job when the speech snapshot becomes terminal. Its `capture_cancel`
callback calls `app_spoken_letters_capture_closed` with the exact job and consumer
generation: PCM ingress/queue close while an already published, unexpired
candidate remains available for confirmation. Closure during recognition cancels
that window and suppresses an in-flight result. Privacy/lock/mode/owner/UI
changes additionally call strong `app_spoken_letters_cancel`, which clears the
candidate even after normal capture closure. Cancellation closes
ingress synchronously and suppresses in-flight candidates. `cleanup_pending`
remains visible until the worker resets the model and clears queued PCM.
Explicit dismissal also strongly cancels the binding. Every new listening
window requires a new manager job and consumer binding; a UI token cannot reopen
retained audio from an old capture job.
Confirmation requires current ownership/context and available preview capacity
before calling `app_spoken_letters_confirm`.

The backend calls official ESP-SR AFE and MultiNet APIs. It uses the component's
`model` partition and exactly `mn7_en`, clears the sample grammar, installs
Alpha-Zulu plus zero-nine and checks the command update result. It queries
actual PCM/chunk dimensions and fails closed on mismatch or missing assets.
WakeNet is disabled for the explicit listening window; AEC is disabled because
simultaneous playback has no qualified reference. The audio manager must admit
local recognition only under its half-duplex resource policy.

Upstream revision: `78a923d29a45e4e1433b909d3da12ab5b0f994fc` (ESP-SR 2.6.0).
The parent dependency/partition owner must pin that revision, enable the English
MultiNet7 model, include the AFE's configured assets and reserve measured model
storage. See [official upstream](https://github.com/espressif/esp-sr/tree/78a923d29a45e4e1433b909d3da12ab5b0f994fc)
and its [Espressif MIT license](https://github.com/espressif/esp-sr/blob/78a923d29a45e4e1433b909d3da12ab5b0f994fc/LICENSE),
whose notices remain with the dependency. No third-party implementation is copied.

The native detector's threshold is retained. Exact ties between different
commands are rejected; native probabilities and runner-up scores are exposed
without claiming calibrated confidence. Acoustic accuracy and probability/margin
tuning remain unqualified pending measurements. No direct single-letter aliases
are installed. Host policy tests and a target build do not establish recognition
accuracy, microphone quality or processing throughput on hardware.

The selected build settings are `CONFIG_MODEL_IN_FLASH=y`,
`CONFIG_AFE_INTERFACE_V1=y`, `CONFIG_SR_NSN_WEBRTC=y`,
`CONFIG_SR_VADN_WEBRTC=y`, `CONFIG_SR_MN_CN_NONE=y` and
`CONFIG_SR_MN_EN_MULTINET7_QUANT=y`. No WakeNet selection is required. The SDK
packs `srmodels/srmodels.bin` beneath the firmware build directory. The pinned
upstream model pack with these selections is 2,761,093 bytes including the
English grammar support files, fitting a 3 MiB model partition. The generated
build configuration must actually select MultiNet7; defaults do not override an
existing choice of None.
