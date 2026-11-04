# UI resource budget and evidence boundaries

Observed from isolated ESP32-S31 target objects using pinned ESP-IDF commit
`383dda6c182e3dabf6355160166fe007df775629`, GCC 16.1.0 esp-20260609 and
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, 2026-10-22. Object sections are
build evidence, not a linked firmware free-heap or runtime latency measurement.

| Owner storage | Actual object bytes | Target section | Access contract |
| --- | ---: | --- | --- |
| UI copied view | 29,488 | `.ext_ram.bss` | LVGL owner under the existing display lock |
| UI pressed remote review | 552 | `.ext_ram.bss` | LVGL owner; immutable identity and consequences retained through release |
| UI color bindings | 8,192 | `.ext_ram.bss` | LVGL creation, render and theme owner only |
| UI ordinary bindings | 1,792 | `.ext_ram.bss` | LVGL render/event owner; copied actions cross the coordinator boundary |
| UI media bindings | 3,840 | `.ext_ram.bss` | LVGL render/event owner; separate pressed snapshot retains release identity |
| UI connection bindings | 2,080 | `.ext_ram.bss` | LVGL render/event owner; separate pressed snapshot retains release identity |
| Presenter pending slot | 29,744 | `.ext_ram.bss` | Coordinator publisher and LVGL owner, protected by the existing short handoff lock |
| Presenter apply scratch | 29,744 | `.ext_ram.bss` | LVGL owner only |
| Connectivity controls | 41,784 | `.ext_ram.bss` | Coordinator task only; complete public trust Review plus current/previous formatted pages |
| Media product controls | 9,952 | `.ext_ram.bss` | Coordinator task only; includes bounded HFP connection sequence |
| Media poll scratch | 4,488 | `.ext_ram.bss` | Coordinator only; distinct incoming snapshots/query records, wiped after polling |
| BLE preference request scratch | 1,344 | `.ext_ram.bss` | Coordinator only; transport copies before return, then wiped |
| Presenter lock | 8 | Internal `.sdata` | Task critical-section handoff |
| Boot copied view | 156 | Internal `.bss` | Startup LVGL owner |

The twelve state blocks total 163,000 bytes (159.18 KiB). They use static
`EXT_RAM_BSS_ATTR` storage after board/PSRAM startup, with an empty native-build
fallback. No ISR or cache-disabled routine may access them. Display transfer
ISR receipts, locks, lifecycle state and other small control data remain in
their owning internal-memory sections. There is no hot-path heap fallback.
The parent's separate main snapshot and final firmware linker placement must
be included in the complete-system memory review.

Raising the complete trust Review ceiling from 8 to 64 pages adds 14,392 bytes
per page-array copy: UI view, two presenter views and two connectivity views.
The visitation bitmap/alignment adds another 8 bytes in the connectivity owner,
for a measured 71,968-byte increase across these owners. The same-ABI baseline
connectivity object was 12,992 bytes. A separate main view adds a further
14,392 bytes and is accounted by the coordinator's final link. Only actual
configured pages are shown (3 minimal, 10 ordinary, 46 maximal-count package,
53 public-array boundary). Target compiler stack-usage reports 240 bytes for
connectivity rebuild and 848 for its bounded line writer; these are individual
function frames, not a measured task stack high-water mark.

The native LVGL resource fixture measures 106 fixed objects, at most 148
observed objects under the 160-object dynamic-panel ceiling, and two timers.
It performs 1,000 open/close cycles per theme across 39 variants. Identical
settled snapshots and hidden updates require zero allocations, reallocations,
frees and flushes. Teardown returns tracked live bytes/blocks to zero. The last
observed warmed core uses 45,360 requested LVGL bytes in 1,064 blocks, with
62,595 peak requested bytes. These counts exclude allocator overhead, static
snapshots, test framebuffers, SDK/radio/codec allocations and target DMA memory.

The persistent Keyboard Keys button and caption account for the two added fixed
objects. Its fourteen-key panel remains below the unchanged 160-object ceiling.
Header width changes occur only when entering or leaving Keyboard; the mode
selector, Keys and Menu use the same 16 px font. The two-line critical status
control retains its 14 px warning text. Measured captions occupy 57/50/62/67 pixels
for Motion/Touch/Spoken/Hotkeys within the 68-pixel caption width. The updated
768-scene native suite retains all 174 historical scenarios and checks both themes,
long host names, capture/lock cues, exact key identities and unchanged drafts.

Native composition tests verify display receipt revocation, heartbeat and
generation replacement. They do not measure PSRAM bandwidth, critical-section
latency, physical refresh timing, minimum free heap, flash cache interaction or
long-running device behavior. Final linked memory budgets and device validation
remain separate gates.

The stack repair adds 5,832 fixed PSRAM bytes for distinct incoming media
observations/query records and the BLE preference request. It reduces the target
media poll frame from 5,072 to 656 bytes and the connectivity refresh frame from
1,616 to 176 bytes. The old 1,424-byte BLE preference handler is inlined into the
80-byte outer handler. Direct copy-out bond getters remove additional wrapper
frames. These individual frames do not include the coordinator, transport/SDK
calls, interrupts or task startup; the aggregate task budget remains a separate
integration check. The existing retained state size and rendered UI are unchanged.

The four binding arrays move 15,904 bytes from internal static storage without
changing their capacities or lifetimes. Isolated target object comparison found
all 97 executable sections byte-identical; all 748 native rendered images also
matched the preceding approved UI. These placement observations do not prove a
physical redraw succeeds. The [runtime memory plan](runtime-memory.md) separately
accounts for SPI bounce buffers, audio DMA, radio pools and task stacks.

## Shared Bluetooth manager addition (2026-11-04)

The current fixed core is 120 objects: the Navigation baseline has 118, and the direct Audio Manage devices button adds exactly two. The dynamic-panel limit increases by the same two objects from 160 to 162. Four-row shared lists reuse the existing bounded binding tables; no per-poll allocation is introduced. The resource harness includes both manager purposes and audio details, with unchanged idle, hidden-state and repeated close/teardown checks.
