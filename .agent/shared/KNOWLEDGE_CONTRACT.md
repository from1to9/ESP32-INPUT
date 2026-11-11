---
scope: repository-shared
portability: cross-checkout
status: active
verified_at: 2026-10-21
evidence_kind: engineering-governance
privacy_reviewed: true
contains_local_runtime_facts: false
---

# Knowledge and evidence contract

The [validation summary](../../docs/validation.md) records the scope and limits
of public compatibility claims.

## Storage

Tracked documentation contains portable requirements, interface contracts and procedures. Keep prototypes, investigations and experimental code local. `.agent/local/` contains resolved paths, logs and machine-specific investigation notes. Tool caches and build directories remain local. Do not copy raw sessions, personal memory or local run directories into Git.

## Promotion

Promote a claim only when useful across compatible checkouts, supported by current code/tests/official contracts or repeated evidence, scoped with invalidation conditions, deduplicated, and reviewed for private or local data. Use relative paths or placeholders such as `<repo-root>` and `<device-port>`.

Imported knowledge needs reverification; it is not authority to overwrite observations. Merge by meaning, retain material conflicts and identify superseded guidance. Historical commits document provenance but must not be presented as current HEAD without checking.

## Evidence and authority

Platform instructions and the user's current authorized scope govern actions. For technical facts, prefer current code and artifacts, then official versioned contracts, current tracked documentation, verified shared knowledge, and finally imported or historical notes. Keep contradictions visible until investigated. Past success does not prove the current checkout, environment or device state.

## Privacy and locality

Do not commit credentials, tokens, pairing keys, private addresses, personal identifiers, recordings, transcripts, raw conversations, absolute home paths, process identifiers or transient operational state. Public upstream names and deliberate repository provenance are allowed when relevant. Compatibility evidence may include non-personal device/OS versions and build identifiers; raw captures and private host details stay local.

When a durable claim changes, update its verification date and scope. Retire obsolete guidance with a replacement reference when its history explains a relevant risk. Knowledge synchronization never authorizes deletion of local files, memories, skills or durable data.
