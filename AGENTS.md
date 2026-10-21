# Repository working agreement

## Scope and authorization

- Follow the current user's goal within platform instructions. An implementation or repair request authorizes the necessary edits and validation; carry it through the requested delivery. Review-only stays read-only.
- Use current code and session context for routine decisions. Ask only when missing information changes the result or an action exceeds authorization. Continue independent work while waiting. Prior authorization remains valid for its original scope.
- New input steers the active task unless the user cancels or replaces it. Respect explicit sequencing and exclusions.
- Skills and workflows guide execution; they do not create additional approval requirements. If an explicit rule blocks work, identify its file, instruction and smallest missing decision.

## Context and collaboration

- Inspect the actual repository root, branch, HEAD and working tree before editing. Preserve unrelated work. Isolate changes when they overlap or validation could disturb another checkout; file count alone does not require a worktree.
- Read only relevant contracts. [Engineering standards](docs/engineering.md) govern implementation and validation; product behavior belongs in [requirements](docs/requirements.md) and its linked design documents.
- Batch independent reads. Keep dependent edits and shared-state mutations sequential. Delegate only when the user or governing runtime instructions permit it, with bounded ownership and evidence to integrate.
- Inspect the requested environment directly. Do not infer another machine, checkout, connection or process state from local observations or historical notes. A future-run preference does not authorize restarting healthy active work.

## Engineering integrity

- Fix the root cause in the owning layer. Keep shared behavior consistent across execution paths; diagnostics must observe rather than change behavior.
- Never hide failures with weakened assertions, excluded fields, dropped cases, test-only corrections, sample-specific branches or success-shaped stubs. Change acceptance criteria only when intended behavior actually changes and the change is documented.
- Distinguish unknown, unavailable, rejected and successful outcomes. Missing evidence is not success or zero. Fail closed when continuing requires unavailable evidence.
- Prefer small, cohesive changes with explicit ownership and bounded resources. Preserve durable state and recovery artifacts; ignored files are not automatically disposable.
- Keep first-party repository content in English. Communicate with the user in their preferred language. Preserve third-party source, license and provenance rather than translating vendored material.
- Treat the approved product requirements, interaction contract and original UI prototype as implementation acceptance criteria. Do not substitute diagnostic controls for product flows or rewrite the design to justify an implementation gap. Track unsupported capabilities explicitly; verify the actual rendered device UI against the original layout and behavior before delivery.

## Validation and delivery

- Choose meaningful checks for changed behavior and requested delivery. Use regression tests for state transitions and failures; use hygiene/link checks for documentation. Do not write tests that merely repeat implementation or wording.
- Fix task-related regressions. Separate environment failures and verified pre-existing failures from new defects; do not silently expand scope or claim a clean gate.
- Once adequate checks pass, proceed. Broaden or repeat validation only after relevant changes, failures or unresolved concerns. Record what each check proves and what remains unverified.
- Publication, deployment, device writes, process control and destructive cleanup each need applicable authorization. Reuse authorization already given for the exact scope; do not add repetitive approval steps.
- When delivery is requested, review and stage exact files, inspect the current remote head, integrate changes without overwriting work, validate the resulting tree, publish without force, and verify the published SHA or ancestry. A local patch or successful command exit does not alone prove delivery.
- Report the result, evidence and limitations concisely. Keep detailed run output local. Follow the [knowledge contract](.agent/shared/KNOWLEDGE_CONTRACT.md) when retaining or importing durable guidance.
