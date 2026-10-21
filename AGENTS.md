# ESP32-INPUT working rules

- Read `docs/requirements.md`, `docs/interaction.md`, and `docs/roadmap.md` before changing behavior.
- Preserve the three main modes: mouse, keyboard, audio. Boot into mouse with gyro off until explicitly enabled.
- Motion character selection and spoken letters belong to keyboard mode. Never emit host pointer movement while selecting characters.
- Keep audio configuration separate from foreground input mode. Parallel HID/audio is a measured capability, not a promise inferred from independent examples.
- Use the pinned official Mosaico BSP. Do not guess board revision or wire shared resources with raw GPIO values.
- Do not copy unlicensed M5 source. Cite behavioral references; write original implementations or use explicitly licensed components.
- Keep hardware-independent behavior in `components/input_core`. Test transitions, stale input rejection, motion integration and actual HID encoding.
- All mode/host/disconnect/lock boundaries must release held keys, cancel text queues and invalidate stale input. Never replay old clicks after reconnect.
- Do not send voice recognition results directly as Enter, app submissions or remote approvals. Preview text first; Enter is a separate user action.
- Do not describe a prototype, host test or SDK build as a successful hardware test. Record exact board, OS, SDK SHA, transport and evidence.
- Do not flash a connected device or erase its data merely to validate a documentation or UI change.
- Avoid adding accounts, secrets, pairing keys, recordings, personal transcripts or large SDK/build outputs to Git.
