# Browser controller regressions

`node tests/web_controller/test_client.mjs` runs the production client module
without a DOM. It covers whole-input US ANSI rejection, typed envelopes,
sequence and CSRF options, retained progress, no replay of an unknown command,
and owner/target/readiness changes.

For the focused delayed-pad regression, pass `--case pad-freshness` to `run.py`.
It pauses the real browser's state GET, ends, cancels, expires or replaces the
contact, then resumes that GET and checks that no old move POST or HID report
appears. Fresh movement on an ongoing/new contact, movement released only after
POST, direction buttons and discrete clicks retain their existing behavior.
The client tests additionally cover every asynchronous renewal/preflight stage,
the original sample deadline and lost replies after actual submission. Dropping
an unsubmitted sample does not consume a command sequence or revoke the lease.

`run.py` uses a real Chromium browser and real loopback HTTPS. Static responses
come from production `app_control_web_serve`; dynamic requests use the actual
`header`, `error_response` and `handle_request` functions extracted unchanged
from `app_control_server.c`, then production `cp_http`, access manager,
coordinator, command bridge, remote input adapter and HID engine. The existing
coordinator harness supplies SDK boundaries; no input decisions are rewritten.
The server worker's state-before-completion ordering is preserved.

Only the network socket/TLS listener, NVS, trusted clock, trusted local approval
and actual HID send APIs are substitutes. A separate loopback-only fixture
route injects approval, local takeover, target replacement and loss of a reply
after production admission. These fixture routes are absent from firmware.
The test checks actual accepted HID bytes and down/neutral pairs, but does not
claim physical radio/USB delivery, host focus or target application effects.
Pairing uses the actual protected access manager and protocol; it never borrows
a preloaded native principal or replaces protocol authorization with a mock.

Install Playwright in an ignored local directory or provide an existing module
and Chromium executable. For example, from the repository root:

```sh
npm install --prefix .agent/local/browser-tools playwright@1.58.2
python3 tests/web_controller/run.py \
  --playwright-module .agent/local/browser-tools/node_modules/playwright/index.mjs \
  --output-dir .agent/local/browser-evidence
```

The matching browser must be installed for that Playwright version, or pass
`--browser-executable /absolute/path/to/chromium`. During separate owner work,
`--protocol-root /path/to/permissions-checkout` reads and compiles that owner's
actual protocol/access sources without modifying its checkout. An integrated
tree needs no override. Retain the manifest and fixed source revisions for such
a cross-checkout run; it does not prove an independently built combined image.

The runner binds only `127.0.0.1`, creates a temporary one-day localhost
certificate, and accepts that certificate only in its isolated test browser.
This exception does not alter the product's TLS policy or the user's browser.
Desktop and mobile contexts check cookie/CSRF/CSP behavior, actual mouse and
touch Pointer Events, onscreen keys, full-text rejection, partial/unknown
outcomes, local takeover, target replacement, saved approval without an active
lease and independent same-name browser identities. HTTP evidence records
method/path/operation/header size, never credentials or session cookies.
