# Device UI refinement and settings contract

Status: implemented in the native UI, 2026-10-21. This refinement follows the user's request to improve all screens and device settings. It preserves the original Mouse / Keyboard / Audio navigation and input workflows from the [prototype](index.html) and [interaction contract](../docs/interaction.md). Hardware, network services, persistent state and asynchronous operations remain coordinator-owned.

## Grid, typography and colors

Use a 480 x 480 canvas, 16 px side margins, 8 px minimum gaps, 12 px control radii, Montserrat 16 body text, 14 metadata and 22 titles. Button targets are at least 44 x 44 px; primary actions and navigation are 48 px high. Controls in a row share dimensions and centered text baselines. Single-line fields have explicit font-line height and ellipsis; wrapping content has a measured fixed region.

| Region | Geometry | Content |
| --- | --- | --- |
| Header | x16, y8, w448, h48 | Actual target name and input readiness; battery; Wi-Fi signal bars and Bluetooth connection icon; Settings |
| Main page | x16, y72, w448, h320 | Mode-specific controls |
| Status | x16, y400, w448 | One metadata line, truncated before navigation |
| Primary navigation | y424, h48 | Three equal 144 px buttons at x16, x168, x320 |

The target touch area is 260 x 48 px. A separate 128 x 48 px telemetry target opens Connectivity. Settings occupies the rightmost 44 x 44 px target. Wi-Fi RSSI bars represent actual measurements: four bars at -55 dBm or better, three at -67 or better, two at -78 or better, one below. Disconnected radios show a cross; Wi-Fi with unknown connected signal shows a question mark. Bluetooth shows a check when connected and never displays RSSI or signal bars. Exact Wi-Fi dBm is available in Connectivity. Battery percentage and charging indication come only from the actual gauge snapshot. USB attachment is explicitly unavailable rather than inferred from current or charge state.

Light retains the approved green/neutral palette. Dark uses background `#080B10`, surface `#111A24`, text `#ECF4FB`, secondary text `#B6C4D2`, accent `#8DD8FF` and selected text `#082B40`. Focus also has a visible border and fill. Theme changes restyle existing objects without rebuilding the screen or changing gesture ownership.

## Primary pages

Mouse keeps the motion toggle, relative touchpad, independent scroll, three touch gains and explicit click/drag/release. Gains have equal 72 x 44 px targets. The touchpad is 384 x 156 px; the scroll column is 56 px wide. The three 144 x 48 px actions align with navigation. Brief transport congestion does not disable controls; active Release remains available.

Keyboard uses the following coordinates relative to the main page. The character grid is genuinely three columns by three rows, with 112 x 44 px targets at x0, x120 and x240. The 88 px utility rail at x360 is separate, leaving every character and utility control at least 44 px high.

| Row or region | Geometry |
| --- | --- |
| Motion / Touch / Spoken / Shortcuts | y0, h44; four 106 px columns at 114 px intervals |
| Preview | y52, h52; separate preview/count lines and Clear 72 x44 |
| Nine-cell character grid | y112, y164 and y216; three equal 112 x44 cells per row |
| Groups / case / symbols rail | x360, matching each grid row; 88 x44 targets |
| Delete / Space / Send or Stop | y272, h48; equal 144 px columns at x0, x152, x304 |

The initial nine groups are ABCD / EFGH / IJKL / MNOP / QRST / UVWX / YZ01 / 2345 / 6789, supplied by the input engine. Selecting a group retains the second-stage preview-first workflow with four valid characters. Empty cells are hidden and non-actionable. The UI does not duplicate the engine's character model. Motion focus may address any of the nine valid first-stage cells. Utilities retain Groups, case and symbol access without reducing touch targets.

The Spoken tab is one line for consistent alignment; its page title remains "Spoken letters". Shortcuts retain an explicit Enter action; Send never appends Enter. Draft capacity, accepted-prefix progress and cancellation are coordinator state. Voice and audio capability remain truthfully unavailable until supported.

Audio uses two equal tabs, a centered state card, equal power/mute targets and a volume row. It never invents an active microphone or stream.

## Settings and local confirmation

Settings is an overlay on the selected mode. Its page header has a 44 px Back target and aligned title. Content begins at y128; save status is at y400; Done is full-width at y424. Home has four equal cards:

- Connectivity: one Bluetooth/USB input transport selector shared across Mouse, Keyboard and Audio mode, followed by Bluetooth hosts and Wi-Fi settings cards. Bluetooth is the default; only the coordinator can acknowledge a switch. Available, disconnected, connected, ready, switching and error states are explicit. During switching the current path and requested path remain distinguishable, and selector/input controls cannot claim an early successful transition. Choosing a transport neither changes the mode nor duplicates input; audio support remains an independent capability.
- Bluetooth hosts: up to 32 actual saved identities, four per page, with optional host-reported names and explicit Connected/Saved status. Missing names use Host plus a short real identity; the full identity stays visible beneath it. Add host (or Close pairing while open), Disconnect and Forget all request local confirmation. The saved count is shown against the actual provider capacity, such as 3/15. Full or over-capacity storage disables Add with a full message; unknown count or capacity also disables Add without claiming a free slot. No pairings are automatically evicted. The UI cache limit of 32 is not the pairing-storage capacity. This page always reports Bluetooth state even when USB is the selected input path. Rows do not claim to actively connect a BLE peripheral to a host.
- Wi-Fi settings: actual SSID/state/signal/IP, setup and forget controls. Saved network reconnection remains service-owned. Back returns to Connectivity; Wi-Fi setup remains a separate protected overlay.
- Display: Light/Dark, brightness 10 to 100 percent in steps of 10 (default 70), and Screen timeout Off / 1 / 5 / 10 / 30 minutes (default 5). Timeout applies in all power states. Only the backlight turns off; Bluetooth and Wi-Fi continue.
- Input: Ctrl/Command, haptic availability/switch, motion calibration and physical-button guidance.
- About: actual firmware/build, board/device identity, battery state and uptime. Unavailable values remain explicit. Long identity values are bounded independently.

Open/Close/Forget Bluetooth, Disconnect and Forget Wi-Fi each require a coordinator-owned local confirmation. Cancel and Confirm have equal large targets. Request buttons only enqueue a request to open confirmation; they do not perform or acknowledge the operation themselves.

Wi-Fi setup shows the protected temporary AP's actual SSID, passcode, local browser address and countdown. Back returns to Connectivity and stops setup through the coordinator. Stop setup is explicit. Closing, expiry, lock or screen-off clears the UI's credential copy and credential labels. Synthetic setup fixtures in native tests contain no real secrets.

## State and event boundary

[app_ui.h](../firmware/main/app_ui.h) is the authoritative interface for view fields, enum values, units and intent payloads. The UI uses independent plain Wi-Fi values rather than importing the service's driver enum. All callbacks only enqueue bounded intents; they never perform hardware/network writes or persist settings.

Theme, haptics, host modifier, brightness and timeout persist in the coordinator. Defaults active, saving, saved, error and unavailable storage are separate states. Saved is displayed only after actual persistence acknowledgement. Pairing's pending transition says "Updating pairing" because it may include opening, closing or clearing.

Existing touch BEGIN/MOVE/END/CANCEL, epoch, revision and gesture IDs remain the sole activity contract. The first whole touch or AI gesture wakes the screen and is consumed by the coordinator. The UI shield emits touch boundaries while screen-off and suppresses button actions for a touch that started asleep, including if a wake snapshot arrives before release. A new gesture can act. Lock remains separate and physical-only.

The source uses bounded action/color registries and fixed-size snapshot strings. Repeated renders keep existing object identity, including while a slider or touchpad gesture is active.

## Validation boundary

The [native test runner](../tests/ui/README.md) compiles production UI against pinned LVGL, drives real hit testing and checks actual geometry. It renders 164 images across both themes and covers core input regressions, settings, 32-entry Bluetooth pagination and connection state, nine-cell/four-character hit maps, Bluetooth/USB selection and unavailable/pending transport states, confirmations, persistence failures, unavailable/unknown telemetry, Wi-Fi setup lifecycle, credential clearing and screen-off wake consumption. All native images are inspected before integration handoff.

Host checks establish source rendering and intent behavior. Integrated target compilation, physical display readability, gauge/radio measurements, persistent device state and wake timing require separate evidence from their owners. Earlier screenshots from `.agent/local/ui-visual/` and `.agent/local/ui-refresh/` do not validate this layout; current output is `.agent/local/ui-nine-transport/`.
