# Native connection controls

Status: native panels and typed UI boundary implemented, 2026-10-21. The
coordinator must populate these views from actual connection owners and admit
their requests. Rendered fixture states do not establish integration or device
connectivity. The governing behavior remains in
[connection management](connection-management.md) and
[requirements](../docs/requirements.md).

`app_connection_ui.h` carries copied, bounded public display data. The UI never
receives a Wi-Fi password, USB import token, API client secret, private key or
original trust package. Navigation only opens a panel.

The Wi-Fi settings page opens Saved networks and Web/API. Saved networks shows
four of at most eight owner profiles per page. Join and First carry the original
profile ID, catalogue revision and page. Labels are display text, never keys.
Unknown or uncertain storage disables mutations without presenting an empty
successful list. The existing current-network Forget flow remains separate.

The Web/API panel distinguishes a local USB enrollment window from actual HTTPS
service readiness. It displays the owner URL only when known, ready and TLS are
all confirmed. A recovery hotspot cannot authorize API access. Imported identity
review shows its host, complete digest and all owner-formatted permission pages.
The ABI-2 summary includes the HTTPS listener port and unauthenticated SNTP
server names; all principal scopes and capture grants; and the update catalog,
signing identity, public-key/CA fingerprints, private-source permission,
minimum release, product/board/layout and complete update policy. Only configured
pages are visited. Storage is bounded to 64 pages of five 40-character lines;
the 64-bit visitation mask handles page 64 without shifting by 64. Every page
must be visited through adjacent navigation before local approval. Any authority
change invalidates prior visits and captured approval, even if a producer reuses
the package digest.
Nonterminated or oversized text makes the entire review unavailable; truncation
cannot authorize approval. Approval is enabled only on the final page, with
owner admission, complete content and the same immutable digest/revision.

The API page separately shows Remote input allowed/blocked. Its local toggle
carries an explicit Boolean and the parent-owned allowance generation; zero
generation is unknown and disables it. HTTPS readiness cannot enable input.
Physical takeover closes allowance, and re-enabling requires a fresh local
physical action. The parent coordinator owns this state and validates the
generation independently from the connectivity helper.

Saved Bluetooth host rows open target details. Select and Move to first retain
the full 32-byte bond identity, address/type, bond revision and preference
revision. Selection does not claim a host connection; the page explains that the
host initiates BLE HID connection. Classic audio peers are independently chosen
through the media controls and actual Classic bond catalogue.

Press-time binding is rechecked on release against current owner identities,
admission, UI epoch/revision, modal state, lock and whole-gesture wake ownership.
Removing a row clears its admitted binding. A held press can neither retarget a
replacement row nor approve a replaced trust proposal. Every action has at least
a 44-pixel hit target in the original 480 x 480 LVGL layout.

Native visual/interaction and resource checks live in `tests/ui`. Host evidence
does not establish radio behavior, trust persistence, USB import interoperability
or physical display readability. The coordinator and owner integration gates
remain separate from the UI rendering gate.


## Remote system reviews and volatile settings

The generic `APP_CONFIRM_REMOTE` panel consumes one copied immutable proposal
from `app_remote_review_ui.h`. It shows the exact operation name, opaque approval
ID, action digest, original resource revision, expiry and complete consequences.
Preference reset includes its group; resetting preferences and connections also
shows the retained-manifest digest and complete reset/retention consequences as
supplied by the coordinator: device preferences, LE bonds, Wi-Fi profiles and HID
target preferences are reset; API trust, Classic bonds, firmware/OTA, models,
recordings, unknown namespaces, NAND and backups remain, without a restart.
Complete metadata must fit the bounded
review in full. Missing, malformed, unbound or oversized content is informational
only; confirmation remains disabled. The UI does not create approval authority.

The same panel recognizes the exact `bluetooth.pairing.open`,
`bluetooth.bonds.forget_all` and `bluetooth.bond.forget` operations. The owner
supplies the complete pairing window or exact bond identity and consequences;
the UI retains the same immutable identity and expiry checks. Other operation
names remain informational. Saved Wi-Fi rows expose their existing typed Forget
request; the connectivity owner captures its exact profile for separate review.

A physical press captures the request identity and displayed consequences.
Release rechecks identity, local ownership, epoch/revision, complete content and
expiry using the owner's monotonic time plus elapsed LVGL ticks. The intent
carries the exact original identity for the coordinator to revalidate before its
local confirmation hook; a queued click cannot approve a replacement request.
Only the existing validated physical route may satisfy physical approval.
Cancel remains explicit, and a remote `ui.primary` request cannot approve this
native review. Restart/reset execution and durable results stay owner-managed.

`APP_SAVE_VOLATILE` distinguishes applied temporary settings from persisted
settings. General settings show `Temporary settings / not saved`; Bluetooth
name shows `temporary name / not saved`. The coordinator assigns this state
only from its actual effective-versus-durable comparison, after pending/error
precedence. Another native setting change cannot implicitly persist unrelated
runtime-only overrides.
