# Live sensor observations

Status: source implemented and component-reviewed; combined integration and
physical qualification are separate gates. This API supports development of [handheld input](motion-estimation.md)
without coupling capture to the Mouse Motion switch or stopping input to export.

## Sources and ownership

The existing board worker remains the sole inertial and battery reader. A bounded
raw journal observes its accepted samples and typed failed/not-ready acquisition
attempts. It retains accepted observations preceding a rejected attempt; the
existing actuation delivery queue and its cancellation policy remain independent.
Diagnostics never change hardware capability, recovery, sampling rate, calibration,
input ownership or persistent settings. No request handler performs sensor I/O.

| Source | Meaning and initial cadence |
| --- | --- |
| IMU | BMI270 acceleration in g and angular rate in degrees/s, display frame X right/Y top/Z outward; configured 100 Hz, 4 g and 2000 degrees/s. Timestamps describe host acquisition completion, not known sensor conversion time |
| Acquisition events | Actual rejected attempt kind, error, known data-ready status and read timing. A rejected attempt is not a valid zero-valued IMU sample |
| Power | Existing BQ27220 cache, polled at most once per second; percent estimate, mV, signed mA, gauge temperature in Celsius, validity and three-state power/battery/charging observations. Gauge temperature is not qualified ambient or MCU temperature; external power remains unknown without a supported measurement |
| Touch | Changes observed by the existing LVGL contact reader, with its sequence/time and validity. Coordinates outside the public touchpad are redacted, including private editors; this is not a second digitizer reader |
| AI button | Existing debounced down/up events and source timestamps. Reading events grants no ability to emulate a physical unlock |
| Context | Capability, sensor health and haptics receipts are status observations, not additional physical sensors |

Uninitialized magnetometers and die-temperature providers must remain explicitly
unavailable. Microphone PCM uses the existing separately authorized media path;
sensor read permission never starts or grants audio capture.

## Common read API

Both transports read the same immutable journal. They maintain independent host
cursors; one client never drains, fences or advances another client's view.

| Resource | Result |
| --- | --- |
| `GET /api/v1/sensors/catalog` | Schema, boot/image/configuration identity, sources, units, ranges and resource limits |
| `GET /api/v1/sensors/latest` | Current cached health/power/context with knownness, freshness and acquisition timestamps |
| `GET /api/v1/sensors/samples?boot=<32hex>&after=<uint64>&limit=<1..8>` | Bounded immutable page, oldest/latest sequence and explicit retention gap |

The station HTTPS path uses existing authentication and the `diagnostics.read`
scope. A `view` grant alone does not grant sensor access. No new listener,
discovery password or Wi-Fi access point is introduced. Wrong boot identities,
future/noncanonical cursors, repeated/unknown parameters and unsupported paths
are rejected. An HTTP server restart does not change the telemetry boot identity.

USB uses the existing CDC owner with an isolated `ST1` namespace. Reads require
a mounted device and known unlocked state. The local trust-import window takes
priority and suspends sensor export; `CP1` commissioning commands remain valid
only inside their existing window. Sensor reads cannot dispatch HID, settings,
enrollment, reset, OTA or recording actions. Lock/disconnect cancels pending
export; bytes already transmitted cannot be recalled.

## Record and transport format

The version-1 `stj1-le-b64` representation uses explicit little-endian encoding,
not native C struct copies. Float values retain their exact IEEE-754 bits;
validity remains separate from numeric zero. The shared page header is 56 bytes:

| Offset | Field |
| --- | --- |
| 0 | Version u8, record count u8, record size u16 |
| 4 | Page flags u32 |
| 8 | Request ID u64 (zero for HTTP) |
| 16 | Boot identity, 16 bytes |
| 32 | Requested cursor u64 |
| 40 | Oldest retained sequence u64 |
| 48 | Latest journal sequence u64 |

Each 88-byte record holds five u64 fields (journal sequence, source sequence,
source generation, acquisition timestamp and publication timestamp), kind u16,
flags u16, error i32, discontinuities u32, validity u32 and a 32-byte typed payload.
The payload mapping is versioned with the producer header and host decoder.
A trailing four-byte IEEE CRC32 covers the entire header and records. Source
generations are not HTTP sessions or actuation subscriber epochs.

The canonical field, validity-bit and typed-payload definitions are in
[board_sensor_journal.h](../firmware/main/board_sensor_journal.h).
Page flags are GAP=1, MORE=2, EXHAUSTED=4 and UNAVAILABLE=8. A GAP page has no
records: resume at `oldest - 1` and record exactly `oldest - after - 1` missing
journal IDs. Otherwise advance only to the final verified record, or retain
the cursor on an empty page. EXHAUSTED does not discard retained observations.

HTTP returns a compact JSON wrapper with `schema`, `encoding` and base64 `data`.
USB requests `ST1 READ <16hex-id> <32hex-boot> <after> <1..3>` and returns one
`ST1 R <id> <raw-length> <base64>` line. Three records plus header/CRC occupy
324 bytes before base64, keeping a response below the 480-byte frame ceiling.
`ST1 CATALOG <id>` and `ST1 LATEST <id>` return metadata fragments as
`ST1 DATA <id> <offset> <total> <raw-fragment-length> <base64>` with at most
192 raw bytes each, followed by `ST1 END <id> <total> <sha256-of-whole-json>`.
Every response frame begins with a newline to recover from a partial write or
interleaved log. A host verifies the whole response before accepting it. No
second stdin reader or USB stack is installed.

USB nominal capacity is three records per 20-ms worker iteration, before log
competition, other work or host delay. This is not a measured loss-free guarantee.
The 256-record journal has finite retention, not a guaranteed duration. Slow or
disconnected readers cannot block acquisition or input; overwrites and exhausted
identities remain visible. Complete length, checksum, boot, request identity and
record ordering must be verified before advancing a host cursor. A repeated read
can return the same immutable records and is safe to deduplicate by boot/sequence.

## Collection and validation

The host collector uses an explicit USB port or an enrolled HTTPS profile. It
never resets the device, clears its buffers, enrolls trust, changes Motion or
issues input. Preserve raw responses, decoded values and a quality receipt in
new local files. Do not interpolate missing samples, replace unknown values with
zero, assume that a complete download is a continuous sensor segment, or claim
pointer usability from sensor availability.

From the repository root, use the project Python environment with `pyserial`
for USB, or the existing enrolled bridge profile for station HTTPS:

```sh
python3 tools/sensor_capture.py --port <device-port> --seconds 30 --output <new-output-prefix>
python3 tools/sensor_capture.py --profile <enrolled-profile.json> --seconds 30 --output <new-output-prefix>
```

The prefix creates four sibling files (`.wire`, `.jsonl`, `.csv`, `.receipt.json`)
with private file permissions; existing outputs are never overwritten.
The polling window is bounded to 1..600 seconds and the default output budget is
64 MiB. `--max-bytes` sets a 64 KiB..256 MiB bound. `--after` selects a cursor
within the boot reported by the initial catalog; do not reuse another boot's
cursor. Keep the raw stream, decoded JSONL/CSV and receipt together. A complete
collection can still contain rejected acquisitions or retention gaps; consult
the receipt rather than treating command completion as usable motion data.
Connection, metadata and completion have separate finite timeouts; see the
[collector instructions](../tools/sensor_capture.md) for these limits and periodic
snapshot options. The tool does not commission an untrusted LAN service. Follow the existing
[local enrollment workflow](wifi-management-ota.md) before using a profile.

Validation covers independent cursors, retention overwrite, counter exhaustion,
typed rejection without losing preceding raw data, readonly power observation,
lock/import/disconnect races, short writes and interleaved logs, truncated or
conflicting fragments, numeric boundaries and corrupted payloads. Target stack,
heap and input timing must be measured with both active and stalled readers.
Physical USB and authenticated LAN runs remain required before declaring either
transport qualified. The earlier diagnostic captures and failed Motion behavior
remain in the [validation record](validation.md).
