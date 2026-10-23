# Native audio, recording and spoken-letter UI proposal

Status: native media controls with the authorized Mic/Speaker primary revision, 2026-10-23.
The UI submits typed requests to the real asynchronous runtime and reads cached
owner snapshots. Hardware compatibility and physical display/audio behavior
remain unverified; host fixtures do not establish those outcomes.

The behavior owners are [requirements](../docs/requirements.md), the
[operation matrix](../docs/api-function-matrix.md), the
[audio/recording contract](../docs/api-audio-recording.md), and the original
[UI plan](ui-plan.md). This proposal maps those contracts to the native screen;
it does not introduce alternate wire commands or loosen admission rules.

## Navigation and shared state

Keep Mouse, Keyboard and Audio as the three primary modes. Audio shows Mic and
Speaker together, each with its own observed state and Start/Stop control.
Its connection chooser contains USB and Bluetooth. Routes, roles, Voice,
recording and storage are not primary Audio navigation. The advanced recording
backend and detail/review panels remain available to their existing owners;
they are not deleted by this product simplification. Keyboard retains
`Spoken letters`. Opening any page selects a
view only: it never enables a role, starts capture, grants consent, unmutes or
changes the HID target. Audio route/peer and HID target remain independently
labeled, including when they happen to name the same computer.

Each 480 x 480 page keeps the shared header and on-demand Menu. A persistent
capture/stop region has priority over page content and overlays. The middle
content uses one dynamically created detail panel, with back navigation and
44-pixel minimum action targets. Large setup forms use sequential panels rather
than shrinking controls or permanently creating hidden screens. Preserve the
current resource ceiling and zero idle redraw requirement; measure both after
integration instead of assuming this proposal fits.

State must separate supported, available, requested, admitted, applied and
unknown. Disabled controls show the manager's safe reason, such as `Audio driver
unavailable`, `Recording storage unavailable`, `Stop recording to change route`,
or `Microphone stop unconfirmed`. A successful command admission cannot change
the label to Listening, Playing, Muted or Saved. Only the corresponding observed
manager/recorder result can do so. Do not use one `audio_available` Boolean to
represent every route and operation.

## Audio page

| Screen control | Owner operation and evidence | Visible result |
| --- | --- | --- |
| USB / Bluetooth | `APP_MEDIA_SELECT_CONNECTION` with the owner-selected connection enum | Applied selection is separate from link readiness; the media owner resolves USB/HFP/A2DP and release barriers. The UI never guesses a Bluetooth profile or changes HID transport |
| Audio device / Connect / Disconnect | Existing media peer and link intents in the Bluetooth connection panel | Actual saved audio peer and link state; service connectivity alone is not evidence of a live microphone or speaker stream |
| Start mic / Stop mic | `APP_MEDIA_LIVE_START/STOP`, microphone role and exact job/consumer identity | Independent live-capture state; explicit start only, with owner admission and local consent. Enabled hardware alone never displays On |
| Mute mic / Unmute mic | `APP_MEDIA_SET_MUTED`, microphone role and explicit Boolean | Confirmed mute differs from requested mute. Unknown acknowledgement offers only an admitted mute, never an invented unmute |
| Start speaker / Stop speaker | `APP_MEDIA_LIVE_START/STOP`, speaker role and exact job/consumer identity | Independent playback state; no implied microphone start, recording, pause or successful audible output |
| Volume | `APP_MEDIA_SET_VOLUME`, bounded by the actual safe ceiling | Applied local output level. Unknown is `--`; zero volume does not imply stopped or hardware-muted playback |

The Mic card is x 12/y 64/w 456/h 168; Speaker uses the same dimensions at
y 244. Each has a title and a two-line state at x 28, and a 136 x 48 Start/Stop
button at x 316/y 80 or y 260. Mic privacy uses x 28/y 168/w 424/h 48.
Speaker volume has a 408-pixel slider at x 36/y 376, with its value above;
the 12-pixel track plus 16-pixel extra hit area on each side gives a 44-pixel
touch region. A bounded reason stays at x 12/y 424/w 456/h 48. The connection
entry reuses x 140/y 8/w 132/h 48. Mouse and Keyboard geometry is unchanged.

Both cards remain visible regardless of the legacy `audio_kind` value.
Starting, running, stopping, finishing and error come from the corresponding
`live_capture` or `playback` job. `can_start` admits an explicit action and can
show Off / Ready; it does not establish running audio. The two cards add six
fixed objects: the approved core limit is 112, with the dynamic limit remaining
160. Native resource measurements must prove these bounds and zero idle work.
An independently running recording or spoken-letter consumer is shown as
Mic in use, rather than claiming that the microphone is off. The global privacy
Stop/lock remains available and ordinary live Stop does not target another job.

Each role also carries `start_block`, the audio owner's semantic result from
the same admission function that produces `can_start`. A disabled Start shows
this reason on the existing second status line and repeats it with its role in
the existing footer. For USB, an unopened host stream says `Open Mic on host`
or `Play audio to this device on host`; it must not claim a disconnected link.
Unknown or invalid reason codes stay unavailable. Running jobs, actual job
errors and an independently active microphone take precedence over start
reasons. The UI maps codes to bounded English copy; it does not reconstruct
the admission decision or add controls, objects, or automatic start behavior.

For `off -> preparing -> ready_muted -> running -> stopping -> off|fault`, show
the exact owner state in readable words. During stopping, disable starts and
conflicting changes; preserve Stop/lock and the uncertainty warning. A rejected
duplex/route request leaves the applied state intact with its rejection reason.
If a future connection needs a distinct bounded call-session operation, its owner
must define that operation before enabling Start mic. Do not alias a local
capture command to an unimplemented HFP connection.

## Consent, capture indicator and local Stop

An advanced recording start review names the source, consumer, destination/persistence,
actual profile, maximum duration and byte bound. File recording also states
retention and export policy. Use an existing valid scoped grant or an explicit
exact-job local consent action; never fabricate a permission grant from a page
visit or saved preference. Audio starts disabled and muted after boot, reconnect
and recovery. A local standalone recording/letter workflow must not depend on
Wi-Fi when its actual manager/store/recognizer is available.

The critical region is driven by manager capture/egress state across every
mode, not `audio_active && !muted` or the current page. It distinguishes
`Starting microphone`, `Microphone active`, `Stopping microphone` and
`Microphone stop unconfirmed`. Consumer labels identify Mic, Recording or
Spoken letters without exposing samples/transcripts. Keep an immediately
accessible local Stop and the existing lock path; indicator loss or screen
blanking cannot leave capture running. Actual stop acknowledgement clears the
warning, not a command queue success. A privacy stop closes all affected
consumers; stopping one ordinary recording does not silently cancel a separately
qualified live consumer. Those distinct actions must have distinct labels.

## Retained advanced recording flow

These panels preserve the earlier recording implementation and recovery paths;
they are excluded from the simplified primary Audio navigation.

1. `Recordings` opens an authorized list with at most four visible rows and
   bounded next/previous cursor navigation. Show permitted metadata only; do
   not expose another principal's filenames, object IDs or recordings through
   global status. Empty, denied, unavailable and loading are separate states.
2. `New recording` opens a review of source, qualified format, available store,
   chosen time/byte limits, retention, export policy and consent. Persisted
   recording requires an explicitly selected, available recording store. The
   current direction is the [dedicated internal recording area](native-recording-storage.md);
   unknown factory NAND is preserved and requires separate future migration.
   A volatile capture is
   never silently presented as a saved recording. Capacity unknown is not zero
   or enough space. Do not preselect unsupported candidate profiles.
3. `Start recording` submits one explicit `recording.start` with the confirmed
   fields and binding revisions. Disable repeat starts until reconciliation;
   retrying an uncertain result uses the same logical request and reconciles
   its returned job/object instead of creating another writer.
4. Active detail displays observed state, elapsed wall time, actual captured
   duration, maximum runtime and storage bound. Distinguish payload, stored and
   verified bytes where relevant; no animation may imply sample capture or sync.
5. `Stop and save` invokes `recording.stop`. Close capture first, then show
   `Finalizing` until verified publication and sync. The microphone indicator
   may clear after acknowledged hardware stop while finalization remains
   pending. Initial v1 has no recording pause/resume control.
6. `Cancel recording` offers explicit `Keep partial` or, with deletion authority,
   `Discard`. Map to `recording.cancel` with its mandatory disposition. Show
   cleanup pending/storage uncertainty truthfully. If publication won the race,
   display the actual terminal outcome; deleting a completed object is a
   separate action, never an automatic cancellation side effect.
7. Completed detail shows verified duration/size and permitted Play, Download
   or Delete actions. Local Play additionally needs a qualified playback owner;
   Download is the authenticated `recording.read` pull, not a completed export
   job. Standalone on-device Download may instead explain the authenticated
   management entry point; no arbitrary destination or fake transfer button.

| Recorder state | Primary presentation | Available safety/recovery action |
| --- | --- | --- |
| `starting` | Preparing recording; no captured-duration claim | Stop/cancel when owned and admitted; local privacy Stop |
| `recording` | Recording with actual elapsed/sample data and limits | Stop and save; explicit cancel; privacy Stop |
| `stopping` | Stopping microphone; hardware state remains explicit | Privacy Stop/lock remain accessible; no restart |
| `finalizing` | Verifying and saving; capture status shown separately | Only owner-advertised cancellation; no delete/read/play of active writer |
| `completed` | Saved, based on immutable verified result | Authorized read/play/delete, subject to current capabilities |
| `partial` | Incomplete recording plus actual verified prefix/reason | Metadata and authorized deletion. No initial-v1 read/play/download of partial content |
| `error` | Recording failed; recovery/cleanup may remain unknown | Owner-supported recovery or deletion; no empty successful recording |

The common job state is separate from the recording artifact state. For example,
`cancelled` can coexist with a retained `partial` object, and cleanup may remain
pending after capture stops. The UI must not collapse these into one success
badge. Default owner-loss policy is stop. `Continue locally` is a separately
explained, explicitly granted file-only option when implemented; no implicit
background activity follows from a HID lease or a connection closing.

## Spoken letters

Preserve the original listen -> candidate -> confirm into preview -> Send
workflow. The target is bounded letter-command recognition, not full dictation.
The production recognizer must supply its actual vocabulary/profile and
confidence policy. Sample words from the prototype are help text only; never
generate a candidate without a recognizer result.

| User action / observation | Owning operation | UI behavior |
| --- | --- | --- |
| Open Spoken letters | `keyboard.submode.select` | Show vocabulary help and availability. No microphone start |
| Listen | `speech.listen.start` with actual bounded profile and capture consent | Starting until real frames/recognizer state; then Listening with persistent capture indicator and Stop |
| Stop listening | `speech.listen.stop` | Stopping until acknowledged; retain warning on uncertainty |
| Candidate | `speech.state`, bound speech generation/revision | Display actual candidate separately from the draft; no automatic insertion or host send |
| Reject / Try again | `speech.candidate.reject`; a new listen requires an explicit admitted start | Invalidate that candidate, retain existing draft; do not silently restart capture |
| Confirm letter | Matrix UI29 confirmation, exact candidate/speech generation plus current input guards | Append once to the local preview only after coordinator acceptance |
| Send | Existing guarded text-send workflow | Send the preview with no implicit Enter, preserving current target/acknowledgement behavior |

The speech owner must agree on bounded states for unavailable, idle, starting,
listening, candidate, stopping and fault, including candidate lifetime after
stop. These are proposed presentation states, not newly claimed wire enums.
Privacy lock/revocation, resource replacement and obsolete generations must
invalidate candidates before confirmation. A stale result cannot enter the
preview after target/session replacement. Private candidate text remains local
to the authorized view and is cleared on its defined invalidation boundary.

## Required owner interface agreement

Before modifying `app_ui.h/c`, agree on bounded snapshots and typed intents:

- Per-operation capability and safe reason, qualified route/profile IDs, actual
  audio peer/link, role state, requested/applied mute and volume, resource
  conflicts, microphone/egress stop acknowledgement and any privacy fault.
- Audio generation, manager/resource revision, session/job identity and owned
  action rights. Media jobs use management/audio authority, not HID lease/epoch;
  candidate confirmation and Send separately use the input guards they require.
- Capture consumers, current indicator obligation and deadlines/limits, without
  samples or unauthorized private metadata in ordinary global status.
- Recorder artifact state, common job state, exact extents and integrity,
  finalized/readable flags, allowed cancellation disposition, cleanup state and
  bounded ACL-filtered list entries/cursors. Unknown values carry availability
  flags, not zeros.
- Speech generation/revision, qualified profile/vocabulary, actual candidate
  availability and expiry, confidence-policy result and capture session state.

The 2026-10-23 connection revision appends `APP_MEDIA_SELECT_CONNECTION`,
preserving existing operation numbers. `value.connection` carries USB or
Bluetooth. The copied view adds `connection_known`, `connection` and two
`can_select_connection` flags. Known means the owner-applied selection is known;
it does not imply host enumeration, negotiated profile, stream readiness or
audio activity. The owner retains the previous applied selection through a
pending switch and owns stop/flush/token barriers. Requests retain the existing
management/audio/resource revision checks. The primary connection chooser uses
the existing media-options overlay; its title and controls contain no internal
profile terminology.

UI callbacks submit typed coordinator intents only; they perform no codec,
storage, networking or recognizer work. Bind controls to the snapshot identity
presented at press time. Reject stale release/selection, disable pending starts,
and reconcile unknown outcomes instead of optimistic toggles. The presenter
derives display strings from owner snapshots without changing owner behavior.

### Implemented capture-warning presentation boundary

`app_view_t` now carries `capture_indicator_required`, the nonzero
`capture_indicator_generation`, and `capture_state`. The last field distinguishes
unknown, starting, active, stopping, uncertain and confirmed off. The required
flag is the audio manager's obligation, including preparation and an unconfirmed
hardware stop; it is independent of the displayed mode, role mute preference,
input lock or HID ownership. The coordinator must map actual manager evidence.

The UI shows the warning and Stop + lock action in the core header or actual
visible panel header, even while a held press defers navigation replacement.
The lock screen retains the full warning and never claims microphone mute/off
without explicit evidence. A hidden screen does not arm an indicator.

`app_ui_capture_indicator()` is a display-lock-only query of
the current visible object tree. It returns an unarmed snapshot for absent/hidden/unknown
generation, otherwise an inclusive marker rectangle plus capture generation and
monotonic indicator revision. A newly armed generation invalidates the warning once, even when
its text matches the preceding generation; repeated snapshots do not redraw.
Legacy listening/audio flags may retain a conservative warning but cannot arm
a generation without the explicit manager obligation.

Arming is not presentation acknowledgement. The presenter/display owner must
bind the armed generation to the frame being rendered and report it only after
the actual panel transfer/frame handoff completes. Publishing a snapshot,
returning from `app_ui_render`, LVGL queue admission and the armed getter alone
are insufficient. Use the actual completion timestamp for the manager's
indicator freshness gate; invalidate visibility on blanking, display failure,
screen replacement or stale generation. If the board cannot prove that
boundary, indicator evidence is unavailable and capture must stay closed.

Native tests verify warning content/Stop on every existing panel, both themes,
lock, deferred navigation, generation replacement, absent generation, off
confirmation and zero idle redraw. These tests observe native flush separately
from arming; they do not prove a physical panel is readable.

## Touch/Motion coexistence follow-up

The requested default is touchpad tap, double tap and drag alongside Motion.
Touch takes priority; Motion resumes only after the existing freeze/release
guard. Keep an explicit movement-only preference, visible drag state, gesture
help and Cancel drag. Swipe behavior needs explicit distance/time/axis semantics
and ownership before activation. Multi-touch availability must come from the
actual sensor/driver event path; do not show two-finger help or controls merely
because the display advertises touch. Separate horizontal/vertical Motion
sensitivity and automatic posture projection remain input-owner work.

## Acceptance evidence needed after interface agreement

Native LVGL checks must cover all states above, transition/late-result rejection,
capture indicator and Stop across pages/overlays, supported versus unavailable
controls, maximum strings/rows, keyboard candidate privacy, pointer release
while disabled, repeated open/close resources and no idle redraw. Manager tests
must exercise real production transitions, cancellation/publication races,
limits, consent and stop failure. Target builds and actual codec/storage/host
qualification are additionally required; rendered fixture states are not audio,
recording, recognition or interoperability evidence.

## Implemented native media surface

`app_media_ui.h` is the bounded copied owner/UI boundary. `app_ui_media.inc`
implements independent Mic/Speaker cards and the USB/Bluetooth connection panel,
plus the retained recording profile/duration review,
recording controls, four-row list/detail, playback, destructive confirmations
and explicit spoken-letter candidate confirmation/rejection. Unknown state is
not rendered as muted, saved, available, or an empty successful list. Audio
peer identity comes from the media owner independently of HID selection.

All actions use `APP_MEDIA_CONTROL` with immutable generations, resource
revision, profile/object/job/consumer and speech candidate identities. A press
captures the presented request; release is rejected after identity or admission
changes. Recording start, delete and discard open a coordinator-owned immutable
`media_review`; `APP_CONFIRM` carries its original request. Start shows the exact
profile, duration, byte limit, retention, download and continuation policy.

Advanced microphone gain follows the engine's discrete 0..42 dB in 6 dB steps;
it is not a primary Audio control. Volume
remains bounded by the actual safe ceiling. Reading or opening a page does not
start capture or playback; speech confirmation never sends HID. Runtime mapping
is owned by `app_media_controls`, while actual jobs, consent, storage, codec and
recognition remain their respective production owners. Physical qualification
is explicitly deferred to a later device test.

## Implemented coordinator boundary

`app_media_controls.c` owns bounded local product policy above the media runtime.
It copies owner snapshots, retains one request token, consumes only its matching
result, and copies query payload before acknowledging it. Rejected or uncertain
mutations are never automatically retried. UI gestures carry resource revisions,
immutable object/job identities and candidate generations; confirmation retains
the original recording limits or deletion target.

Core Start/Stop submits a bounded live consumer or stops its exact job. The
retained backend role-enable operation only prepares hardware; it does not
create a live consumer and is not a separate primary UI step.
Live host routes have a one-hour ceiling. Local recording defaults to five
seconds, clamped to actual store overhead and free quota, and is separately
reviewed with a 512 KiB maximum budget, one-hour retention and no export grant.
The four-object store uses a zero next cursor to mean end of list. Unknown
recording byte totals display as pending, never zero verified bytes.

The saved audio peer control cycles the actual cached Classic bond list; choosing
a peer does not connect it. Connect carries the exact bond revision and address.
Connect voice retains the exact saved Classic peer, bond-list revision and
transport request token. It waits for the successful HFP service-level connection
before explicitly submitting OPEN_VOICE for that same peer, or submits OPEN_VOICE
directly when that peer already has an SLC. Each phase has the transport's
15-second bound. Success also requires observed SCO and its supported negotiated
format. Replaced requests/peers, catalogue changes, lock, missing runtime state,
failure or timeout prevent the next phase; they never cause automatic retry.
Already submitted radio work remains owned by the transport's quarantine and
timeout, rather than cancelling a possibly replaced global request. The separate
Disconnect button remains available for an idle SLC after a failed connection.

An SLC-only connection cannot enable live microphone capture. Connection neither
starts a live consumer nor grants microphone consent; Start remains a separate
local action. Bluetooth audio peer selection never reads or changes the BLE HID
target. A2DP and explicit disconnect retain their media-worker request path.

Native tests exercise real LVGL gestures and production controls with documented
cached-owner seams. They do not validate physical codec acknowledgement, a
Bluetooth host, live USB audio, flash persistence or optical readability.
