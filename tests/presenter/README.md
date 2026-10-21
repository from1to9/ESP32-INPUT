# Presentation handoff checks

Run `python3 tests/presenter/run.py`. The harness executes unchanged production
`app_presenter.c` and the real `app_view_t` definition. Only LVGL timer creation,
the monotonic clock, UI rendering and FreeRTOS critical sections are substituted.
The lock boundary uses a real host mutex; a second thread can hold rendering
open while the publishing caller continues. Strict compiler diagnostics and
address/undefined-behavior sanitizers remain enabled.

Fourteen cases cover initialization failure and explicit retry, first zero-valued
identity, 10,000 latest-value publications, idle ticks without replay, temporary
pointer lifetime, bounded text copies and NULL semantics, independently stale
epochs/revisions, equal-identity telemetry, and identity wrap rejection. Cleanup
checks inspect the module's actual static slot and scratch bytes, including old
credential tails, hidden/locked/asleep/expired setup and consumption completion.
Tests include production source for storage observation; they do not reset,
patch or replace its algorithms. Every named case starts in a fresh process.

Publication during rendering must retain the new pending snapshot without
changing the in-flight one or triggering catch-up rendering. A deliberately
paused renderer permits 10,000 subsequent publications and caller progress before
it is released. This demonstrates independence from renderer completion, not a
target latency guarantee or HID delivery. All rendering and timer creation must
occur under the simulated existing LVGL owner, outside the slot critical section.
Status observation verifies pending age survives coalescing, application latency
is distinct from acceptance, and missing publication/application remains explicit.

The presenter owns two static snapshot buffers and one startup LVGL timer. It
performs no runtime allocation, queue operation, display-lock acquisition, HID
callback or device I/O. The coordinator must initialize it under the startup
display lock, handle initialization failure, publish instead of synchronously
rendering, retain stale-intent rejection, and wipe its own temporary secrets.
Only the latest pending snapshot is retained; a new snapshot published during a
render is applied on a later timer invocation. The existing UI remains responsible
for clearing its own credentials and preserving press-time gesture identity.

This fixture does not execute the LVGL scheduler or UI renderer. The separate
[native UI suite](../ui/README.md), coordinator integration tests and exact target
build remain necessary. Device display contention, critical-section duration,
maximum presentation age and input scheduling require measured target evidence.

The display boundary tests substitute the Board receipt and LVGL event APIs,
including late completion, exact token/region/count conditions, failed begin/end,
owner heartbeat versus actual transfer time, hidden/superseded markers and
display replacement. LVGL refresh completion alone is never transfer proof.
Board observer coverage tests and actual target integration remain independent.


The revocation cases require the Board's monotonic invalidation revision. An
ordinary empty refresh does not renew or revoke unchanged-pixel proof, whereas
an overwrite revokes it even when no status caller observed the intermediate
state. Actual callback/display generation replacement forces a fresh indicator
redraw under the LVGL owner. Read-only status calls never perform LVGL work.
The native UI suite separately composes real UI, presenter, LVGL and Board code
with only the physical panel/IO and operating-system boundaries substituted.
