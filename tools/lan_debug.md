# Authenticated LAN diagnostic collection

Use an already enrolled public client profile from the
[control companion](../bridge/README.md). A Wi-Fi address alone does not enable
the HTTPS service. The profile checks the CA, hostname and exact certificate;
its native credential stays in macOS Keychain. No discovery, insecure fallback,
automatic enrollment, input, capture, firmware command or device reset occurs.

```sh
.tools/control-bridge-venv/bin/python tools/lan_debug.py \
  --profile '<private-parent>/client-profile.json' \
  --output '.agent/local/lan-probe'
.tools/control-bridge-venv/bin/python tools/lan_debug.py \
  --profile '<private-parent>/client-profile.json' --seconds 60 \
  --interval 1 --output '.agent/local/lan-capture'
```

The diagnostic bundle needs `view`, `diagnostics.read` and one of
`update.check`, `update.stage` or `update.activate` for the firmware-state read.
It does not require an update policy merely to read status. Keep credentials and
publisher keys outside the repository. Additional mutation scopes are unrelated
to diagnostic read permission.

The tool records the sensor catalog once, then reads protocol/device state,
`/api/v1/diagnostics/runtime`, firmware state and clock status. The runtime
resource is a bounded copied observation of the current owners, not arbitrary
raw serial text or a historical event archive. Use [sensor_capture.py](sensor_capture.md)
for ordered high-rate sensor records and [device_smoke.py](device_smoke.py) for
USB console evidence. Missing providers and unknown fields stay explicit.

The companion reuses one verified HTTPS connection per controller. Coordinator
reads can initially return `busy` while the owner prepares a snapshot; only GET
requests with that exact result are polled, at most four attempts with bounded
backoff. The firmware's 250 ms freshness rule is unchanged. Mutating requests,
authentication failures, dropped connections and unknown outcomes are not
automatically replayed. The connection closes when the session closes.

`--seconds 0` performs one probe. A collection window is at most 600 seconds;
the interval is 0.5..60 seconds. Each blocking socket operation has a four-second
timeout; this is not a total request deadline. A busy read has at most four
requests plus 140 ms backoff. Admission, an in-flight
round, final identity check and session close can extend the window. Slow reads
do not cause concurrent collectors or a catch-up burst. The tool never renews
an input lease.

Two new mode-0600 files are created without overwriting existing evidence:

- `.jsonl`: GET response bodies, HTTP status, timing and read-attempt count.
  Login credentials, session responses and authorization headers are excluded.
- `.receipt.json`: image and boot identities, completion/error, observed HTTP
  statuses, issues, byte count and the data SHA-256. The telemetry boot identity
  is separate from the authenticated protocol boot identity.

The default output budget is 8 MiB, configurable between 64 KiB and 16 MiB.
Budget exhaustion, interruption, session loss and boot changes preserve partial
evidence and report incomplete collection. A resource error remains in the
receipt even if a later poll succeeds. `all_resources_observed` requires every
read to succeed, but does not claim that every subsystem is healthy. Physical
input/audio, endurance and OTA rollback remain separate qualification.
Exit zero means all resources were observed; otherwise inspect the receipt.
The tool makes no claim that a lost logout response was acknowledged.

Offline regression tests use real session and collection code with HTTP,
Keychain and time boundaries substituted:

```sh
.tools/control-bridge-venv/bin/python tests/test_lan_debug.py
.tools/control-bridge-venv/bin/python tests/control_protocol/test_bridge.py
```
