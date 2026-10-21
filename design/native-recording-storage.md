# Native recording storage preparation

Status: UI boundary implemented; Board and Audio owner integration pending,
2026-10-21. The selected product direction is a dedicated internal recording
volume (832 KiB reserved; usable quota is owner-reported), with unknown factory NAND preserved. The UI never
uses that planned size as observed capacity or calculates time from a guessed
sample rate. External NAND migration is a separate future operation.

## View and local actions

Audio's chooser links to `APP_OVERLAY_RECORDING_STORAGE`. The
`app_view_t.recording_storage` snapshot carries a typed backend and lifecycle,
generation/revision, verified reserved-region identity, owner admission to
prepare, actual free/quota bytes, an independently available remaining-seconds
value for the selected profile, and safe profile/reason strings. No filename,
raw address, partition path, sample or credential enters this view.

Unknown capacity and remaining time display unavailable rather than zero.
Known zero remains zero. The Audio/store owner calculates the maximum whole
recordable seconds after actual container, encryption, manifest, reservation,
quota and selected-format costs. The UI only formats that result; it cannot
relabel 48 kHz bus samples as a 16 kHz recording profile or promise the planned
volume's whole raw capacity. Changes to the selected profile refresh this
evidence through the owning manager.

The local preparation sequence is:

1. `Prepare storage` submits `APP_RECORDING_STORAGE_PREPARE_REQUEST` with the
   visible storage generation/revision captured at press. It only opens the
   coordinator-owned `APP_CONFIRM_RECORDING_STORAGE_PREPARE` confirmation.
2. Admit the request only for `APP_RECORD_BACKEND_INTERNAL_RESERVED`, an observed
   unprepared state, a nonzero generation, `reserved_region_verified=true` and
   `can_prepare=true`. The Board owner must prove the exact dedicated region
   and its non-overlap with application/factory storage. Neither a device ID,
   an unmounted filesystem nor an unknown NAND handle proves that authority.
3. The confirmation names the dedicated internal recording area, explains that
   its contents will be removed, and states that factory NAND stays untouched.
   `Cancel` closes it without I/O. `Prepare storage` emits the existing
   `APP_CONFIRM` with `x=1` and the press-time storage identity.
4. The coordinator retains the original requested target/generation/revision
   and rechecks it at confirmation. Never silently rebind a pending confirmation
   to a replaced region or newer preparation request. The UI additionally
   rejects a generation/revision change during the confirming press.
5. Only the Board owner's explicit bounded prepare operation may write the
   reserved region. It must not initialize or format unknown external NAND.
   Opening Audio/Storage, booting, mount failure, timeout or retry never prepares
   storage automatically. Preparation is separate from capture consent and
   cannot start or resume a recording.
6. Display preparing until the actual owner acknowledges a usable store; show
   failure/uncertainty without success or repeated erase attempts. A ready/full
   store offers no Prepare action. Reformatting existing recordings is not this
   flow and needs a separately designed operation.

The UI has no storage I/O or new public wire operation. Main integration must
allow the new overlay and local request, preserve its confirmation identity,
and provide the actual snapshot from Board/Audio. The underlying Board API and
shared audio contract still need to expose the selected internal store; the
older NAND-only planning language must not be used as permission to format
factory NAND or as evidence that this new store already works.

## Validation boundary

Native UI tests cover unknown/unprepared/preparing/ready/full/error/uncertain
states, actual zero versus unavailable metrics, selected-profile remaining
seconds, rejection of external/unknown/unverified regions, explicit request and
confirmation identities, stale confirmation release, cancellation, disabled
preparation after readiness and capture warning visibility. Resource/idle checks
include the storage panel and its confirmation. Board operation, persistence,
partition placement and recording integrity require their owner's tests and
target compilation. No device write or device test is authorized in this phase.
