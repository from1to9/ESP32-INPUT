# Native boot screen

Status: implemented as an isolated LVGL module; application integration and device
qualification are separate. The startup coordinator owns readiness and faults.

## Appearance and resource ownership

The 480 x 480 screen uses the product's dark background (`#080B10`), pale-blue
accent (`#8DD8FF`), light text (`#ECF4FB`) and muted text (`#B6C4D2`). A compact
vector mark combines three input bars with a pointer outline. It is built from
LVGL objects, with no bitmap asset, additional display framebuffer, timer,
animation or artificial startup delay. Startup faults use `#FFB4AB` and the text
`STARTUP FAULT`; color is never the only signal.

The module creates one opaque, touch-blocking child of the active screen and
owns only that subtree. It does not clear the existing screen, change display
defaults, perform hardware I/O, log, retry services or claim connection success.
The existing LVGL allocation-failure policy applies; this is not an independent
out-of-memory recovery screen.

## Integration contract

The interface is [app_boot_ui.h](../firmware/main/app_boot_ui.h), implemented by
[app_boot_ui.c](../firmware/main/app_boot_ui.c). The application source list must
include the latter when integrating it.

1. After a usable 480 x 480 display exists, hold its lock and call
   `app_boot_ui_create(display)` once. A duplicate instance, null display or
   different resolution is rejected without mutation.
2. Submit actual stage snapshots using `app_boot_ui_update(&view)`. Every call
   holds the same display lock. Strings are copied from bounded arrays; stage
   must be nonempty and both strings must be terminated printable ASCII. Invalid
   snapshots leave the preceding visible state intact.
3. Use `STARTING` while local controls are initializing. Step counts are optional
   observations of the coordinator's fixed startup work, not estimated time or
   interpolated percentage. Unknown counts display `Progress unavailable`.
4. Use `FAULT` only for an observed startup failure and give a short, actionable,
   non-private reason. If hardware/display initialization fails before a usable
   display exists, this module cannot show it: retain the application's actual
   error-reporting path. Do not report a successful boot render in that case.
5. `READY` means local controls are ready. It does not assert Bluetooth, Wi-Fi,
   audio, storage, host selection or HID readiness. Known step counts must be
   complete. Optional-service failures belong in the product UI when local
   controls can function; they must not indefinitely block that UI.
6. As soon as local controls can take over, hold one display lock, call
   `app_boot_ui_destroy()`, and create/present the product UI. Do not force a
   flush between those actions or wait for a minimum logo duration. A separate
   rendered READY frame is optional and must not delay the handoff.

Destroy is idempotent. If the owning screen/display is deleted externally, the
delete event clears module state; later updates are rejected and recreation is
possible. A late boot callback must not recreate the logo over the product UI;
the application owns that lifecycle guard. Recovery from a fault requires the
coordinator to submit a new observed state, not a UI retry button.

Repeated identical snapshots do not set labels/styles again. The view has no
borrowed strings or caller-owned pointers. The screen contains no dynamic
connection badge, fake completion animation or progress bar.

## Validation boundary

[Native boot tests](../tests/boot_ui/README.md) compile the production module
against the pinned LVGL source and render it at 480 x 480. They cover real object
geometry, maximum label text, progress/ready/fault transitions, invalid snapshots,
idle allocation/flush behavior, ownership, external deletion and repeated
create/destroy cycles. They do not establish target heap, display DMA,
initialization ordering, perceived boot duration or hardware recovery. Those
remain integration/device checks.
