#!/usr/bin/env python3
"""Exercise task ownership/accounting without running a target compiler."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_stack_budget as stack  # noqa: E402

CONFIG = "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192\n#define CONFIG_ESP_TIMER_TASK_STACK_SIZE 3584\n#define CONFIG_BT_BTC_TASK_STACK_SIZE 3072\n#define CONFIG_FREERTOS_ISR_STACKSIZE 1536\n"


class StackBudgets(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory(prefix="esp32-stack-policy-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        for relative in set(stack.BUDGET_FILES.values()) | set(stack.PROVENANCE_FILES):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / relative).read_bytes())
        return root

    @staticmethod
    def frame(size, kind="static"):
        return {"bytes": size, "kind": kind, "location": "fixture.c:1:1:entry"}

    def test_real_ownership_and_nonspendable_sdk_supplement(self):
        budgets = stack.task_budgets(CONFIG)
        self.assertEqual({key: value["budget_bytes"] for key, value in budgets.items()}, {
            "main": 8192, "esp_timer": 3584, "sensor": 4096, "power": 3072,
            "control_runtime": 12288, "media_control": 6144, "board_audio": 6144,
            "ble_control": 6144, "btc": 3072, "usb_audio": 3072, "https": 12288, "lvgl": 8192})
        self.assertEqual(budgets["main"]["allocated_bytes"], 8704)
        self.assertEqual(budgets["esp_timer"]["allocated_bytes"], 4096)
        self.assertEqual(budgets["btc"]["allocated_bytes"], 3584)
        self.assertEqual(stack.PATH_OWNERS["sensor_power_journal"], "sensor")
        self.assertEqual(stack.PATH_OWNERS["brightness_worker"], "power")
        self.assertEqual(stack.PATH_OWNERS["sensor_button_down_publish"], "esp_timer")
        nano = stack.task_budgets(CONFIG + "#define CONFIG_LIBC_NEWLIB_NANO_FORMAT 1\n")
        self.assertEqual(nano["esp_timer"]["allocated_bytes"], 3584)
        self.assertEqual(nano["btc"]["allocated_bytes"], 3072)
        self.assertEqual(nano["main"]["budget_bytes"], 8192)

    def test_small_task_fails_when_same_frames_fit_main(self):
        budgets = stack.task_budgets(CONFIG)
        paths = {"small": [("test", "entry")], "main": [("test", "entry")]}
        chains, errors = stack.evaluate_paths({"test": {"entry": self.frame(1800)}}, budgets,
            paths, {"small": "esp_timer", "main": "main"}, set())
        self.assertEqual(len(errors), 1)
        self.assertIn("small (esp_timer)", errors[0])
        self.assertLess(chains["small"]["remaining_after_reserve_bytes"], 0)
        self.assertEqual(chains["small"]["allocated_bytes"], 4096)
        self.assertEqual(chains["small"]["reserve_bytes"], 2048)
        self.assertGreater(chains["main"]["remaining_after_reserve_bytes"], 0)

    def test_source_budget_drift_changes_gate_and_hash(self):
        root = self.fixture()
        path = root / stack.BUDGET_FILES["board"]
        before = stack.task_budgets(CONFIG, root)
        source = path.read_text()
        self.assertIn("SENSOR_STACK_BYTES = 4096", source)
        path.write_text(source.replace("SENSOR_STACK_BYTES = 4096", "SENSOR_STACK_BYTES = 3072"))
        after = stack.task_budgets(CONFIG, root)
        self.assertEqual(after["sensor"]["budget_bytes"], 3072)
        self.assertEqual(after["main"]["budget_bytes"], 8192)
        self.assertNotEqual(before["sensor"]["evidence"][0]["sha256"], after["sensor"]["evidence"][0]["sha256"])
        self.assertEqual(after["sensor"]["evidence"][0]["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        paths, owners = {"producer": [("test", "entry")]}, {"producer": "sensor"}
        frames = {"test": {"entry": self.frame(1200)}}
        self.assertFalse(stack.evaluate_paths(frames, before, paths, owners, set())[1])
        self.assertTrue(stack.evaluate_paths(frames, after, paths, owners, set())[1])

    def test_missing_or_changed_ownership_fails_closed(self):
        with self.assertRaises(RuntimeError):
            stack.task_budgets("#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192\n")
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG.replace("#define CONFIG_BT_BTC_TASK_STACK_SIZE 3072\n", ""))
        root = self.fixture()
        path = root / stack.BUDGET_FILES["board"]
        path.write_text(path.read_text().replace('"board_sensor", SENSOR_STACK_BYTES', '"board_sensor", unknown_budget'))
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG, root)
        root = self.fixture()
        path = root / stack.BUDGET_FILES["server"]
        path.write_text(path.read_text() + "\nvoid extra(void) { config.httpd.stack_size = 1024; }\n")
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG, root)
        root = self.fixture()
        path = root / stack.BUDGET_FILES["board"]
        path.write_text(path.read_text() + "\nvoid extra(void) { display_config.task_stack_size = 1024; }\n")
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG, root)

    def test_clones_dynamic_frames_and_required_callbacks_still_gate(self):
        budgets = stack.task_budgets(CONFIG)
        frames = {"test": {"entry": self.frame(1000), "entry.part.0": self.frame(64),
            "entry.constprop.0": self.frame(128, "dynamic,bounded")}}
        paths = {"producer": [("test", "entry"), ("test", "callback")]}
        chains, errors = stack.evaluate_paths(frames, budgets, paths, {"producer": "sensor"}, {("test", "callback")})
        self.assertEqual(chains["producer"]["known_bytes"], 1192)
        self.assertTrue(any("Non-static frame" in error for error in errors))
        self.assertTrue(any("Required emitted frame missing" in error for error in errors))
        self.assertEqual(len(stack.variants_for(frames, "test", "entry")), 3)

    def test_paths_without_verified_owners_are_not_charged_to_main(self):
        budgets = stack.task_budgets(CONFIG)
        with self.assertRaises(RuntimeError):
            stack.evaluate_paths({}, budgets, {"unknown": []}, {}, set())
        with self.assertRaises(RuntimeError):
            stack.evaluate_paths({}, budgets, {"unknown": []}, {"unknown": "missing"}, set())

    def test_lvgl_inline_helper_uses_required_caller_but_public_entries_cannot_disappear(self):
        budgets = stack.task_budgets(CONFIG)
        paths = {"touch": [("lvgl_timer", "lv_timer_handler"), ("lvgl_timer", "lv_timer_exec"),
            ("lvgl_indev", "lv_indev_read_timer_cb"), ("lvgl_indev", "lv_indev_read")]}
        frames = {"lvgl_timer": {"lv_timer_handler": self.frame(48)},
            "lvgl_indev": {"lv_indev_read_timer_cb": self.frame(16), "lv_indev_read": self.frame(96)}}
        owners = {"touch": "lvgl"}
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)
        self.assertEqual(chains["touch"]["known_bytes"], 160)
        self.assertEqual(chains["touch"]["frames"][1]["bytes"], 0)
        frames["lvgl_timer"]["lv_timer_exec"] = self.frame(32)
        frames["lvgl_timer"]["lv_timer_exec.constprop.0"] = self.frame(16)
        self.assertEqual(stack.evaluate_paths(frames, budgets, paths, owners)[0]["touch"]["known_bytes"], 208)
        for module, name in (("lvgl_timer", "lv_timer_handler"),
                             ("lvgl_indev", "lv_indev_read_timer_cb"), ("lvgl_indev", "lv_indev_read")):
            with self.subTest(required_entry=name):
                saved = frames[module].pop(name)
                errors = stack.evaluate_paths(frames, budgets, paths, owners)[1]
                self.assertTrue(any(f"Required emitted frame missing: {module}:{name}" in error for error in errors))
                frames[module][name] = saved

    def test_bluetooth_management_counts_coordinator_clones_and_native_leaves(self):
        label = "bluetooth_policy_request_key"
        steps = stack.MANAGEMENT_PATHS[label]
        frames = {module: {} for module, _ in steps}
        for module, name in steps:
            frames[module][name] = self.frame(32)
        frames["bluetooth_management"]["app_bluetooth_management_service.part.0"] = self.frame(256)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: stack.PATH_OWNERS[label]})
        self.assertFalse(errors)
        self.assertEqual(chains[label]["task_owner"], "main")
        self.assertEqual(chains[label]["known_bytes"], 32 * len(steps) + 256)
        self.assertEqual(chains[label]["budget_bytes"], 8192)
        self.assertEqual(steps[-3:], [("ble", "ble_hid_target_preferences_request"),
            ("ble", "submit_preferences_locked"), ("ble", "key_for_bond")])
        frames["bluetooth_management"].clear()
        self.assertTrue(any("bluetooth_management:app_bluetooth_management_service" in error for error in
            stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1]))

    def test_local_management_stops_at_async_admission_and_separates_sequential_calls(self):
        for label, steps in stack.MANAGEMENT_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], "main", label)
            self.assertFalse(any(module in ("ble", "media_runtime") and name == "worker"
                for module, name in steps), label)
        submit = stack.MANAGEMENT_PATHS["audio_select_connection_submit"]
        resolve = stack.MANAGEMENT_PATHS["audio_select_connection_resolve"]
        self.assertEqual(submit[-1], ("media_runtime", "app_media_runtime_submit_local"))
        self.assertNotIn(("media_controls", "connection_route"), submit)
        self.assertEqual(resolve[-1], ("media_controls", "connection_route"))
        self.assertNotIn(("media_controls", "submit_runtime"), resolve)
        self.assertIn(("main", "consume_coordinator_event"), submit)

    def test_audio_observer_copies_and_loggers_are_separate_main_task_paths(self):
        labels = ["audio_diagnostics_media_snapshot", "audio_diagnostics_usb_snapshot", "audio_diagnostics_codec_snapshot"]
        paths = {label: stack.AUDIO_DIAGNOSTIC_PATHS[label] for label in labels}
        frames = {}
        for steps in paths.values():
            for module, name in steps:
                frames.setdefault(module, {})[name] = self.frame(32)
        owners = {label: stack.PATH_OWNERS[label] for label in labels}
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths, owners)
        self.assertFalse(errors)
        expected_leaves = {("media_runtime", "app_media_runtime_snapshot"),
            ("usb_audio", "usb_audio_snapshot"), ("board_audio", "board_audio_mosaico_status")}
        for label, steps in paths.items():
            self.assertEqual(set(steps) & expected_leaves, {steps[-1]})
            self.assertEqual(chains[label]["known_bytes"], 32 * len(steps))
            self.assertEqual(chains[label]["task_owner"], "main")
        for label, steps in stack.AUDIO_DIAGNOSTIC_PATHS.items():
            if "_log_" in label:
                self.assertNotIn(("audio_diagnostics", "observe"), steps)
                self.assertEqual(steps[-1][0], "audio_diagnostics")
        label = "audio_diagnostics_codec_format"
        steps = stack.AUDIO_DIAGNOSTIC_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["audio_diagnostics"]["codec_changed.constprop.0"] = self.frame(64)
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], 32 * len(steps) + 64)
        del frames["audio_helpers"]["ae_format_equal"]
        errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})[1]
        self.assertTrue(any("audio_helpers:ae_format_equal" in error for error in errors))

    def test_worker_allocation_branches_must_agree(self):
        for key, task in (("media_runtime", "media_control"), ("board_audio", "board_audio")):
            with self.subTest(worker=task):
                root = self.fixture()
                path = root / stack.BUDGET_FILES[key]
                source = path.read_text()
                allocation = f'worker, "{task}", 6144'
                self.assertIn(allocation, source)
                path.write_text(source.replace(allocation, f'worker, "{task}", 4096', 1))
                with self.assertRaises(RuntimeError):
                    stack.task_budgets(CONFIG, root)

    def test_board_clock_worker_uses_its_allocation_and_not_the_submitting_task(self):
        label = "board_audio_unmute_start_tx_clock_purge"
        steps = stack.BOARD_AUDIO_PATHS[label]
        frames = {"board_audio": {name: self.frame(32) for _, name in steps}}
        frames["board_audio"]["service"] = self.frame(4096)
        frames["board_audio"]["hardware_unmute.part.0"] = self.frame(64)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: "board_audio"})
        self.assertTrue(errors)
        self.assertEqual(chains[label]["budget_bytes"], 6144)
        self.assertEqual(chains[label]["reserve_bytes"], 2048)
        self.assertFalse(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1])
        del frames["board_audio"]["worker"]
        errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: "board_audio"})[1]
        self.assertTrue(any("Required emitted frame missing: board_audio:worker" in error for error in errors))
        for label, steps in stack.BOARD_AUDIO_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], "board_audio", label)
            self.assertNotIn(("main", "app_main"), steps, label)
            self.assertNotIn(("audio_manager", "worker"), steps, label)
            self.assertFalse({("board_audio", "start_tx_clock"), ("board_audio", "stop_tx_clock")} <= set(steps), label)

    def test_static_inline_codec_operation_name_counts_emission_without_requiring_it(self):
        label = "audio_diagnostics_codec_operation"
        steps = stack.AUDIO_DIAGNOSTIC_PATHS[label]
        helper = ("audio_diagnostics", "board_audio_sdk_operation_name")
        frames = {}
        for module, name in steps:
            if (module, name) != helper:
                frames.setdefault(module, {})[name] = self.frame(32)
        owners = {label: "main"}; budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, owners)
        self.assertFalse(errors)
        known = chains[label]["known_bytes"]
        frames[helper[0]][helper[1]] = self.frame(16)
        frames[helper[0]][helper[1] + ".constprop.0"] = self.frame(32)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, owners)
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], known + 48)
        del frames["audio_diagnostics"]["app_audio_diagnostics_poll"]
        self.assertTrue(any("Required emitted frame missing: audio_diagnostics:app_audio_diagnostics_poll" in error
            for error in stack.evaluate_paths(frames, budgets, {label: steps}, owners)[1]))

    def test_advertising_callback_uses_btc_base_without_spending_sdk_extra(self):
        label = "ble_advertising_gap_callback_target_flags"
        owner, steps = stack.BLE_ADVERTISING_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["ble"]["gap_callback"] = self.frame(900)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})
        self.assertEqual(owner, "btc")
        self.assertTrue(errors)
        self.assertEqual(chains[label]["budget_bytes"], 3072)
        self.assertEqual(chains[label]["allocated_bytes"], 3584)
        self.assertLess(chains[label]["known_bytes"] + 2048, 3584)
        self.assertFalse(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1])

    def test_advertising_inline_dispatch_and_synchronous_observation_boundaries(self):
        label = "ble_advertising_gap_callback_forget_flags"
        owner, steps = stack.BLE_ADVERTISING_PATHS[label]
        inline = ("btc_gap", "btc_gap_ble_cb_to_app")
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})
            if (module, name) != inline:
                frames[module][name] = self.frame(32)
        frames["ble"]["advertising_trace_locked.part.0"] = self.frame(64)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], 32 * (len(steps) - 1) + 64)
        for module, name in (("ble", "gap_callback"), ("btc_thread", "btc_thread_handler")):
            with self.subTest(required_callback=name):
                saved = frames[module].pop(name)
                errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})[1]
                self.assertTrue(any(f"Required emitted frame missing: {module}:{name}" in error for error in errors))
                frames[module][name] = saved
        for label, (owner, steps) in stack.BLE_ADVERTISING_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], owner)
            if owner == "btc":
                self.assertNotIn(("ble", "control_worker"), steps)
                self.assertNotIn(("ble", "advertising_request_observation"), steps)
            if label.endswith("_observing"):
                self.assertNotIn(("ble", "advertising_trace_locked"), steps)
            else:
                self.assertNotIn(("ble", "advertising_observing"), steps)
        self.assertNotIn(("ble", "ble_hid_advertising_event_name"), stack.PATHS["ble_advertising_replay_copy"])
        self.assertNotIn(("ble", "ble_hid_copy_advertising_diagnostics"), stack.PATHS["ble_advertising_replay_name"])

    def test_usb_worker_is_gated_separately_from_main_and_interrupt_inventory(self):
        label = "usb_audio_worker_count"
        steps = stack.USB_AUDIO_WORKER_PATHS[label]
        frames = {"usb_audio": {name: self.frame(32) for _, name in steps}}
        frames["usb_audio"]["service_io"] = self.frame(1056)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: "usb_audio"})
        self.assertTrue(errors)
        self.assertEqual(chains[label]["budget_bytes"], 3072)
        self.assertEqual(chains[label]["reserve_bytes"], 2048)
        self.assertFalse(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1])
        for label, steps in stack.USB_AUDIO_WORKER_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], "usb_audio")
            self.assertFalse(any(name.endswith("_done_isr") for _, name in steps))
        self.assertFalse(set(stack.INTERRUPT_PATHS) & set(stack.PATHS))

    def test_usb_fifo_read_counts_nested_peek_and_missing_emission_fails_closed(self):
        label = "usb_audio_worker_fifo_read"
        paths = {label: stack.USB_AUDIO_WORKER_PATHS[label]}
        owners = {label: "usb_audio"}
        budgets = stack.task_budgets(CONFIG)
        frames = {"usb_audio": {"worker": self.frame(112)},
            "tinyusb_audio": {"tud_audio_n_read": self.frame(0)},
            "tinyusb_fifo": {"tu_fifo_read_n_access_mode": self.frame(32),
                "tu_fifo_peek_n_access_mode": self.frame(64)}}
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], 208)
        self.assertEqual(chains[label]["remaining_after_reserve_bytes"], 816)
        # The read frame remains live during peek; omitting peek hides overflow.
        frames["usb_audio"]["worker"] = self.frame(960)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertEqual(chains[label]["known_bytes"], 1056)
        self.assertLess(chains[label]["remaining_after_reserve_bytes"], 0)
        self.assertTrue(errors)
        del frames["tinyusb_fifo"]["tu_fifo_peek_n_access_mode"]
        errors = stack.evaluate_paths(frames, budgets, paths, owners)[1]
        self.assertTrue(any("Required emitted frame missing: tinyusb_fifo:tu_fifo_peek_n_access_mode" in error
            for error in errors))

    def test_interrupt_inventory_never_qualifies_and_known_oversize_blocks(self):
        label = "usb_audio_rx_count"
        steps = stack.INTERRUPT_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["usb_audio"]["count.part.0"] = self.frame(16)
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertFalse(errors)
        self.assertEqual(inventory["status"], "UNQUALIFIED")
        self.assertFalse(inventory["safety_pass_claimed"])
        self.assertFalse(inventory["task_reserve_applied"])
        self.assertEqual(inventory["configured_stack_bytes"], 1536)
        entry = inventory["paths"][label]
        self.assertEqual(entry["known_application_bytes"], 80)
        self.assertEqual(entry["known_selected_chain_bytes"], 32 * len(steps) + 16)
        self.assertIsNone(entry["pre_change_application_bytes"])
        frames["usb_audio"]["tud_audio_rx_done_isr"] = self.frame(1600)
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertTrue(errors)
        self.assertEqual(inventory["status"], "BLOCKED")
        self.assertEqual(inventory["paths"][label]["qualification"], "BLOCKED_KNOWN_FRAMES")
        del frames["usb_audio"]["tud_audio_rx_done_isr"]
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertTrue(errors)
        self.assertIsNone(inventory["paths"][label]["known_application_bytes"])
        self.assertEqual(stack.SDK_ISR_RESERVE_BYTES, 2048)
        with self.assertRaises(RuntimeError):
            stack.interrupt_stack_budget(CONFIG.replace("#define CONFIG_FREERTOS_ISR_STACKSIZE 1536\n", ""))

    def test_generated_audio_is_bound_to_selected_build_and_wrong_source_fails_closed(self):
        root = self.fixture()
        for relative in stack.UNITS.values():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                path.write_text("/* fixture */\n")
        build = root / "alternate-build"
        (build / "config").mkdir(parents=True)
        (build / "compile_commands.json").write_text("[]\n")
        (build / "config/sdkconfig.h").write_text(CONFIG)
        (root / "firmware/sdkconfig").write_text("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192\n")
        generated = build / "generated/tinyusb_audio/audio_device.c"
        generated.parent.mkdir(parents=True)
        generated.write_text("/* reviewed generated fixture */\n")
        manifest_path = root / "patches/tinyusb-audio-streams.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["output_sha256"] = hashlib.sha256(generated.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        database = [{"file": str(generated)}]
        upstream = root / "firmware/managed_components/espressif__tinyusb/src/class/audio/audio_device.c"
        with patch.object(stack, "ROOT", root):
            units = stack.measurement_units(build)
            self.assertEqual(root / units["tinyusb_audio"], generated)
            proof = stack.verify_generated_audio_source(build, database)
            self.assertEqual(proof["sha256"], manifest["output_sha256"])
            with self.assertRaises(RuntimeError):
                stack.verify_generated_audio_source(build, database + [{"file": str(upstream)}])
            with self.assertRaises(RuntimeError):
                stack.compile_unit(("tinyusb_audio", units["tinyusb_audio"]), [{"file": str(upstream)}], root / "unused")
            before = stack.input_snapshot(build, [])
            stack.verify_generated_audio_snapshot(proof, before)
            conflicting = dict(before)
            conflicting["patches/tinyusb-audio-streams.json"] = "changed during proof read"
            with self.assertRaises(RuntimeError):
                stack.verify_generated_audio_snapshot(proof, conflicting)
            generated.write_text("/* changed generated fixture */\n")
            after = stack.input_snapshot(build, [])
            self.assertEqual({key for key in before if before[key] != after[key]},
                {"alternate-build/generated/tinyusb_audio/audio_device.c"})
            with self.assertRaises(RuntimeError):
                stack.verify_generated_audio_source(build, database)

    def test_async_audio_execution_uses_media_budget_and_admission_uses_main(self):
        label = "audio_worker_start_submit"
        owner, steps = stack.AUDIO_RUNTIME_PATHS[label]
        self.assertEqual(owner, "media_control")
        self.assertEqual(stack.PATH_OWNERS[label], "media_control")
        self.assertEqual(steps[:3], [("media_runtime", "worker"),
            ("media_runtime", "media_step"), ("media_runtime", "process_command")])
        self.assertNotIn(("audio_manager", "worker"), steps)
        frames = {module: {} for module, _ in steps}
        for module, name in steps:
            frames[module][name] = self.frame(32)
        frames["media_runtime"]["media_step"] = self.frame(4096)
        budgets = stack.task_budgets(CONFIG)
        self.assertTrue(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "media_control"})[1])
        self.assertFalse(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1])
        self.assertEqual(stack.PATH_OWNERS["audio_live_admission_usb_capabilities"], "main")
        self.assertEqual(stack.PATH_OWNERS["audio_worker_identity_usb_capabilities"], "media_control")
        self.assertNotIn(("media_runtime", "worker"), stack.PATHS["audio_live_admission_usb_capabilities"])

    def test_audio_route_factory_returns_before_capability_callback(self):
        factory = stack.PATHS["audio_worker_start_usb_factory"]
        callback = stack.PATHS["audio_worker_start_usb_capabilities"]
        self.assertIn(("usb_audio", "usb_audio_route_port"), factory)
        self.assertNotIn(("usb_audio", "capabilities"), factory)
        self.assertIn(("usb_audio", "capabilities"), callback)
        self.assertNotIn(("media_runtime", "route_port"), callback)

    def test_touch_residual_remote_frame_clones_and_public_admission_still_gate(self):
        label = "touch_remote_enqueue_guard"
        steps = stack.TOUCH_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["remote_input"]["virtual_touch.constprop.0"] = self.frame(320)
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], 32 * len(steps) + 320)
        self.assertEqual(chains[label]["reserve_bytes"], 2048)
        self.assertIn(("main", "remote_control_service"), steps)
        for entry in (("hid_engine", "hid_engine_enqueue_mouse_displacement"), ("input_core", "input_can_emit")):
            with self.subTest(required_entry=entry):
                module, name = entry
                saved = frames[module].pop(name)
                errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})[1]
                self.assertTrue(any(f"Required emitted frame missing: {module}:{name}" in error for error in errors))
                frames[module][name] = saved

    def test_touch_conversion_admission_and_later_send_have_separate_lifetimes(self):
        labels = ("touch_local_conversion", "touch_local_enqueue_age")
        paths = {label: stack.TOUCH_PATHS[label] for label in labels}
        frames = {}
        for steps in paths.values():
            for module, name in steps:
                frames.setdefault(module, {})[name] = self.frame(32)
        frames["interaction"]["interaction_touch_displacement_owned"] = self.frame(3000)
        frames["hid_engine"]["hid_engine_enqueue_mouse_displacement"] = self.frame(3500)
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths,
            {label: stack.PATH_OWNERS[label] for label in labels})
        self.assertFalse(errors)
        self.assertNotIn(("interaction", "interaction_touch_displacement_owned"), paths[labels[1]])
        self.assertNotIn(("hid_engine", "hid_engine_enqueue_mouse_displacement"), paths[labels[0]])
        for label, steps in stack.TOUCH_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], "main", label)
            self.assertNotIn(("usb_hid", "worker"), steps, label)
            if "_enqueue_" in label:
                self.assertNotIn(("hid_engine", "hid_engine_step"), steps, label)
        for label in ("touch_send_ble_refresh", "touch_send_usb_ready"):
            self.assertIn(("main", "send_report"), stack.TOUCH_PATHS[label])
            self.assertIn(("hid_engine", "hid_engine_step"), stack.TOUCH_PATHS[label])
        self.assertTrue(all(chain["remaining_after_reserve_bytes"] > 0 for chain in chains.values()))
        label = "touch_send_usb_ready"
        steps = stack.TOUCH_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        for module, name in (("main", "send_report"), ("usb_hid", "usb_hid_send_generation")):
            with self.subTest(required_callback_or_backend=name):
                saved = frames[module].pop(name)
                errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})[1]
                self.assertTrue(any(f"Required emitted frame missing: {module}:{name}" in error for error in errors))
                frames[module][name] = saved

    def test_remote_touch_cancel_charges_revocation_clones_and_defers_release_sends(self):
        label = "touch_remote_cancel_owner_release"
        steps = stack.TOUCH_REMOTE_CANCEL_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["remote_input"]["virtual_touch.constprop.0"] = self.frame(128)
        frames["remote_input"]["revoke.part.0"] = self.frame(256)
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})
        self.assertFalse(errors)
        self.assertEqual(chains[label]["known_bytes"], 32 * len(steps) + 384)
        self.assertEqual(steps[-3:], [("hid_engine", "hid_engine_set_owner"),
            ("input_core", "input_set_owner"), ("input_core", "input_invalidate")])
        del frames["hid_engine"]["hid_engine_set_owner"]
        errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), {label: steps}, {label: "main"})[1]
        self.assertTrue(any("Required emitted frame missing: hid_engine:hid_engine_set_owner" in error for error in errors))
        for label, steps in stack.TOUCH_REMOTE_CANCEL_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], "main", label)
            self.assertNotIn(("hid_engine", "hid_engine_step"), steps, label)
            self.assertNotIn(("main", "send_report"), steps, label)
            self.assertNotIn(("hid_engine", "hid_engine_enqueue_mouse_displacement"), steps, label)
        self.assertNotIn(("main", "remote_changed"), stack.TOUCH_REMOTE_CANCEL_PATHS["touch_remote_cancel_observe_ble"])
        self.assertNotIn(("remote_input", "observe"), stack.TOUCH_REMOTE_CANCEL_PATHS["touch_remote_cancel_callback_ready"])

    def test_stack_expression_cannot_execute_or_resolve_unknown_names(self):
        self.assertEqual(stack.integer_expression("(8 * 1024U)"), 8192)
        for expression in ("__import__('os').system('false')", "unknown", "-1", "True", "2 ** 100"):
            with self.subTest(expression=expression), self.assertRaises(RuntimeError):
                stack.integer_expression(expression)

    def test_budget_vendor_header_changes_are_in_drift_snapshot(self):
        root = self.fixture()
        for relative in stack.UNITS.values():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                path.write_text("/* fixture */\n")
        build = root / "firmware/build"
        (build / "config").mkdir(parents=True)
        (build / "compile_commands.json").write_text("[]\n")
        (build / "config/sdkconfig.h").write_text(CONFIG)
        (root / "firmware/sdkconfig").write_text("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192\n")
        target = root / stack.BUDGET_FILES["lvgl_header"]
        with patch.object(stack, "ROOT", root):
            before = stack.input_snapshot(build, [])
            target.write_text(target.read_text().replace("(8 * 1024)", "(4 * 1024)"))
            after = stack.input_snapshot(build, [])
        changed = {key for key in before if before[key] != after[key]}
        self.assertEqual(changed, {stack.BUDGET_FILES["lvgl_header"]})


if __name__ == "__main__":
    unittest.main()
