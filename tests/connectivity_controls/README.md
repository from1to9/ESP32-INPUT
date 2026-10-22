# Connectivity coordinator regressions

Run `python3 tests/connectivity_controls/run.py`. Sanitized C11 tests compile the
production helper and the real scope registry, substituting only cached owner
getters and nonblocking submission APIs. They do not exercise radio, USB, NVS
or device writes. The helper creates no worker, initializes no service, and
leaves Classic audio selection in the existing media helper.

Cases cover exact epoch/UI/resource revisions, preservation of legacy and
parent-only routing, eight-profile complete reorder, observed DHCP versus join
acceptance, immutable single-profile forget confirmation across parent overlay
epoch changes, actual BLE fingerprints, durable request IDs, uncertain outcomes,
unknown/current/disconnected row evidence, bounded API review/navigation,
actual TLS service readiness, timeouts and rejection without automatic retries.

Commissioning approval requires every page of the complete owner-provided
principal/capture/update authority summary and the exact proposal digest. A
revision-only trust-store update is reported as exact identity unconfirmed;
the owner's active digest must match the approved package before success.
Long/invalid/incomplete summaries disable approval instead of truncating it.
Parent main owns physical-origin and remote-input allowance checks and fills
remote-input fields after the helper; neither TLS nor media enables that gate.

The 14 cases include complete ABI-2 disclosure of the HTTPS port, unauthenticated
SNTP servers, source CA fingerprint, private-source permission, minimum release,
product/board/layout and all principal, capture and update policy fields. A
minimal enrollment uses 3 pages; a one-principal/grant/source/policy example uses
10. The public metadata captured from a parser-accepted 7,735-byte synthetic
enrollment (four principals with all 25 scopes and eight grants) uses 46 pages.
The separate public-array/numeric boundary fixture uses 53; it is not asserted
to be an admissible serialized package. Neither fixture contains credentials.
The trust suite owns package parsing and cryptography evidence.

The fixed ceiling is 64 pages of five 40-character lines. Sequential adjacent
navigation, every-page visitation, changed authority under an unchanged digest,
stale approval, missing/nonterminated fields, exact 64-bit visitation and a
65th-page writer overflow are exercised. Overflow disables approval. Native
LVGL composition in `tests/ui` additionally renders and touches every actual
page in both themes; helper-only tests do not prove screen fit.

Bond refresh copies directly into the retained cache, avoiding a large returned
structure. A separate 1,344-byte coordinator-only PSRAM request workspace is
cleared before building each BLE preference submission and wiped on return,
including rejection. The full-capacity reorder/rejected-request/shorter-select
sequence verifies exact revisions, peer identities, order and zero unused tail.
The target refresh frame is 176 bytes (previously 1,616); the preference handler
is now inlined into the 80-byte outer handler instead of a separate 1,424-byte
frame. Transport submission and durable completion rules are unchanged.
