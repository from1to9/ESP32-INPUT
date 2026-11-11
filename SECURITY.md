# Security policy

ESP32-INPUT is development firmware. Report suspected vulnerabilities against
the current source and include the exact commit or the build identity shown
by the device. Published security maintenance versions have yet to be defined.

Use the repository's **Security → Report a vulnerability** form when available.
If the form is unavailable, request a private contact method through a public
issue with a neutral title and no technical details. Share the affected
interface, prerequisites, impact and a minimal reproduction privately.
Exclude credentials, private keys, device addresses, recordings and flash dumps.

Keep the control listener on a trusted LAN. Use the enrolled HTTPS identity
and grant each browser or native client only the permissions it needs. Store
commissioning bundles, client credentials and publisher keys outside Git.
Use the board's local review screen to inspect new grants. See
[companion enrollment](bridge/README.md#local-enrollment) and
[browser approval](docs/browser-controller.md#connect-and-take-control)
for revocation and enrollment behavior.

Bluetooth Just Works pairing provides encryption without protection from
an active man-in-the-middle during pairing. Open an enrollment or Bluetooth
pairing window only while you can review the intended connection.
