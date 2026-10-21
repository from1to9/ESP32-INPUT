# Remote device controls

Run `python3 tests/remote_device/run.py` for the production portable adapter with
the generated canonical schemas, settings codec and protocol SHA-256. Strict C11
warnings and Address/Undefined Behavior sanitizers are enabled. The manager port
is the substitution boundary; no device, serial, pairing or flash operation runs.

The suite checks inert bounded transactions, field-scope union, exact native
units, whole-candidate rejection, immutable principal/session/revision-bound
approvals, physical-presence requirements, cancellation, clock rollback,
completion backpressure and secret-free reads. Accepted/running work cannot
become applied/completed without its actual owning receipt. Explicit durable
settings additionally require the saved revision.

The coordinator binding cases in
[`remote_device_cases.inc`](../coordinator/remote_device_cases.inc) exercise the
actual main helpers, production adapter and independently delivered manager
acknowledgements. They cover temporary settings without NVS writes, preservation
of the durable baseline, haptic on/final-off receipts, capture-safe display sleep,
immutable local review, restart-pending semantics, ordered partial factory reset,
failure preservation, scoped read caches and bounded rejection recovery.

`preferences.reset` supports `all`, `display`, `input`, `haptics` and `bluetooth`.
Input defaults include the complete native motion preferences. Factory reset is
explicitly **Reset preferences and connections**: device preferences, LE HID
bonds, Wi-Fi profiles and HID target preferences. Its published retention
manifest preserves control trust, Classic bonds, firmware/OTA, models,
recordings, unknown namespaces, external NAND and factory backups. There is no
raw erase, automatic restart, new pairing window or claimed atomic rollback.
Wi-Fi deletion runs last; a partial failure stops later stages and remains
visible. An absent approval ID stages an inert review; a supplied ID must match
the immutable action. The retained-manifest digest remains mandatory.

The device settings provider supports the sixteen canonical fields backed by
`device_settings_t`. Other owners' fields are rejected as unsupported rather
than silently ignored. Independent motion X/Y gain, inversion and freeze fields
share the native preferences and selective reset path. Scalar gain aliases set
both axes; mixing scalar and per-axis gain in one candidate is rejected.
Haptic diagnostics use the four existing native profiles:
confirm 24ms/35%, mode 45ms/55%, lock 65ms/65%, and error 165ms/65% with its native
off gap. Unsupported patterns or differing duration/strength overrides reject.
The peer resource paginates at most eight rows using `cursor` and `limit`; it
reports independent owner revisions and unknown names/current matches explicitly.

Host checks and target compilation do not establish hardware timing, physical
orientation, optical visibility, motor output or successful restart behavior.
