# Bounded sensor collector

This tool reads the same versioned sensor journal over USB CDC or authenticated
station HTTPS. It does not change Motion, input, settings, commissioning, audio,
OTA or device state. It never resets the device, clears input buffers or chooses
a device automatically. Use only an explicitly selected port or an existing
private profile carrying `diagnostics.read` authorization.

From the repository root:

```sh
.tools/control-bridge-venv/bin/python tools/sensor_capture.py \
  --port '<device-port>' --seconds 30 --output '.agent/local/sensors/usb-trial'
.tools/control-bridge-venv/bin/python tools/sensor_capture.py \
  --profile '<private-parent>/client-profile.json' --seconds 30 \
  --output '.agent/local/sensors/lan-trial'
```

Running either command accesses the selected device; offline validation does
not run these commands. The existing [bridge environment](../bridge/README.md)
provides pyserial and authenticated session dependencies. The collector never
creates a profile, imports credentials or enrolls trust. LAN retains the existing
CA, hostname and exact enrolled leaf-certificate fingerprint checks, with no
redirect or insecure fallback. Authentication tokens remain in memory; only
sensor GET response bodies enter the raw LAN file.

`--seconds` is a 1..600-second polling window after initial connection/metadata.
Initial authentication and metadata, one in-flight operation, final identity
verification and session closure have separate finite timeouts. USB permits at
most three read attempts within 6.5 seconds; HTTPS retains its four-second
request timeout. `--snapshot-seconds` controls separate latest-state reads
(0.5..60 seconds, default 1). Batch limits are three USB/eight HTTPS records;
the host immediately follows available data and sleeps only after an empty page.

`--after` is a canonical decimal unsigned 64-bit journal cursor, default `0`
(request history from record 1). Retention overwrite returns an explicit GAP
with the exact unavailable count. The tool records that event before adopting
`oldest-1` and continuing. It never interpolates, sorts, filters, drops a rejected
record or substitutes a zero measurement. Wrong boot, conflicting data, unknown
schema, invalid checksum or incomplete response cannot advance the cursor.
Each read retry uses a fresh transport ID and the same application cursor.

Four new files are reserved exclusively with mode 0600 before opening a device:

| File | Content |
| --- | --- |
| `.wire` | USB received bytes, including interleaved logs; for HTTPS, consecutive big-endian u32 body lengths and exact sensor response bodies, excluding authentication and HTTP headers |
| `.jsonl` | Catalog, latest snapshots, every decoded typed record with original binary/float bits, explicit gaps and page commit markers |
| `.csv` | A JSON unit/knownness header followed by fixed typed columns; unknown/nonfinite values remain empty with validity and original bits retained |
| `.receipt.json` | Identity, cursor, source/retention issues, transport anomalies, byte counts and SHA256 values of the three evidence files |

Existing evidence is never overwritten. The aggregate bound is `--max-bytes`
(64 MiB by default, allowed 64 KiB..256 MiB), including reserved receipt space.
An interrupted/failed output may have record lines without a subsequent page
commit; only completed commits and the receipt cursor acknowledge stored pages.
Preserve incomplete evidence for diagnosis. Snapshots are separate from raw
samples and cannot be used as substitute trajectory frames.

The boot, schema and image/configuration identity are checked and retained
without normalization. An archive's empty Git identifier with `unknown` state
remains empty and produces an explicit identity-quality issue. Source hardware
generation zero is valid when its known flag is set. Clock-failure attempts can
have end before start and unknown duration; their original timing is preserved.
Rejected attempts have no fabricated IMU axes. Private, invalid or overflowing
touch observations may be explicitly redacted with no invented coordinates.

`collection_completed` describes execution, not continuous sensor data.
Retention gaps, source rejection, generation changes, invalid measurements and
unknown identity are visible separately in the receipt. Exit zero means the
bounded collection completed, even if its data-quality receipt has issues.
`physical_motion_qualified` is always false. Neither wire completeness nor an
available sensor establishes pointer direction, accuracy or human usability.

USB leaves DTR and RTS inactive before opening the port. It never toggles them
to improve a result. The pinned TinyUSB default permits overwriting unread TX
while DTR is inactive; checksum/request identity verification detects incomplete
or conflicting responses, but loss-free physical throughput is not established.
The journal has finite retention and other logs/readers compete for transport.
Do not interpret an offline throughput regression as a device qualification.

## Offline validation

```sh
python3 tests/sensor_capture/run.py
```

Tests compile the production `app_sensor_api`, `sensor_wire`, journal and ST1
codecs with strict warnings and sanitizers. Cached board/SDK, serial-driver and
HTTPS socket delivery are named substitutes. Real collector logic verifies
production-generated bytes, units, redaction, nonfinite values, clock failures,
u64 boundaries, gaps, corruption, short writes, partial/interleaved/duplicate
frames, retries, disconnects and identity changes. The serial substitute models
pyserial's wait-for-count behavior; the collector waits for one byte and then
reads only a bounded available snapshot. The HTTPS tests use the real pinned
transport/session with synthetic private credentials and a socket substitute.
No test opens an actual device, network connection or user Keychain.
