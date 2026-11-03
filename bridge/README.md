# Authenticated control companion

The companion runs one local stdio MCP peer per authenticated device session. A second controller needs its own process/session; identities and input leases are never multiplexed implicitly. All tools come from [`api/control-v1.json`](../api/control-v1.json); catalog mismatch fails closed. Accepted work is not device or host completion.

Use Python 3.12 in a project-local environment:

```sh
python3.12 -m venv .tools/control-bridge-venv
.tools/control-bridge-venv/bin/python -m pip install -r bridge/requirements.lock
.tools/control-bridge-venv/bin/python tests/control_protocol/test_bridge.py
```

Dependencies are pinned, including the official [MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) v1.30.0. Tests substitute Keychain, HTTP and serial boundaries and exercise the real MCP stdio handshake. No test opens a serial device or writes the user's Keychain.

## Local enrollment

Create a new private directory **outside the repository**, with explicit minimal scopes. The command creates the device TLS identity, a random native client credential, and opaque principal/grant IDs. The client credential goes to macOS Keychain; only its SHA-256 verifier enters the device package. No default credentials, automatic discovery, HTTP downgrade or TOFU are used.

```sh
.tools/control-bridge-venv/bin/python bridge/commission.py create \
  --output '<private-parent>/device-owner' --host '<device-hostname>' \
  --scope view --scope wifi.configure --scope audio.capture \
  --time-server '<approved-time-server>' --grants '<private-grants.json>'
```

`--grants` is optional. Its JSON array contains source `local_microphone`, purpose `speech`, `live_capture`, `recording` or `background_recording`, finite `expires_unix_seconds` as a decimal string, decimal-string `maximum_bytes`, integer `maximum_duration_seconds`/`maximum_retention_seconds`, and Boolean `live_destination`/`file_destination`/`owner_download`. The tool issues `id`, `principal_id` and revision. Speech is 1–8 seconds; live capture is at most eight hours/eight GiB. File-only continuation requires its own reviewed background recording grant. Enrollment never starts capture, unmutes a role or resumes old work.

Optional `--update-source` and `--update-policy` accept explicit JSON files. A source defines `catalog_url`, `tls_ca_pem`, P-256 uncompressed `signing_public_key` as 130 lowercase hex characters, `signing_key_id`, `product`, `board`, layout `esp32s31-16m-ota6-v3`, integer `minimum_release_index`, and Boolean `allow_private_source`. The catalog and its signed artifact must share that HTTPS origin; redirects, credentials/query strings, loopback and link-local destinations are rejected. Private address sources require the explicit policy flag.

An update policy defines decimal-string `expires_unix_seconds`, integer `maximum_job_ms` (1,000–600,000) and `maximum_bytes` (up to six MiB), and Boolean `allow_download`, `allow_activate`, `allow_restart`, `continue_after_owner_loss`, `allow_signed_releases`. If signed releases are not broadly delegated, `allowed_digest` pins one SHA-256. Scopes must independently allow every delegated action; restart additionally requires `system.restart`. IDs and revision are issued by the tool. Query `/api/v1/firmware/policy` as that principal to obtain its actual policy ID/revision. An HTTP request deadline bounds admission; the immutable policy separately bounds the asynchronous job.

The package is limited to 8,192 bytes. It holds at most four principals and eight capture grants, with parser/token and NVS budgets still enforced. Optional `--replace-unknown-store` requests explicit local Review of replacing unreadable **control trust banks only**. It never erases the NVS partition or other namespaces. `--export-client-secret` explicitly creates an additional private backup; no backup is exported by default. Keep the private package and any backup outside Git. A failed Keychain import retains the private files for deliberate recovery and reports failure.

On the device, open the enrollment window locally while unlocked. A caller may then explicitly send the package over the existing CDC interface:

```sh
.tools/control-bridge-venv/bin/python bridge/commission.py send \
  --package '<private-parent>/device-owner/device-package.json' --port '<device-port>'
```

This command opens the selected serial port and is a separate device action. It uploads bounded numbered chunks and returns `pending_local_review`. Review the host, certificate fingerprint, each principal's identity/scopes/native/browser status, grant bounds and update source/policy on the device, then approve the exact candidate digest locally. USB cannot approve, open a presence window or issue a reset. Windows expire after two minutes. No physical commissioning command is part of host validation.

The `control` NVS partition is separate from existing Wi-Fi/bond storage. TLS material and trust use versioned checked banks. A failed write may already have applied and remains uncertain until verified/replaced. At-rest protection is reported from actual partition status; a private partition label is not encryption evidence. Existing devices without the approved partition table require a separately authorized migration. Nothing here performs that migration.

## MCP connection

Configure an MCP stdio server with this interpreter, script, and public profile:

```sh
.tools/control-bridge-venv/bin/python bridge/control_bridge.py \
  --profile '<private-parent>/device-owner/client-profile.json'
```

TLS checks the CA path, hostname and exact enrolled leaf fingerprint. Native session tokens stay in memory, never URLs or tool results. Tool arguments use `{ "context": { ... }, "args": { ... } }`; copy current management/resource or input/UI identities from `state_get` before mutations. The device validates scopes, bounds, state and actual capabilities again. The companion does not acquire/renew an input lease automatically. It sends session heartbeats while its stdio peer is connected, without renewing input ownership. EOF closes the logical session; missing logout acknowledgement remains unknown and server expiry still applies.

After an unknown network outcome, `bridge_retry_pending` reuses the identical request/envelope. A fresh request or session is never used to replay it. Device result eviction returns unknown, not success. Separate session loss, input takeover, lock and media grant revocation remain separate boundaries.

One controller reuses its verified HTTPS connection until session closure or a
transport failure. Exact `busy` responses to GET are polled at most four times
with 20/40/80 ms backoff so the coordinator can publish its bounded fresh read.
Commands and failed connections are never automatically replayed. This preserves
the device's read-cache freshness limit instead of relying on a new TLS handshake
fitting inside it. Use the [LAN diagnostic collector](../tools/lan_debug.md) for
bounded status evidence and the [sensor collector](../tools/sensor_capture.md)
for ordered raw sensor observations.

## HTTP and media

The station-only HTTPS server has no provisioning-AP route. OpenAPI is generated in [`api/openapi.json`](../api/openapi.json). GET `/api/v1/sessions/current` is an authenticated heartbeat; media operational liveness expires five seconds after authenticated client activity. `/api/v1/capture/grants` returns only the principal's enrolled grants. Clock status stays unknown after boot until a real SNTP callback and exposes unauthenticated SNTP quality separately.

WSS `/api/v1/channel` requires authentication **before** upgrade. Enable `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`; without it WSS is unavailable. Native clients send their Bearer header. Browsers first send an empty `POST /api/v1/ws-ticket` with `Content-Type: application/json`, the secure session cookie, exact Origin and `X-CSRF-Token`. The returned 64-hex ticket is bound to that session, replaces its prior ticket, expires after 10 seconds and is consumed before upgrade. Pass subprotocols `esp32-control-v1` and `ticket.<ticket>`; never put it in a URL or log. Reusable CSRF values cannot authorize a WSS upgrade. Each opened channel has a new internal generation. Closing it revokes its input lease/queued input and exact live network media, including a producer with no queued PCM. The same logical HTTP session, independent management and file recording keep their own liveness and grants. A replacement channel requires a fresh input acquire; old callbacks or queued PCM cannot bind to it. Text frames carry the same command envelope, `{ "v": 1, "type": "heartbeat" }`, or `{ "v": 1, "resource": "/api/v1/state" }`. Fragmented/oversized frames are rejected. Send a heartbeat at least once per second while owning media.

Binary PCM uses the bounded [`ACM1` codec](../components/control_protocol/include/control_media.h): a 64-byte big-endian header naming job, audio/consumer generations, sequence, sample offset and actual format, followed by complete interleaved signed 16-bit little-endian PCM. Payload is at most 1,920 bytes; frame sizes are 10 or 20 ms. The audio owner revalidates every live job/consent and frame. The server retains at most two outgoing frames, expires queued data after 100 ms and reports failed egress to the audio owner. Acceptance does not claim socket/host receipt.

GET `/api/v1/recordings/<32-hex-id>/content` serves actual WAV bytes through the existing asynchronous media/store owner. It requires both recording read and export scopes, the owning principal, completed verified data and `owner_download`. Range requests require the exact strong ETag in `If-Match`; suffix/open-ended single ranges are supported, multipart ranges are rejected. The server checks authentication before/after each bounded read and closes an interrupted response without a completion marker. This route never substitutes JSON samples for a download or reads flash on the network/coordinator task.

## Offline releases

Create a publisher key outside the repository, then sign the **application** artifact, not a full flash dump:

```sh
.tools/control-bridge-venv/bin/python bridge/sign_release.py keygen --output '<private-parent>/publisher.pem'
.tools/control-bridge-venv/bin/python bridge/sign_release.py sign \
  --image firmware/build/esp32_input.bin --private-key '<private-parent>/publisher.pem' \
  --output '<release-parent>/catalog.json' --key-id '<reviewed-key-id>' \
  --product '<reviewed-product>' --board '<reviewed-board>' \
  --release-id '<release-id>' --release-index 1 \
  --url 'https://<approved-origin>/<image.bin>' --expires '<unix-seconds>'
```

The signed payload binds full SHA-256/size, version/project extracted from the image, chip, board/product, settings schema, layout, release index, artifact URL and a finite expiry of at most seven days. ECDSA uses P-256/SHA-256 and an exact 64-byte `r || s` signature. This publisher signature is separate from optional SDK secure boot signing; no fuse changes occur.

V3 has two six-MiB OTA slots (`0x10000`, `0x610000`), otadata `0xc10000`, model `0xc20000`, control `0xf20000`, recordings `0xf30000`. NVS/PHY keep their original addresses. It has no factory slot: the earlier factory-only layout is rejected and the external original backup remains the recovery artifact. Ordinary OTA only writes an actual inactive OTA slot; staging does not select boot. Activation rechecks the exact policy and maintenance barrier, verifies the slot again, records a release floor, selects boot and restarts. Selection uncertainty is not retried automatically. `CONFIRM_BY_APP` rollback and continuous local health proof prevent early confirmation; boot does not resume downloads/capture/input jobs. Source/host/target tests do not establish physical compatibility or release qualification.

The MCP companion checks canonical wire formats and UTF-8 byte limits before authentication; malformed arguments never become a device command. Device-side validation and current manager authority remain mandatory.
