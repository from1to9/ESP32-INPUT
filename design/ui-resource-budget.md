# UI resource budget and evidence boundaries

Observed from isolated ESP32-S31 target objects using pinned ESP-IDF commit
`383dda6c182e3dabf6355160166fe007df775629`, GCC 16.1.0 esp-20260609 and
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, 2026-10-21. Object sections are
build evidence, not a linked firmware free-heap or runtime latency measurement.

| Owner storage | Actual object bytes | Target section | Access contract |
| --- | ---: | --- | --- |
| UI copied view | 29,488 | `.ext_ram.bss` | LVGL owner under the existing display lock |
| UI pressed remote review | 552 | `.ext_ram.bss` | LVGL owner; immutable identity and consequences retained through release |
| Presenter pending slot | 29,744 | `.ext_ram.bss` | Coordinator publisher and LVGL owner, protected by the existing short handoff lock |
| Presenter apply scratch | 29,744 | `.ext_ram.bss` | LVGL owner only |
| Connectivity controls | 41,784 | `.ext_ram.bss` | Coordinator task only; complete public trust Review plus current/previous formatted pages |
| Media product controls | 9,952 | `.ext_ram.bss` | Coordinator task only; includes bounded HFP connection sequence |
| Presenter lock | 8 | Internal `.sdata` | Task critical-section handoff |
| Boot copied view | 156 | Internal `.bss` | Startup LVGL owner |

The six state blocks total 141,264 bytes (137.95 KiB). They use static
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

The native LVGL resource fixture measures 104 fixed objects, at most 146
observed objects under the 160-object dynamic-panel ceiling, and two timers.
It performs 1,000 open/close cycles per theme across 39 variants. Identical
settled snapshots and hidden updates require zero allocations, reallocations,
frees and flushes. Teardown returns tracked live bytes/blocks to zero. The last
observed warmed core uses 44,579 requested LVGL bytes in 1,043 blocks, with
61,814 peak requested bytes. These counts exclude allocator overhead, static
snapshots, test framebuffers, SDK/radio/codec allocations and target DMA memory.

Native composition tests verify display receipt revocation, heartbeat and
generation replacement. They do not measure PSRAM bandwidth, critical-section
latency, physical refresh timing, minimum free heap, flash cache interaction or
long-running device behavior. Final linked memory budgets and device validation
remain separate gates.
