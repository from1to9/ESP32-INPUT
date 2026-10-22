# Engineering standards

Status: active. Established 2026-10-21. These standards govern first-party engineering work; functional behavior is defined in [requirements.md](requirements.md).

## 1. Language and source ownership

All first-party comments, identifiers, documentation, UI strings, diagnostics, tests, commit messages, issue text and review descriptions use English. Conversation may use the user's preferred language. Future localization must be an explicit, isolated resource layer rather than mixed-language source.

Third-party submodules, license texts and required attribution retain their original content and are excluded from first-party language enforcement. External names, URLs and standards identifiers remain exact. Do not translate upstream source to satisfy a local style rule.

Use descriptive names and consistent existing style. C and Python use four-space indentation; web assets and structured configuration use two spaces. Use UTF-8, LF endings, final newlines and no trailing whitespace. Unicode symbols are allowed where useful; English does not mean ASCII-only.

## 2. Architecture and implementation

- Put deterministic domain behavior in portable components. Keep hardware access, transport, UI and application orchestration in separate owning modules.
- Represent transitions explicitly. Define state ownership, mutation rights and how asynchronous events are rejected after cancellation or replacement.
- State units, ranges, timing source, capacity and failure behavior. Validate external values at the boundary; avoid unexplained constants and silent truncation.
- Use bounded buffers and queues. Do not block latency-sensitive callbacks on UI, storage or long computations. Define backpressure and cancellation before adding producers.
- Keep observation behavior-neutral. Test production algorithms; hardware substitutes belong at documented boundaries, not inside domain logic.
- Return truthful status. Acceptance, queueing, transport acknowledgement and observed target outcome are different events.
- Preserve the user's working tree and durable data. Avoid unrelated rewrites, cleanup and dependency upgrades.

## 3. Dependencies and reproducible builds

- Prefer official, maintained sources with an explicit compatible license. Record the source and rationale for alternatives.
- Pin SDK commits and submodule revisions. Commit lockfiles after resolution. A floating branch alone is not a reproducible build definition.
- Provide repository-relative setup/build commands and version checks. Do not hardcode a contributor's home directory, host, interpreter or credentials.
- Keep toolchains, downloaded dependencies, build outputs and local evidence outside tracked source. A clean checkout must reproduce builds from tracked instructions and pins.
- Scope tooling changes to this project unless a shared installation is requested. Do not silently rewrite global shell or Git configuration.
- Separate setup, build, tests, packaging and deployment. Build helpers must not implicitly deploy, flash or erase a device.

## 4. Validation by impact

| Change | Starting evidence | Extend when |
| --- | --- | --- |
| Documentation and policy | Diff hygiene, local links, language and source boundaries | Executable instructions changed |
| UI copy or layout | Syntax and focused visual/interaction checks | Behavior or accessibility changed |
| Portable behavior | Meaningful host regressions and strict compiler diagnostics | State or failure handling spans components |
| Firmware integration | Host tests and target build with pinned SDK | Device, timing or protocol claims require hardware |
| Protocol or asynchronous state | Schema, interruption, stale-event and failure checks | Host-stack behavior cannot be reproduced locally |
| Release or deployment | Evidence for the exact delivered tree and artifacts | The delivery contract requires hardware qualification |

Use sanitizers where supported. Test invariants, numerical boundaries and recovery; avoid tests that mirror implementation line for line. Never relax checks just to obtain a pass. If a requirement changes, document the new expected result.

For product UI work, use the original `design/` prototype and [interaction contract](interaction.md) as the baseline. The prototype was introduced in commit `21c30f1`; its simulated host, recognition and audio state must be replaced by truthful runtime capability and connection state, while preserving its navigation and input workflows. Diagnostic screens do not satisfy the product UI acceptance gate. Inspect actual LVGL renders at 480 x 480 and exercise the corresponding production interaction paths. Record feature-level gaps without changing the approved baseline to match incomplete code.

Run `python3 tools/check_repository.py` for first-party text hygiene and local Markdown links. Its CJK check prevents the previous mixed-language regression; it is not a complete language detector. Review wording manually. Run `python3 tools/check.py` for portable C behavior.

Distinguish source-reviewed, host-tested, target-compiled, device-tested, host-compatible and release-qualified evidence. One level does not establish the next. Record SDK/dependency revisions, target and configuration for build evidence; record actual board and host versions for compatibility. Do not invent latency, stability or interoperability results.

For coordinator stack changes or growing snapshots, run
`python3 tools/check_stack_budget.py` after configuring the pinned target build.
It uses that build's compile database, writes isolated objects and stack-usage
evidence outside the shared build directory, and checks the declared startup,
render, polling and management callback paths. Count GCC split/clone bodies as
well as public entries, including nested owning adapters and backend leaves.
It rejects drift in the measured first-party sources and headers, compile
database, configuration and command response files during measurement; the
report states the exact hashed input scope. The startup frame limit is 512 bytes; the known path
sums must leave 2 KiB for SDK, library, RTOS and interrupt overhead. This reserve
is an engineering budget, not a measured bound on opaque SDK or device usage.
Keep runtime-only frames out of startup, and give large copied observations
bounded storage with explicit single-task or serialized ownership. Distinct
nested workspaces must not overwrite pending requests; clear transient private
payloads after their owning consumer has copied them. Target stack reports do
not replace device high-water and failure evidence.

Stop expanding a successful run without a specific unresolved concern. Revalidate after substantive edits or integration changes. Documentation changes do not require unrelated hardware tests.

## 5. Git and delivery

Use focused commits named `type(scope): concrete outcome`, for example `feat(hid): add a composite report descriptor`. Types include `feat`, `fix`, `refactor`, `test`, `docs`, `build` and `chore`. Branches and worktrees provide isolation; they are not mandatory ceremony for every edit.

Review exact diffs and stage intended paths. Inspect the current remote head before publication. Integrate concurrent work and revalidate; do not force-push over it. Follow branch protection if a pull request is required. Verify the destination commit or containment after publishing.

An authenticated connector may publish when terminal Git credentials are unavailable. Verify its tree against the local staged tree and align local history to the actual published commit. Do not expose credentials to enable a second access path.

Code synchronization does not imply a release, package publication, server restart or device deployment. Follow the user's delivery boundary exactly.

## 6. Knowledge and evidence

Keep durable decisions in tracked documentation and machine-specific logs in `.agent/local/`. Store no credentials, pairing secrets, private recordings or personal transcripts in Git. Review examples for private data. Follow the [knowledge contract](../.agent/shared/KNOWLEDGE_CONTRACT.md).

Historical notes are leads, not current evidence. Reverify cheap, changeable facts. Mark uncertainty and preserve material conflicts until resolved. Link to authoritative definitions rather than maintaining copies that can drift.

## 7. Completion contract

Complete only the explicitly delivered scope: intended behavior is implemented, relevant checks pass or limitations are evidenced, documentation matches the result, unrelated work is preserved, and requested publication is verified. Scaffolding, mocks or a successful build do not establish a working device.

Report what changed, why it matters, how it was checked and what remains unverified. State blockers precisely without disguising partial progress as completion.

## Adaptation source

These general principles were adapted from the current QuantTrade `AGENTS.md`, knowledge contract, Git delivery workflow and validation workflow, inspected on 2026-10-21. They retain persistent authorization, scoped edits, provenance, root-cause fixes, meaningful validation and verified delivery. Trading semantics, optimizer rules, machine names, fixed worker counts, data paths and QuantTrade-specific commands are excluded. There is no runtime dependency on that checkout.
