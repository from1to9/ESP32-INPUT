# Combined delivery checklist

This checklist reconciles the accepted requirements with the current combined
implementation. The latest delivery boundary is source review and GitHub
publication after local validation. No flashing or device testing is authorized
in this stage. Earlier device observations describe their original images only.

Do not turn a proposed interface, mocked backend or successful link into a
completed function. Each row needs an implemented user path, failure handling,
focused regression evidence and independent review. Hardware qualification is a
separate, explicitly deferred result.

## Implementation inventory

| Area | Required behavior | Owning boundary | Local evidence required | Deferred physical evidence |
| --- | --- | --- | --- | --- |
| Composite input | BLE and USB keyboard/mouse; explicit transport selection, no replay or duplicate host output | HID engine, BLE/USB adapters, coordinator | Descriptor checks; release, congestion, reconnect and switch tests | iPhone/iPad/Windows/macOS input |
| Motion | Calibrated aiming basis, independent X/Y gain and direction, Recenter, no replay after pauses | Interaction, board transform | Rotation increments, varied grips, recovery and suppression | Axis signs, comfortable gain and stationary drift |
| Touchpad | Default single/double tap and held drag alongside Motion; saved movement-only option; swipe movement | Interaction, touch reader, coordinator | Raw movement paths, missing/up/stale events, refusal and release ownership | Contact behavior and feel |
| Additional gestures | Two-finger scroll/right tap only with an actual multi-contact provider; clear help and cancel | One touch reader, portable recognizer, UI | Duplicate IDs, overflow, two-to-one transition, timeout and no phantom clicks | Multi-contact sensor accuracy |
| Keyboard | Nine-cell group/letter selection, preview, explicit Send, punctuation, common keys and shortcuts | Interaction, HID engine, UI | Selection boundaries, interrupted prefix, key pairs, empty cells | Target layout and focus behavior |
| Spoken letters | Explicit listening window, Alpha-Zulu candidate, confirmation into preview, no automatic Send | Speech consumer, audio manager | Pinned model/ABI, grammar errors, stale sessions, timeout and bounded PCM | Noise/confusable-command accuracy |
| Bluetooth management | Rename, bounded multiple bonds, current peer, pairing window, forget, selected target/order | Bluetooth owner, settings/UI | Capacity, asynchronous acknowledgement, identity/privacy, reconnect and storage failures | Host privacy/address behavior and reconnect |
| Wi-Fi | Join existing LAN from scan/password UI; saved profiles/order/backoff; recovery AP secondary only | Wi-Fi owner, local private editor | Immutable scan selection, credential lifetime, legacy migration, retry and storage errors | Router/security compatibility and RF coexistence |
| Audio transport | HFP HF, A2DP Sink and USB UAC under one codec owner; explicit readiness and peer generation | Audio manager, Bluetooth/UAC, board HAL | Exact PCM formats, bounded queues, generation replacement, mute/stop and failure cleanup | Host negotiation, sound quality and latency |
| Audio coexistence | Preserve local HID while selected audio runs within resources; distinct link identities | Coordinator, transport/manager arbitration | No second radio/codec/USB stack; overload and disconnect tests | Same-host and different-host concurrency |
| Recording | Explicit capture, visible indicator, quota/duration, stop/finalize, recovery, playback/delete/export | Audio manager, durable store | WAV boundaries, partial/uncertain writes, retention, access, no auto-resume | NAND geometry/provisioning, power-loss durability |
| LAN API | Full typed operation registry, status/events, authenticated ownership, cancellation and local override | Control protocol/server, coordinator | Schema parity, auth/Origin, leases, dedupe, deadlines, stale target and backpressure | Sustained network load |
| MCP | Usable companion for published operations; trust enrollment and scoped credentials | Bridge, LAN API | Protocol/schema tests, TLS trust, disconnect/cancellation, no hidden direct HID | End-to-end client workflow |
| Updates | Authorized signed download, inactive staging, explicit installation boundary, rollback state | Update manager, partitions/trust | Artifact/target validation, short download, cancellation, slot exclusion and boot health | Partition migration, rollback and power-loss recovery |
| Preferences | Shared typed config, validated transactions, coalesced persistence, legacy preservation | Settings owner and module config | Old record fixtures, corruption, bounds, unchanged/no boot writes | Flash endurance |
| Basic UI | Compact workspaces, both themes, status, 50% brightness default, 1/5/10/30/Off timeout, wake-only gesture | Product UI, presenter, coordinator | Actual 480x480 renders, hit targets, private editor wipe, idle/hidden work and cycles | Panel/touch performance |
| Startup | Lightweight ESP32-INPUT logo, truthful progress/fault, no delay or extra framebuffer | Boot UI, coordinator | Render/teardown, invalid snapshots, fault handoff | Visible boot timing |
| Haptics and portability | Configurable bounded feedback; no vibration-induced movement; isolated board capabilities | Board/HAL, haptics, interaction | Missing hardware, cancel/disable, explicit capabilities and no BSP types in portable code | Alternative board qualification |
| Continuous operation | Bounded queues/memory, watchdog, diagnostic output without a reader, recovery | All owners | Resource ledger, saturation/failure tests and reproducible configuration | Long-duration, thermal and supply tests |

## Combined source gate

1. Reconcile the operation registry, local UI and module capabilities against
   every row above; document conditional or unavailable capabilities explicitly.
2. Freeze the complete intended file inventory and dependency/configuration
   identities. Preserve private recovery backups and exclude credentials,
   recordings, machine paths and transient evidence from Git.
3. Run meaningful host regressions, native LVGL rendering/resource checks,
   repository hygiene and the pinned ESP32-S31 build for the integrated source.
   Record failures and repair them without reducing assertions.
4. Independently review all changed code and integration boundaries. Close
   actionable findings and repeat affected checks after fixes. Match reviewed
   files and built artifacts to the final inventory.
5. Inspect remote history, commit and push without overwriting concurrent work,
   then verify the published commit/tree. This is source delivery, not a device
   release or installation.

See [requirements](requirements.md), [validation](validation.md),
[configuration](configuration.md), [API inventory](api-function-matrix.md) and
[engineering standards](engineering.md) for the governing behavior and evidence.
