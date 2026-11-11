# Validation procedures

Run checks from the repository root after following the
[build instructions](../firmware/README.md). Use the
[companion environment](../bridge/README.md) for Python collector and MCP
checks that require its dependencies.

| Check | Boundary |
| --- | --- |
| `python3 tools/check_repository.py` | First-party text and Markdown paths. Review prose and intent separately. |
| `python3 tools/generate_control_api.py --check` | Generated firmware registry, JSON schema, OpenAPI and MCP metadata agree. |
| `python3 tools/check.py` | Portable engines, adapters, production coordinator, protocol and tool regressions with hardware calls replaced at defined boundaries. |
| `python3 tests/ui/run.py` | Native LVGL behavior, presentation and 480 × 480 renders. Inspect PNGs for layout, clipping and legibility. |
| `python3 tools/idf.py build` | Build against the pinned SDK, BSP and managed components. |
| `python3 tools/check_stack_budget.py` | Static known call paths with the configured target compile database. |
| [Browser harness](../tests/web_controller/README.md) | Browser and protocol behavior with controlled device responses. |

## Device qualification

Confirm the running build identity and test one named board, host OS and
transport at a time. Record the actual outcome, including steps that failed
or could not be observed.

1. **Input.** Pair or attach USB, confirm HID readiness, and use a disposable
   editor to verify mouse, wheel, drag, keyboard modifiers, preview and Send.
   Exercise host sleep, cancellation between press and release, and target
   changes. Check that stale work never appears on the next host.
2. **Motion and touch.** Calibrate in a known direction, turn along each axis
   and test touch pause, resume, grip changes and intentional Recenter. Keep
   a single contact held across the pad boundary. Observe direction, response
   and continuity on the target, including while audio is active.
3. **Audio.** Exercise Mic and Speaker independently on each supported route.
   Capture and listen at the actual endpoints. Verify explicit start, mute,
   stop and restart, then test concurrent workloads separately. Verify saved
   Classic reconnect without opening Add device.
4. **LAN and updates.** Enroll the expected identity, check expiry and local
   takeover, and observe the actual host response to authorized input. Test
   signed staging, verification, cancellation and fallback on the matching
   partition layout before qualifying an update workflow.
5. **Endurance.** Measure timing and resource use over a stated duration and
   workload. Separate steady-state behavior from reconnect, failure recovery
   and update transitions.

The [validation summary](validation.md) lists observed results and current
limitations. Keep serial logs, network captures and recordings local.
