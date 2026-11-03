# Bluetooth audio transport

The ESP32-S31 transport is implemented in `firmware/main/bt_audio.c` with the
public contract in `firmware/main/bt_audio.h`. It uses the pinned ESP-IDF commit
`383dda6c182e3dabf6355160166fe007df775629`, Bluedroid HFP HF over HCI and A2DP
Sink with the SDK's internal codecs. HFP CVSD is mono PCM16LE at 8 kHz; mSBC is
mono PCM16LE at 16 kHz. LC3 and external-codec modes are unavailable. A2DP uses
actual decoded SBC mono/stereo at 16/32/44.1/48 kHz, explicitly downmixing stereo
by arithmetic mean into mono. There is no transport resampling or acoustic AEC.

## Shared lifecycle and ownership

The application must initialize NVS, enable one BTDM controller and one
Bluedroid instance before calling `bt_audio_init`. Initialization success means
SDK submission only; both profile success events are required for readiness.
Missing events expire after five seconds. Partial initialization retains its
resources and fails closed until the complete shared stack is reset/rebooted.
The transport never initializes/enables/disables those singletons, opens I2S or
a codec, changes a device name, automatically pairs or starts capture.
`ble_hid_init` is the current shared lifecycle owner: it enables BTDM when
Classic is configured, and retains BLE mode for a BLE-only image. The audio
module owns Classic GAP/security and bounded discoverability. Its device name
comes from asynchronous Classic GAP cache readback at initialization and each
explicit pairing window. Read failure/timeout leaves the name unavailable and
does not fail audio or prevent closing pairing. The application currently has no
Classic name writer: the Classic name can differ from the separately configured
HID name. Neither a GAP name nor discovery cache observation proves radio receipt.
BLE HID readiness is unrelated to either Classic profile's readiness.

The shared owner must enable Classic A2DP, HFP client, HFP HCI audio and WBS,
keep internal SBC/mSBC codecs, disable LC3/external-codec paths, and reserve one
BR/EDR ACL and one SCO connection alongside LE HID. ESP32-S31's HFP example
requires HCI voice data. A BLE-only configuration returns NOT_SUPPORTED; it
cannot masquerade as implemented audio. Enabled controller status does not
prove its runtime mode; BTDM is the shared owner's explicit precondition.

[The unapplied BLE integration proposal](bt-audio-shared-stack.patch) adds a
shared-stack entry point if the application later moves singleton setup out of
BLE HID. It is an optional unapplied proposal, not required by the current
BTDM lifecycle. Its zero-context patch can be inspected with
`git apply --unidiff-zero --check docs/bt-audio-shared-stack.patch`.
The audio module must not initialize a second controller or Bluedroid instance.

## Bounded Classic pairing

`bt_audio_pairing_open(ms)` accepts an explicit window up to 300 seconds;
`bt_audio_pairing_close()` or expiry immediately denies new SSP admission.
The coordinator closes LE pairing first. An atomic guard rejects simultaneous
LE/Classic handshakes, and fresh capacity checks use the SDK's exact shared
address-section count before granting security. Public Classic/LE bond lists
omit incomplete address sections and cannot prove spare capacity themselves.
`bt_bond_guard.h` declares this narrowly pinned internal SDK dependency.
No existing bond is deleted or evicted by the adapter.

Classic discovery starts closed. The service task applies bounded discovery
and checks the SDK GAP cache; `discovery_known` explicitly does not mean radio
readback. Missing verification fails closed. An admitted handshake keeps the
shared guard until actual authentication completion; a missing completion
retains an uncertain fault until reset. Classic bond count/peer matches are
refreshed from the actual Classic SDK list, independently of HID identity.
Unverified/unbonded peers cannot receive microphone PCM or supply playback.
Legacy PIN/passkey-entry requests are rejected. SSP Just Works has no MITM
claim, no numeric-comparison UI and no inferred nearby-host list.

Audio → Bluetooth exposes **Pair audio** and **Close pairing**, each with a local
confirmation. Pair audio closes LE admission, then requests the existing 120-second
Classic window; it preserves all saved bonds. The page distinguishes preparing,
SDK-confirmed discoverability with a countdown, finishing authentication,
authentication completed, expiry and failure. Authentication completed does not
mean a profile or PCM stream is ready. Lock, screen-off, leaving Audio mode or
selecting USB closes a window opened by this coordinator; an already admitted
handshake still follows the transport's existing completion rule.
Closing denies new admission immediately, but remains visible as closing and
cannot reopen until the GAP owner confirms non-discoverability and actually
releases shared admission. Only a capacity rejection from opening a window is
labelled full; asynchronous security errors retain a general failure message.

Pairing requires idle, hardware-off audio. Mic/Speaker start waits until the
pairing window and admitted handshake end, then still requires the actual
negotiated role, format and Classic peer identity. An empty, disconnected idle
profile selection may yield to the other ready Bluetooth profile; an existing
different peer, an active consumer or unconfirmed hardware-off still blocks it.
The retained audio-owner peer is checked even if the transport already cleared
its disconnected peer. Shared transport preparation and the legacy policy entry
point reject audio activation while shared Classic admission is held, including
pending authentication and closure. Pairing opening rechecks audio idleness under
the same lock after querying SDK capacity, so queued remote/local starts and
pairing cannot both be admitted.
An engine start rejected as busy during transport preparation ends with a busy
job error, without codec preparation, quarantine or a stop for an unowned
transport. Closing pairing permits a fresh, separately authorized start.
Current transport faults take precedence over an older window-opening error.
A new opening attempt clears a previous nonfatal pairing error; fatal transport
faults remain latched. The coordinator copies the actual post-attempt snapshot.
Preparation ownership is recorded before authority/privacy rechecks, so a
simultaneous revocation retains its own reason without stopping an unowned
source. An ordinary busy rejection uses the distinct busy stop reason.
Submitting codec preparation revokes the previous hardware-off confirmation.
Privacy cleanup includes pending preparation even without an active job, and
hardware-off becomes confirmed only after the full stop acknowledgement.
HFP Connect uses actual SLC completion before requesting SCO. A2DP Speaker waits
for the host to supply a decoded audio stream. Neither action grants capture.

### Device acceptance

Use a candidate built with the pinned target configuration; coordinate flashing
and serial ownership separately. Host tests and UI renders do not qualify radio,
host compatibility, acoustic output or microphone capture.

1. Keep existing HID pairings. Select Audio → Bluetooth, confirm Pair audio and
   search on a Classic-capable host for the displayed audio name. Check preparing
   versus discoverable/countdown, cancel, expiry and reopen without bond loss.
2. Complete pairing; verify the saved Classic peer and actual profile link. Check
   that pairing alone starts neither Mic nor Speaker. Reconnect an existing peer
   with Connect; HFP must wait for actual SLC and SCO/format readiness.
3. Select the device as the host microphone, open its recording stream, then
   explicitly Start mic. Record and listen to captured speech. Stop/mute and
   verify host capture stops; a ready label alone is insufficient.
4. Start host playback to the device, explicitly Start speaker and listen to the
   physical output. Verify Speaker never enables Mic. Exercise idle HFP-to-A2DP
   and A2DP-to-HFP role changes, retaining different-peer and active-job barriers.
5. Check lock, screen-off, leaving Audio and USB selection close new admission.
   Return to USB and verify the existing independent Mic/Speaker flows. Record
   host OS, actual profile/format, observed results and any untested step.

The upstream pinned SDK auto-accepts Just Works before notifying the app. The
reviewed [SDK patch](../patches/esp-idf-classic-pairing-admission.patch) routes
that request through the existing confirmation event. Its [manifest](../patches/esp-idf-classic-pairing-admission.json)
records the exact upstream commit, preimage/postimage file hashes and patch
digest. `tools/idf.py` setup/build verifies/applies it through
`tools/idf_patches.py`, rejecting all other tracked SDK changes and preserving
the unchanged strict BSP policy. Known partial application can resume; staged
or unknown edits are never overwritten. `bt_audio` reports unsupported without
the patch's header marker. Run `python3 tools/idf_patches.py --check` to inspect
the applied state without writes. The patch needs separate security review on
SDK updates; changing the header marker alone does not qualify an SDK.

## Manager integration and safety

`bt_audio_route_port()` implements the shared audio route interface.
Capabilities expose actual Classic peer and stream tokens, including changes
on reconnect, SCO/format change and A2DP suspend/start. Tokens never use the HID
peer. Preparation requires the exact current token and actual mono format,
with 10 ms frames. HFP supports microphone and speaker; A2DP supports speaker.
Every capture carries that token, exact audio generation and an increasing
sequence. A stopped/cancelled reservation cannot be rearmed by retrying the old
prepare. A stop for an old reservation cannot mute its replacement.

One serialized manager task calls `bt_audio_service()` at least every 10 ms,
polls `bt_audio_receive()` and forwards successful frames through
`app_audio_submit_transport_playback(route, frame)`. The manager explicitly
calls the route's `bind_playback` with a nonzero consumer generation, separate
from preparation. Each callback retains that identity on entry and rechecks
it before committing PCM; received frames carry that immutable generation.
Binding changes flush only receive queues/partial frames, reset sequence and
sample offset, and preserve microphone capture. An old close cannot stop a
replacement, identical enable is idempotent, and closed generations cannot
be resurrected. The manager rejects old/zero consumer IDs rather than rebinding
them to its newest playback job.
Bluetooth callbacks never recurse into the manager or perform codec/storage
I/O. `bt_audio_receive` assembles complete 10 ms frames from variable SDK PCM
chunks without padding. The manager owns format conversion, physical mute,
recording permissions, capture consent, the indicator and privacy/lock policy.
It must stop/cancel the transport immediately when those permissions close.
Transport gates do not prove physical microphone-off or speaker mute.

There are sixteen fixed 480-sample packet slots (8 SBC RX, 4 voice RX, 4 voice
TX), allocated once with PSRAM preference and internal-memory fallback. No
callback allocates or waits on an application queue. Packets expire after
150 ms; malformed receive PCM, receive overrun or an expired receive packet
revokes the current reservation instead of stitching PCM across a hidden gap.
TX underflow returns zero bytes; samples are never fabricated. Full TX queues
provide backpressure. An old radio packet already copied by the SDK cannot be
recalled; stop confirms the local producer/queue barrier, not peer buffers or
physical output. The legacy SDK PCM callbacks carry no peer/handle; their
association and internal flush behavior depend on this pinned SDK. A change
of that SDK requires re-review and hardware qualification of these boundaries.

One profile command can be pending/in flight. Acceptance mutes and flushes;
actual profile events determine completion. Cancelled in-flight operations
remain quarantined until their matching completion or the 15-second timeout.
A timeout leaves an uncertain failure, never an invented disconnect. Snapshot
`error` and its stage/time preserve the first historical transport failure;
current readiness and the current request error are separate fields.

## Saved Classic host UI

`bt_audio_bond_list()` returns the actual bounded Classic SDK saved-address
list, its known/refreshing/error state and a revision. Current HFP/A2DP matches
are separate from BLE HID host identity. The SDK list has no saved host names;
every name remains explicitly unknown. Old rows retained after a read failure
remain marked unknown rather than becoming zero hosts.

`bt_audio_refresh_bonds()` queues a task-side refresh. UI connect/disconnect and
voice operations use `bt_audio_saved_request()` with both the current transport
generation and exact Classic list revision. Unknown/stale lists and addresses
absent from that list reject before queueing. A queued saved-host command is
rejected if an actual bond mutation/removal or an unavailable refreshed list
overtakes it before SDK submission. An observational refresh of the same saved
peer preserves the accepted intent.
Acceptance, actual profile completion and current media readiness remain
separate observations; selecting a saved host does not unmute capture.

## Evidence

Run `python3 tests/bt_audio/run.py` for sanitized native state/PCM regressions.
The real module was also compiled against the pinned ESP32-S31 SDK headers
with Classic/HFP/A2DP enabled and compiler warnings treated as errors. This
isolated object check does not prove the combined image links or runs.
The lifecycle/configuration owner must build the final combined image.
Run `python3 tests/bt_audio/test_sdk_admission.py` for the actual patched SDK
function's Just Works/confirmation dispatch and allocation/dispatch failures.
Run `python3 tests/toolchain/test_idf_patches.py` for isolated Git fixtures
covering apply, idempotence, known partial recovery, wrong pins/digests and
preservation of unrelated/unknown/staged SDK work and strict BSP verification.

No board, host pairing, USB/serial, flashing or physical audio test was used.
Capabilities remain physically unqualified. Actual microphone/speaker levels,
HFP duplex behavior, A2DP/HFP/BLE coexistence, callback timing, host privacy and
interoperability require later authorized device qualification. These are
explicit evidence limits, not successful or zero-valued measurements.
