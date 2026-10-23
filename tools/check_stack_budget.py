#!/usr/bin/env python3
"""Measure selected coordinator paths with the configured target compiler.

This is a focused regression gate, not a whole-program stack proof. Compiler
frames include inlined locals. Named out-of-line clone bodies are conservatively
summed, and the declared callback paths retain a separate SDK/ISR reserve.
No shared build output, device, or running process is changed.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SDK_ISR_RESERVE_BYTES = 2048
STARTUP_FRAME_LIMIT_BYTES = 512
UNITS = {
    "main": "firmware/main/main.c",
    "bridge": "firmware/main/app_command_bridge.c",
    "device": "firmware/main/app_remote_device.c",
    "remote_input": "firmware/main/app_remote_input.c",
    "remote_media": "firmware/main/app_remote_media.c",
    "media_controls": "firmware/main/app_media_controls.c",
    "connectivity": "firmware/main/app_connectivity_controls.c",
    "media_runtime": "firmware/main/app_media_runtime.c",
    "ble": "firmware/main/ble_hid.c",
    "classic": "firmware/main/bt_audio.c",
    "settings": "firmware/main/settings_store.c",
    "preferences": "components/device_settings/device_settings.c",
    "wifi": "firmware/main/app_wifi.c",
    "control_runtime": "firmware/main/app_control_runtime.c",
    "board": "firmware/main/board_mosaico.c",
    "interaction": "components/interaction/interaction.c",
    "imu_queue": "firmware/main/board_imu_queue.c",
    "motion_capture": "firmware/main/motion_capture.c",
}
CORE = [("main", "app_main"), ("main", "coordinator_run")]
DEVICE_SUBMIT = CORE + [("bridge", "app_command_bridge_service"),
                        ("bridge", "execute_flight"), ("main", "command_execute"),
                        ("device", "app_remote_device_execute"),
                        ("device", "execute_owned"), ("device", "execute"),
                        ("device", "submit_job"), ("main", "remote_device_submit"),
                        ("main", "remote_device_submit_action")]
MEDIA_INTENT = CORE + [("main", "handle_intent"),
                       ("media_controls", "app_media_controls_handle")]
# These are explicit owning callback paths, not inferred function-pointer calls.
# Private helpers may be inlined; an omitted private frame is already charged to
# its emitted caller. Public entries and the lifetime boundaries are required.
PATHS = {
    "raw_motion_capture": CORE + [("main", "service_motion_capture"),
        ("main", "capture_observation"), ("motion_capture", "motion_capture_append")],
    "raw_motion_drain": CORE + [("main", "service_motion_capture"),
        ("board", "board_imu_stream_drain"), ("imu_queue", "board_imu_queue_drain"),
        ("imu_queue", "board_imu_queue_break"), ("imu_queue", "advance_epoch"),
        ("imu_queue", "fail_ids"), ("imu_queue", "clear_frames"), ("imu_queue", "count_dropped")],
    "raw_motion_dump": CORE + [("main", "service_motion_capture"),
        ("main", "capture_dump_step"), ("motion_capture", "motion_capture_row")],
    "runtime_diagnostics_wifi": CORE + [("main", "report_runtime_diagnostics"),
                                        ("wifi", "app_wifi_get_snapshot")],
    "scan_diagnostics_copy": CORE + [("main", "report_runtime_diagnostics"),
                                     ("wifi", "app_wifi_get_scan_diagnostics")],
    "scan_diagnostics_log": CORE + [("main", "report_runtime_diagnostics"),
                                    ("main", "report_scan_observation")],
    "motion_diagnostics": CORE + [("main", "report_motion_diagnostics"),
                                   ("board", "board_imu_status")],
    "motion_sample_observation": CORE + [("main", "process_motion"),
                                         ("main", "observe_motion_sample")],
    "reference_diagnostics_log": CORE + [("main", "report_motion_diagnostics"),
                                          ("main", "report_reference_diagnostics"),
                                          ("main", "report_reference_frame")],
    "reference_sample_observation": CORE + [("main", "process_motion"),
        ("remote_input", "app_remote_input_motion_sample"),
        ("interaction", "interaction_motion_sample_owned"), ("interaction", "reference_begin")],
    "reference_loss_observation": CORE + [("main", "process_motion"),
        ("remote_input", "app_remote_input_motion_sample"),
        ("interaction", "interaction_motion_sample_owned"), ("interaction", "update_reference"),
        ("interaction", "invalidate_reference"), ("interaction", "reference_accepted")],
    "reference_rejection_observation": CORE + [("main", "process_motion"),
        ("remote_input", "app_remote_input_motion_sample"),
        ("interaction", "interaction_motion_sample_owned"), ("interaction", "update_reference"),
        ("interaction", "reference_rejected")],
    "reference_startup_observation": [("main", "app_main"), ("interaction", "interaction_init"),
        ("interaction", "interaction_calibrate"), ("interaction", "reference_begin")],
    "reference_storage_binding": [("main", "app_main"),
        ("interaction", "interaction_reference_diagnostics_bind"),
        ("interaction", "interaction_reference_diagnostics_reset")],
    "reference_clock_observation": CORE + [("main", "process_buttons"),
        ("interaction", "interaction_tick"), ("interaction", "observe_clock"),
        ("interaction", "reference_begin")],
    "reference_sync_observation": CORE + [("main", "sync_interaction"),
        ("interaction", "interaction_sync"), ("interaction", "observe_clock"),
        ("interaction", "reference_begin")],
    "reference_native_recenter": CORE + [("main", "handle_intent"), ("main", "recenter_motion"),
        ("interaction", "interaction_recenter_owned"), ("interaction", "recenter_reference")],
    "reference_remote_recenter": CORE + [("bridge", "app_command_bridge_service"),
        ("bridge", "execute_flight"), ("main", "command_execute"),
        ("remote_input", "app_remote_input_execute"), ("main", "remote_delegate"),
        ("main", "recenter_motion"), ("interaction", "interaction_recenter_owned"),
        ("interaction", "recenter_reference")],
    "reference_enable_reset": CORE + [("main", "handle_intent"), ("main", "set_motion"),
        ("interaction", "interaction_set_motion_owned"), ("interaction", "recenter_reference")],
    "reference_read_observation": CORE + [("bridge", "app_command_bridge_service"),
        ("main", "command_read"), ("device", "app_remote_device_read"),
        ("main", "remote_device_board_read"), ("main", "motion_reference_state"),
        ("board", "board_capabilities")],
    "reference_render_observation": CORE + [("main", "render_view"),
        ("main", "motion_reference_state"), ("board", "board_capabilities")],
    "runtime_diagnostics_media": CORE + [("main", "report_runtime_diagnostics"),
                                         ("media_runtime", "app_media_runtime_snapshot")],
    "runtime_diagnostics_control": CORE + [("main", "report_runtime_diagnostics"),
                                           ("control_runtime", "app_control_runtime_snapshot")],
    "preferences_load": [("main", "app_main"), ("settings", "settings_store_init"),
                         ("preferences", "device_settings_decode")],
    "startup_display": [("main", "app_main"), ("main", "service_display"),
                        ("main", "capture_requires_display"),
                        ("media_runtime", "app_media_runtime_snapshot")],
    "render": CORE + [("main", "render_view"), ("ble", "ble_hid_copy_bond_list")],
    "priority": CORE + [("main", "process_priority")],
    "update_maintenance": CORE + [("main", "remote_update_service"),
                                  ("main", "remote_update_health_service"),
                                  ("ble", "ble_hid_copy_bond_list")],
    "availability": CORE + [("bridge", "app_command_bridge_refresh"),
                             ("main", "command_available"),
                             ("device", "app_remote_device_available"),
                             ("main", "remote_device_owner_available"),
                             ("ble", "ble_hid_copy_bond_list")],
    "peer_read": CORE + [("bridge", "app_command_bridge_service"),
                          ("main", "command_read"),
                          ("device", "app_remote_device_read"),
                          ("main", "remote_device_peers_read"),
                          ("classic", "bt_audio_copy_bond_list")],
    "factory_submission": DEVICE_SUBMIT + [("main", "remote_device_factory_progress"),
                                            ("main", "remote_device_settings_progress")],
    "remote_ble_target": DEVICE_SUBMIT + [("main", "remote_device_bluetooth_progress"),
                                          ("ble", "ble_hid_target_select"),
                                          ("ble", "submit_preferences_locked"),
                                          ("ble", "key_for_bond")],
    "ble_order_api_leaf": [("ble", "ble_hid_target_order"),
                            ("ble", "submit_preferences_locked"), ("ble", "key_for_bond")],
    "ble_preferences_matching_leaf": [("ble", "submit_preferences_locked"),
                                       ("ble", "target_key_matches")],
    "ble_preferences_editable_leaf": [("ble", "submit_preferences_locked"),
                                       ("ble", "target_editable_locked")],
    "factory_preferences_poll": CORE + [("device", "app_remote_device_service"),
                                         ("main", "remote_device_poll"),
                                         ("main", "remote_device_factory_progress"),
                                         ("ble", "ble_hid_target_preferences_request"),
                                         ("ble", "submit_preferences_locked"),
                                         ("ble", "key_for_bond")],
    "media_poll": CORE + [("media_controls", "app_media_controls_poll"),
                           ("media_controls", "poll_owners"),
                           ("classic", "bt_audio_copy_bond_list")],
    "media_poll_rebuild_job": CORE + [("media_controls", "app_media_controls_poll"),
                                      ("media_controls", "rebuild"),
                                      ("media_controls", "map_job"),
                                      ("media_controls", "idle_controls")],
    "media_poll_rebuild_record": CORE + [("media_controls", "app_media_controls_poll"),
                                         ("media_controls", "rebuild"),
                                         ("media_controls", "map_record")],
    "media_poll_request_list": CORE + [("media_controls", "app_media_controls_poll"),
                                       ("media_controls", "request_list"),
                                       ("media_controls", "submit_runtime"),
                                       ("media_runtime", "app_media_runtime_submit_local")],
    "media_intent_rebuild_job": MEDIA_INTENT + [("media_controls", "rebuild"),
                                                ("media_controls", "map_job"),
                                                ("media_controls", "idle_controls")],
    "media_intent_rebuild_record": MEDIA_INTENT + [("media_controls", "rebuild"),
                                                   ("media_controls", "map_record")],
    "media_intent_request_list": MEDIA_INTENT + [("media_controls", "request_list"),
                                                 ("media_controls", "submit_runtime"),
                                                 ("media_runtime", "app_media_runtime_submit_local")],
    "media_intent_start": MEDIA_INTENT + [("media_controls", "submit_start"),
                                          ("media_controls", "submit_runtime"),
                                          ("media_runtime", "app_media_runtime_submit_local")],
    "media_intent_stop": MEDIA_INTENT + [("media_controls", "stop_job"),
                                         ("media_controls", "submit_runtime"),
                                         ("media_runtime", "app_media_runtime_submit_local")],
    "connectivity_poll": CORE + [("connectivity", "app_connectivity_controls_poll"),
                                  ("connectivity", "refresh_owners"),
                                  ("ble", "ble_hid_copy_bond_list")],
    "connectivity_rebuild": CORE + [("connectivity", "app_connectivity_controls_poll"),
                                     ("connectivity", "rebuild"),
                                     ("connectivity", "write_line"),
                                     ("connectivity", "write_char"),
                                     ("connectivity", "write_char")],
    "connectivity_intent": CORE + [("main", "handle_intent"),
                                    ("connectivity", "app_connectivity_controls_handle"),
                                    ("connectivity", "handle_bluetooth"),
                                    ("ble", "ble_hid_copy_bond_list")],
    "connectivity_preferences_submit": CORE + [("main", "handle_intent"),
                                                ("connectivity", "app_connectivity_controls_handle"),
                                                ("connectivity", "handle_bluetooth"),
                                                ("ble", "ble_hid_target_preferences_request"),
                                                ("ble", "submit_preferences_locked"),
                                                ("ble", "key_for_bond")],
    "remote_media_link": CORE + [("bridge", "app_command_bridge_service"),
                                  ("bridge", "execute_flight"),
                                  ("main", "command_execute"),
                                  ("remote_media", "app_remote_media_execute"),
                                  ("main", "remote_media_submit"),
                                  ("media_runtime", "app_media_runtime_submit_remote"),
                                  ("media_runtime", "remote_link_bond"),
                                  ("classic", "bt_audio_copy_bond_list")],
    "remote_ui": CORE + [("bridge", "app_command_bridge_service"),
                          ("bridge", "execute_flight"), ("main", "command_execute"),
                          ("remote_input", "app_remote_input_execute"),
                          ("main", "remote_delegate"), ("main", "remote_device_execute_ui"),
                          ("device", "app_remote_device_execute_ui_authorized"),
                          ("device", "execute_owned"), ("device", "execute"),
                          ("device", "submit_job"), ("main", "remote_device_submit")],
}
REQUIRED = set(CORE + [("main", "render_view"), ("main", "report_runtime_diagnostics"),
                       ("main", "report_motion_diagnostics"), ("main", "observe_motion_sample"),
                       ("main", "report_scan_observation"), ("main", "report_reference_diagnostics"),
                       ("main", "report_reference_frame"), ("interaction", "reference_begin"),
                       ("interaction", "reference_rejected"), ("interaction", "invalidate_reference"),
                       ("interaction", "recenter_reference")]) | {
    step for path in PATHS.values() for step in path
    if step[1].startswith(("app_", "ble_", "bt_", "settings_", "device_settings_", "interaction_"))
}
CONSUMERS = [
    "firmware/main/app_motion_capture.inc",
    "firmware/main/main.c", "firmware/main/app_remote_device_bindings.inc",
    "firmware/main/app_remote_device_maintenance.inc", "firmware/main/app_remote_device_read.inc",
    "firmware/main/app_remote_media_bindings.inc", "firmware/main/app_update_bindings.inc",
    "firmware/main/app_media_controls.c",
    "firmware/main/app_connectivity_controls.c", "firmware/main/app_media_runtime.c",
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_snapshot(build, database):
    """Hash the bounded first-party/configuration inputs, including file sets."""
    paths = {Path(__file__).resolve(), build / "compile_commands.json",
             build / "config/sdkconfig.h", ROOT / "firmware/sdkconfig"}
    for directory in (ROOT / "firmware/main", ROOT / "components"):
        paths.update(path for path in directory.rglob("*")
                     if path.is_file() and path.suffix in (".c", ".h", ".inc", ".S"))
    paths.update(path for path in (build / "toolchain").glob("*") if path.is_file())
    # Forced headers and response files can live outside the first-party tree.
    measured_sources = {ROOT / path for path in UNITS.values()}
    for entry in database:
        if Path(entry["file"]) not in measured_sources:
            continue
        pending = shlex.split(entry["command"])
        visited = set()
        index = 0
        while index < len(pending):
            word = pending[index]
            filename = None
            if word.startswith("@"):
                filename = word[1:]
            elif word in ("-include", "-imacros"):
                index += 1
                filename = pending[index]
            elif word.startswith("-include"):
                filename = word[len("-include"):]
            elif word.startswith("-imacros"):
                filename = word[len("-imacros"):]
            if filename:
                path = Path(filename)
                if not path.is_absolute():
                    path = Path(entry["directory"]) / path
                path = path.resolve()
                paths.add(path)
                if word.startswith("@") and path not in visited:
                    visited.add(path)
                    pending.extend(shlex.split(path.read_text()))
            index += 1
    return {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path):
            digest(path) for path in sorted(paths)}


def compile_unit(unit, database, output):
    name, relative = unit
    source = ROOT / relative
    entry = next((item for item in database if Path(item["file"]) == source), None)
    if entry is None:
        raise RuntimeError(f"No target compile command for {relative}")
    command = shlex.split(entry["command"])
    command[command.index("-o") + 1] = str(output / f"{name}.o")
    for flag in ("-MF", "-MT", "-MQ"):
        if flag in command:
            index = command.index(flag)
            del command[index:index + 2]
    command = [word for word in command if word not in ("-MD", "-MMD")]
    command += ["-fstack-usage", "-Werror"]
    (output / f"{name}-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / f"{name}.log").open("w") as log:
        result = subprocess.run(command, cwd=entry["directory"], stdout=log,
                                stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"Target compile failed: {relative}; see {name}.log")
    frames = {}
    for row in (output / f"{name}.su").read_text().splitlines():
        location, size, kind = row.split("\t")
        frames[location.rsplit(":", 1)[1]] = {
            "bytes": int(size), "kind": kind, "location": location,
        }
    return name, frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "firmware/build")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = (args.output_dir or ROOT / ".agent/local/stack-budget" / stamp).resolve()
    if output == build or build in output.parents:
        raise SystemExit("Evidence output must be outside the shared build directory")
    output.mkdir(parents=True, exist_ok=False)
    database_bytes = (build / "compile_commands.json").read_bytes()
    database = json.loads(database_bytes)
    before = input_snapshot(build, database)
    config_bytes = (build / "config/sdkconfig.h").read_bytes()
    for path, content in ((build / "compile_commands.json", database_bytes),
                          (build / "config/sdkconfig.h", config_bytes)):
        key = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
        if hashlib.sha256(content).hexdigest() != before[key]:
            raise SystemExit(f"Compilation input changed while being read: {path}")
    config = config_bytes.decode()
    match = re.search(r"^#define CONFIG_ESP_MAIN_TASK_STACK_SIZE (\d+)$", config, re.M)
    if not match:
        raise SystemExit("Configured main task stack size unavailable")
    stack_bytes = int(match[1])
    paths = set(UNITS.values()) | set(CONSUMERS)
    legacy = [path for path in CONSUMERS if re.search(
        r"\b(?:ble_hid_bond_list|bt_audio_bond_list)\s*\(", (ROOT / path).read_text())]
    with ThreadPoolExecutor(max_workers=3) as pool:
        frames = dict(pool.map(lambda unit: compile_unit(unit, database, output), UNITS.items()))
    errors, chains = [], {}
    if legacy:
        errors.append("Large by-value bond getter remains in: " + ", ".join(sorted(legacy)))
    for label, steps in PATHS.items():
        entries = []
        for module, function in steps:
            variants = {name: data for name, data in frames[module].items()
                        if name == function or name.startswith(function + ".")}
            if not variants and (module, function) in REQUIRED:
                errors.append(f"Required emitted frame missing: {module}:{function}")
            for data in variants.values():
                if data["kind"] != "static":
                    errors.append(f"Non-static frame requires review: {data['location']}")
            entries.append({"module": module, "function": function,
                            "bytes": sum(data["bytes"] for data in variants.values()),
                            "variants": variants,
                            "note": "" if variants else "private helper inlined/not emitted"})
        total = sum(entry["bytes"] for entry in entries)
        chains[label] = {"known_bytes": total, "reserve_bytes": SDK_ISR_RESERVE_BYTES,
                         "remaining_bytes": stack_bytes - total, "frames": entries}
        if total + SDK_ISR_RESERVE_BYTES > stack_bytes:
            errors.append(f"{label}: {total} + {SDK_ISR_RESERVE_BYTES} reserve > {stack_bytes}")
    startup = frames["main"].get("app_main", {}).get("bytes")
    if startup is None or startup > STARTUP_FRAME_LIMIT_BYTES:
        errors.append(f"Startup frame {startup} exceeds {STARTUP_FRAME_LIMIT_BYTES} bytes")
    after = input_snapshot(build, database)
    changed = [path for path in before.keys() | after.keys()
               if before.get(path) != after.get(path)]
    if changed:
        errors.append("Compilation inputs changed during measurement: " + ", ".join(sorted(changed)))
    report = {"status": "FAIL" if errors else "PASS", "configured_stack_bytes": stack_bytes,
              "sdk_isr_reserve_bytes": SDK_ISR_RESERVE_BYTES,
              "startup_frame_limit_bytes": STARTUP_FRAME_LIMIT_BYTES,
              "source_hashes": {path: after[path] for path in sorted(paths)},
              "input_hashes": after, "changed_during_measurement": sorted(changed),
              "input_scope": "First-party firmware/main and components C/headers/includes, "
                             "this checker, compile database, sdkconfig, generated config header, "
                             "toolchain files, command response files and forced headers. "
                             "Other SDK/vendor dependencies are not covered by this drift check.",
              "paths": chains, "all_frames": frames, "errors": errors,
              "limits": "Declared first-party paths only. Clone sums are conservative; no "
                        "SDK/binary/ISR high-water proof or device qualification is claimed."}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    for label, chain in chains.items():
        print(f"{label}: {chain['known_bytes']} known + {SDK_ISR_RESERVE_BYTES} reserved "
              f"/ {stack_bytes} bytes")
    print(f"{report['status']}: stack budget; evidence {output}")
    if errors:
        raise SystemExit("\n".join(errors))


if __name__ == "__main__":
    main()
