# Wi-Fi management and signed updates

Wi-Fi station operation is independent of USB or Bluetooth input. Join a LAN
with the [device setup workflow](wifi-provisioning.md). The device remembers up
to eight ordered profiles. A station IP address shows network connectivity.
The HTTPS listener also needs an installed TLS identity, local enrollment
and authorization for the caller. The recovery hotspot handles setup only.

## Signed update workflow

The [companion guide](../bridge/README.md#offline-releases) documents signing
and serving a catalog. Use the built application image for the pinned board,
chip, settings schema and layout. Protect the publisher key separately from
the device certificate and client credentials.

1. Enroll the trusted publisher, source and an appropriate update policy using
   [local commissioning](../bridge/README.md#local-enrollment).
2. Request download from the authorized HTTPS source. The policy bounds the
   publisher, artifact, job lifetime, size and permitted actions. The device
   validates the signed catalog and downloads to an inactive application slot.
3. Read the firmware job and verify that staging completed for the expected
   artifact. Download acceptance does not prove that staging completed or
   select an image to boot.
4. Request activation through the separate authorized operation. The device
   waits for maintenance and release barriers before changing the boot
   selection and restarting. During first-boot health checks, the UI permits
   browsing and pauses host output. Check the running identity and confirmation
   status after reboot.

The firmware layout `esp32s31-16m-ota6-v3` uses 16 MiB flash and two 6 MiB
application slots. The device must already have the matching partition table.
OTA cannot migrate the table or replace a full recovery backup. See the
[device procedure](../firmware/README.md#device-procedure) for first
installation and recovery.

Use `GET /api/v1/firmware/state` to distinguish active and staged images,
pending activation, confirmation and the most recent terminal failure.
Observe the job and running image after a lost reply before requesting
another action. See [validation](validation.md) for qualification gaps.
