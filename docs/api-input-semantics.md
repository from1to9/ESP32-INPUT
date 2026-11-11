# Input command semantics

Use the [operation catalog](../api/control-v1.json) for exact argument and
context fields. The [LAN API reference](remote-control.md) describes transport,
authentication and results.

- Mouse commands use relative X/Y, signed wheel steps and explicit button
  actions. Use the currently selected HID target and current input context.
  Touch input uses an identified contact and bounded lifetime. Expired or
  superseded movement is discarded.
- Keyboard commands distinguish a single key action, a local preview and direct
  text. Direct text is an explicit operation with 1–32 printable US ANSI
  characters. It never appends Enter. Observe completed key pairs, started
  prefix and any uncertain character before taking further action.
- Mode, target, owner, UI revision and input epoch protect context-sensitive
  actions from delayed requests. Refresh state after a transition. A retry
  uses the original request identity and unchanged envelope within the
  documented session. A changed intent needs a new request.
- Lock and local takeover revoke remote input and initiate release recovery.
  Neutral release submission and actual host receipt remain separate
  observations. A disconnect never transfers pending work to a different host.
- Recenter uses the current valid attitude and the same input authority as
  local Mouse Recenter. It clears pending model movement, preserves the
  Motion setting and leaves gyro bias unchanged.

See the [interaction contract](interaction.md) for on-device mode and control
behavior and the [aiming reference](motion-estimation.md) for units and recovery.
