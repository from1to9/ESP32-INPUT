#!/usr/bin/env python3
"""Exercise task ownership/accounting without running a target compiler."""
import hashlib
import json
from pathlib import Path
import shlex
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

    def generated_fixture(self, *, external=False):
        root = self.fixture()
        for relative in stack.UNITS.values():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                path.write_text("/* fixture */\n")
        if external:
            temporary = tempfile.TemporaryDirectory(prefix="esp32-stack-external-")
            self.addCleanup(temporary.cleanup)
            build = Path(temporary.name).resolve() / "build"
        else:
            build = root / "alternate-build"
        (build / "config").mkdir(parents=True)
        (build / "compile_commands.json").write_text("[]\n")
        (build / "config/sdkconfig.h").write_text(CONFIG)
        (root / "firmware/sdkconfig").write_text("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192\n")
        manifest_path = root / "patches/tinyusb-audio-streams.json"
        manifest = json.loads(manifest_path.read_text())
        database = []
        for unit, contract in stack.GENERATED_TINYUSB.items():
            generated = build / "generated/tinyusb_audio" / contract["filename"]
            generated.parent.mkdir(parents=True, exist_ok=True)
            generated.write_text(f"/* reviewed {unit} fixture */\n")
            spec = manifest if contract["manifest_key"] is None else manifest[contract["manifest_key"]]
            upstream = root / "firmware/managed_components/espressif__tinyusb" / contract["source_path"]
            spec["source_sha256"] = hashlib.sha256(upstream.read_bytes()).hexdigest()
            spec["output_sha256"] = hashlib.sha256(generated.read_bytes()).hexdigest()
            spec["patch_sha256"] = hashlib.sha256((root / contract["patch_file"]).read_bytes()).hexdigest()
            database.append({"file": str(generated), "directory": str(root),
                "command": shlex.join(["fixture-compiler", "-c", str(generated), "-o", "unused.o"])})
        manifest_path.write_text(json.dumps(manifest))
        return root, build, database

    @staticmethod
    def frame(size, kind="static"):
        return {"bytes": size, "kind": kind, "location": "fixture.c:1:1:entry"}

    def test_real_ownership_and_nonspendable_sdk_supplement(self):
        budgets = stack.task_budgets(CONFIG)
        self.assertEqual({key: value["budget_bytes"] for key, value in budgets.items()}, {
            "main": 8192, "esp_timer": 3584, "sensor": 4096, "power": 3072,
            "control_runtime": 12288, "media_control": 6144, "board_audio": 6144,
            "ble_control": 6144, "btc": 3072, "usb_audio": 3072, "tinyusb": 4096, "https": 12288, "lvgl": 8192})
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

    def test_admission_replay_keeps_copy_name_and_logging_lifetimes_separate(self):
        paths = {label: steps for label, (_, steps) in stack.BLE_ADMISSION_PATHS.items()
            if label.startswith("ble_admission_replay_")}
        owners = {label: stack.PATH_OWNERS[label] for label in paths}
        frames = {"main": {"app_main": self.frame(64), "coordinator_run": self.frame(128),
            "report_bluetooth_admission_diagnostics": self.frame(96),
            "report_bluetooth_admission_event": self.frame(192)},
            "ble": {"ble_hid_copy_admission_diagnostics": self.frame(640),
                "ble_hid_admission_event_name": self.frame(16)}}
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths, owners)
        self.assertFalse(errors)
        self.assertEqual(chains["ble_admission_replay_copy"]["known_bytes"], 928)
        self.assertEqual(chains["ble_admission_replay_name"]["known_bytes"], 496)
        self.assertEqual(chains["ble_admission_replay_event_log"]["known_bytes"], 480)
        self.assertEqual(chains["ble_admission_replay_log"]["known_bytes"], 288)
        self.assertTrue(all(owner == "main" for owner in owners.values()))
        for module, name in (("ble", "ble_hid_copy_admission_diagnostics"),
                             ("ble", "ble_hid_admission_event_name"),
                             ("main", "report_bluetooth_admission_diagnostics"),
                             ("main", "report_bluetooth_admission_event")):
            with self.subTest(required_entry=name):
                saved = frames[module].pop(name)
                errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths, owners)[1]
                self.assertTrue(any(f"Required emitted frame missing: {module}:{name}" in error for error in errors))
                frames[module][name] = saved

    def test_admission_producers_use_actual_worker_btc_and_main_owners(self):
        label = "ble_admission_worker_admission_window"
        owner, steps = stack.BLE_ADMISSION_PATHS[label]
        frames = {"ble": {"control_worker": self.frame(160), "service_pairing_admission": self.frame(96),
            "admission_trace_locked": self.frame(48), "admission_trace_locked.constprop.0": self.frame(32),
            "pairing_allowed_locked": self.frame(16), "single_forget_busy_locked": self.frame(16),
            "admission_flags_locked": self.frame(640), "admission_trace_finish_locked": self.frame(512)}}
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})
        self.assertFalse(errors)
        self.assertEqual(owner, "ble_control")
        self.assertEqual(chains[label]["known_bytes"], 368)
        self.assertNotIn(("ble", "admission_flags_locked"), steps)
        self.assertNotIn(("ble", "admission_trace_finish_locked"), steps)
        for label, (owner, steps) in stack.BLE_ADMISSION_PATHS.items():
            self.assertEqual(stack.PATH_OWNERS[label], owner)
            self.assertNotIn(("ble", "notification_watchdog"), steps)
            self.assertFalse(any(name.endswith("_done_isr") for _, name in steps))
            if owner == "btc":
                self.assertEqual(steps[:3], stack.BTC_DISPATCH)
                self.assertNotIn(("ble", "control_worker"), steps)
        remote = stack.BLE_ADMISSION_PATHS["ble_admission_remote_submit_open_flags"]
        self.assertEqual(remote[0], "main")
        self.assertIn(("main", "remote_control_service"), remote[1])
        self.assertIn(("main", "remote_device_submit_action"), remote[1])
        self.assertIn(("ble", "ble_hid_pairing_open"), remote[1])

    def test_admission_remote_adapters_and_emitted_clones_can_exhaust_main_budget(self):
        budgets = stack.task_budgets(CONFIG)
        for label, helper in (("ble_admission_remote_submit_open_flags", "remote_device_execute"),
                              ("ble_admission_remote_approval_open_flags", "remote_device_service"),
                              ("ble_admission_remote_poll_open_flags", "remote_device_service")):
            with self.subTest(adapter=helper, path=label):
                owner, steps = stack.BLE_ADMISSION_PATHS[label]
                paths, owners = {label: steps}, {label: owner}
                frames = {}
                for module, name in steps:
                    frames.setdefault(module, {})[name] = self.frame(16)
                frames["main"].pop(helper, None)
                chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
                self.assertFalse(errors)  # An inlined private adapter has no separate frame.
                spare = chains[label]["remaining_after_reserve_bytes"]
                frames["main"]["coordinator_run"] = self.frame(16 + spare - 128)
                chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
                self.assertFalse(errors)
                baseline = chains[label]["known_bytes"]
                self.assertEqual(chains[label]["remaining_after_reserve_bytes"], 128)
                frames["main"][helper] = self.frame(128)
                frames["main"][helper + ".constprop.0"] = self.frame(64)
                chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
                self.assertEqual(chains[label]["known_bytes"], baseline + 192)
                self.assertEqual(chains[label]["remaining_after_reserve_bytes"], -64)
                self.assertTrue(errors)

    def test_admission_btc_callback_cannot_spend_main_or_sdk_extra(self):
        label = "ble_admission_gap_callback_flags"
        owner, steps = stack.BLE_ADMISSION_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(16)
        frames["ble"]["gap_callback"] = self.frame(1000)
        budgets = stack.task_budgets(CONFIG)
        chains, errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})
        self.assertEqual(owner, "btc")
        self.assertTrue(errors)
        self.assertEqual(chains[label]["budget_bytes"], 3072)
        self.assertEqual(chains[label]["allocated_bytes"], 3584)
        self.assertLess(chains[label]["known_bytes"] + 2048, 3584)
        self.assertFalse(stack.evaluate_paths(frames, budgets, {label: steps}, {label: "main"})[1])
        del frames["ble"]["gap_callback"]
        errors = stack.evaluate_paths(frames, budgets, {label: steps}, {label: owner})[1]
        self.assertTrue(any("Required emitted frame missing: ble:gap_callback" in error for error in errors))

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
        self.assertEqual(entry["known_application_bytes"], 112)
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

    def test_tinyusb_allocation_chain_drift_changes_budget_and_override_is_rejected(self):
        root = self.fixture()
        before = stack.task_budgets(CONFIG, root)
        self.assertEqual(before["tinyusb"]["budget_bytes"], 4096)
        self.assertEqual(before["tinyusb"]["allocated_bytes"], 4096)
        self.assertEqual(before["tinyusb"]["sdk_supplement_excluded_bytes"], 0)
        paths, owners = {"entry": [("fixture", "entry")]}, {"entry": "tinyusb"}
        frames = {"fixture": {"entry": self.frame(1800)}}
        self.assertFalse(stack.evaluate_paths(frames, before, paths, owners, set())[1])
        defaults = root / stack.BUDGET_FILES["tinyusb_defaults"]
        original = defaults.read_text()
        defaults.write_text(original.replace("TINYUSB_DEFAULT_TASK_SIZE      4096",
            "TINYUSB_DEFAULT_TASK_SIZE      2048"))
        after = stack.task_budgets(CONFIG, root)
        self.assertEqual(after["tinyusb"]["budget_bytes"], 2048)
        self.assertTrue(stack.evaluate_paths(frames, after, paths, owners, set())[1])
        token = "TINYUSB_TASK_DEFAULT()"
        position = original.index(token, original.index("#define TINYUSB_CONFIG_FULL_SPEED"))
        defaults.write_text(original[:position] + "TINYUSB_TASK_CUSTOM(8192, 5, 0)" + original[position + len(token):])
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG, root)
        defaults.write_text(original)
        console = root / stack.BUDGET_FILES["usb_console"]
        console.write_text(console.read_text() + "\nvoid override(void) { s_driver . task . size = 8192; }\n")
        with self.assertRaises(RuntimeError):
            stack.task_budgets(CONFIG, root)

    def test_tinyusb_task_register_sample_and_clones_cannot_spend_main_budget(self):
        label = "tinyusb_out_submit_registers"
        owner, steps = stack.USB_TRANSPORT_TASK_PATHS[label]
        budgets = stack.task_budgets(CONFIG)
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(16)
        frames["tinyusb_dcd"].pop("input_usb_register_sample", None)
        paths, owners = {label: steps}, {label: owner}
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)
        self.assertEqual(owner, "tinyusb")
        spare = chains[label]["remaining_after_reserve_bytes"]
        frames["tinyusb_task"]["tinyusb_device_task"] = self.frame(16 + spare - 64)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)
        baseline = chains[label]["known_bytes"]
        frames["tinyusb_dcd"]["input_usb_register_sample"] = self.frame(72)
        frames["tinyusb_dcd"]["input_usb_register_sample.constprop.0"] = self.frame(16)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertEqual(chains[label]["known_bytes"], baseline + 88)
        self.assertEqual(chains[label]["remaining_after_reserve_bytes"], -24)
        self.assertTrue(errors)
        self.assertFalse(stack.evaluate_paths(frames, budgets, paths, {label: "main"})[1])
        del frames["usb_audio"]["input_usb_audio_dcd_registers"]
        errors = stack.evaluate_paths(frames, budgets, paths, owners)[1]
        self.assertTrue(any("Required emitted frame missing: usb_audio:input_usb_audio_dcd_registers" in error for error in errors))
        cleanup_owner, cleanup = stack.USB_TRANSPORT_TASK_PATHS["tinyusb_startup_failure_deinit"]
        self.assertEqual(cleanup_owner, "main")
        self.assertEqual(cleanup[0], ("main", "app_main"))
        self.assertIn(("tinyusb_task", "tinyusb_task_stop"), cleanup)
        self.assertIn(("tinyusb_dcd", "dcd_deinit"), cleanup)
        self.assertNotIn(("tinyusb_task", "tinyusb_device_task"), cleanup)

    def test_dcd_interrupt_wrapper_and_hooks_remain_unqualified_and_fail_closed(self):
        label = "usb_dcd_rx_rearm_count"
        steps = stack.INTERRUPT_PATHS[label]
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(32)
        frames["tinyusb_dcd"].pop("input_usb_dcd_complete", None)
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertFalse(errors)
        baseline = inventory["paths"][label]
        frames["tinyusb_dcd"]["input_usb_dcd_complete"] = self.frame(64)
        frames["tinyusb_dcd"]["input_usb_dcd_complete.part.0"] = self.frame(32)
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertFalse(errors)
        entry = inventory["paths"][label]
        self.assertEqual(entry["known_selected_chain_bytes"], baseline["known_selected_chain_bytes"] + 96)
        self.assertEqual(entry["known_application_bytes"], baseline["known_application_bytes"] + 96)
        self.assertEqual(inventory["status"], "UNQUALIFIED")
        self.assertFalse(inventory["safety_pass_claimed"])
        self.assertIn("cannot be attributed separately", inventory["application_scope"])
        frames["tinyusb_dcd"]["dcd_int_handler"] = self.frame(1600)
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertTrue(errors)
        self.assertEqual(inventory["status"], "BLOCKED")
        self.assertLess(inventory["paths"][label]["known_application_bytes"], 1536)
        del frames["usb_audio"]["input_usb_audio_dcd_transfer"]
        inventory, errors = stack.interrupt_frame_inventory(frames, CONFIG, {label: steps})
        self.assertTrue(errors)
        self.assertIsNone(inventory["paths"][label]["known_application_bytes"])

    def test_tinyusb_control_request_helper_and_clones_can_exhaust_its_own_budget(self):
        label = "tinyusb_out_submit_registers"
        owner, steps = stack.USB_TRANSPORT_TASK_PATHS[label]
        paths, owners = {label: steps}, {label: owner}
        budgets = stack.task_budgets(CONFIG)
        frames = {}
        for module, name in steps:
            frames.setdefault(module, {})[name] = self.frame(16)
        frames["tinyusb_audio"].pop("audiod_control_request", None)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)
        spare = chains[label]["remaining_after_reserve_bytes"]
        frames["tinyusb_task"]["tinyusb_device_task"] = self.frame(16 + spare - 64)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertFalse(errors)  # The private helper may be inlined into its caller.
        baseline = chains[label]["known_bytes"]
        self.assertEqual(chains[label]["remaining_after_reserve_bytes"], 64)
        frames["tinyusb_audio"]["audiod_control_request"] = self.frame(48)
        frames["tinyusb_audio"]["audiod_control_request.constprop.0"] = self.frame(32)
        chains, errors = stack.evaluate_paths(frames, budgets, paths, owners)
        self.assertEqual(chains[label]["known_bytes"], baseline + 80)
        self.assertEqual(chains[label]["remaining_after_reserve_bytes"], -16)
        self.assertEqual(chains[label]["task_owner"], "tinyusb")
        self.assertTrue(errors)

    def test_transport_snapshot_and_large_logger_have_separate_main_lifetimes(self):
        labels = ("audio_diagnostics_transport_snapshot", "audio_diagnostics_log_transport")
        paths = {label: stack.AUDIO_DIAGNOSTIC_PATHS[label] for label in labels}
        owners = {label: stack.PATH_OWNERS[label] for label in labels}
        frames = {}
        for steps in paths.values():
            for module, name in steps:
                frames.setdefault(module, {})[name] = self.frame(32)
        frames["usb_audio"]["usb_audio_transport_snapshot"] = self.frame(640)
        frames["audio_diagnostics"]["log_transport"] = self.frame(512)
        chains, errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths, owners)
        self.assertFalse(errors)
        self.assertEqual(chains[labels[0]]["known_bytes"], 800)
        self.assertEqual(chains[labels[1]]["known_bytes"], 640)
        self.assertEqual(set(owners.values()), {"main"})
        del frames["usb_audio"]["usb_audio_transport_snapshot"]
        errors = stack.evaluate_paths(frames, stack.task_budgets(CONFIG), paths, owners)[1]
        self.assertTrue(any("Required emitted frame missing: usb_audio:usb_audio_transport_snapshot" in error for error in errors))

    def test_generated_audio_and_dcd_are_bound_to_the_selected_build(self):
        for external in (False, True):
            root, build, database = self.generated_fixture(external=external)
            with self.subTest(external_build=external), patch.object(stack, "ROOT", root):
                units = stack.measurement_units(build)
                before = stack.input_snapshot(build, database)
                for unit, contract in stack.GENERATED_TINYUSB.items():
                    generated = build / "generated/tinyusb_audio" / contract["filename"]
                    self.assertEqual(root / units[unit], generated)
                    proof = stack.verify_generated_source(build, database, unit)
                    self.assertEqual(proof["sha256"], hashlib.sha256(generated.read_bytes()).hexdigest())
                    self.assertEqual(proof["generated_compile_entries"], 1)
                    stack.verify_generated_source_snapshot(proof, before)
                self.assertEqual(stack.verify_generated_audio_source(build, database),
                    stack.verify_generated_source(build, database, "tinyusb_audio"))
                self.assertEqual(stack.verify_generated_dcd_source(build, database),
                    stack.verify_generated_source(build, database, "tinyusb_dcd"))

    def test_generated_compile_inputs_reject_missing_duplicate_and_managed_fallback(self):
        root, build, database = self.generated_fixture()
        with patch.object(stack, "ROOT", root):
            units = stack.measurement_units(build)
            for unit in stack.GENERATED_TINYUSB:
                proof = stack.verify_generated_source(build, database, unit)
                generated = proof["source"]
                missing = [entry for entry in database if entry["file"] != generated]
                cases = {"missing": missing, "duplicate": database + [{"file": generated}],
                    "managed_fallback": database + [{"file": proof["managed_source"]}]}
                for case, commands in cases.items():
                    with self.subTest(unit=unit, rejected_input=case), self.assertRaises(RuntimeError):
                        stack.verify_generated_source(build, commands, unit)
                with self.assertRaises(RuntimeError), patch.object(stack.subprocess, "run",
                        side_effect=AssertionError("The unit fixture must not invoke a compiler")):
                    stack.compile_unit((unit, units[unit]), [{"file": proof["managed_source"]}], root / "unused")

    def test_generated_preimage_postimage_and_patch_pins_cannot_drift(self):
        root, build, database = self.generated_fixture()
        manifest_path = root / "patches/tinyusb-audio-streams.json"
        original_manifest = manifest_path.read_bytes()
        with patch.object(stack, "ROOT", root):
            for unit, contract in stack.GENERATED_TINYUSB.items():
                proof = stack.verify_generated_source(build, database, unit)
                for key in ("source", "managed_source", "patch"):
                    path = Path(proof[key])
                    original = path.read_bytes()
                    try:
                        path.write_bytes(original + b"/* drift */\n")
                        with self.subTest(unit=unit, changed_file=key), self.assertRaises(RuntimeError):
                            stack.verify_generated_source(build, database, unit)
                    finally:
                        path.write_bytes(original)
                for field, changed in (("source_path", "src/unreviewed.c"),
                                       ("patch_file", "patches/unreviewed.patch"),
                                       ("source_sha256", "0" * 64), ("output_sha256", "0" * 64),
                                       ("patch_sha256", "0" * 64)):
                    manifest = json.loads(original_manifest)
                    spec = manifest if contract["manifest_key"] is None else manifest[contract["manifest_key"]]
                    spec[field] = changed
                    try:
                        manifest_path.write_text(json.dumps(manifest))
                        with self.subTest(unit=unit, changed_pin=field), self.assertRaises(RuntimeError):
                            stack.verify_generated_source(build, database, unit)
                    finally:
                        manifest_path.write_bytes(original_manifest)
            manifest = json.loads(original_manifest)
            del manifest["dcd"]
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                stack.verify_generated_dcd_source(build, database)

    def test_generated_proof_snapshot_covers_all_validated_bytes(self):
        root, build, database = self.generated_fixture()
        with patch.object(stack, "ROOT", root):
            for unit in stack.GENERATED_TINYUSB:
                proof = stack.verify_generated_source(build, database, unit)
                before = stack.input_snapshot(build, database)
                for key in ("source", "manifest", "managed_source", "patch"):
                    path = Path(proof[key])
                    snapshot_key = str(path.relative_to(root)) if path.is_relative_to(root) else str(path)
                    conflicting = dict(before)
                    conflicting[snapshot_key] = "changed after proof validation"
                    with self.subTest(unit=unit, changed_input=key), self.assertRaises(RuntimeError):
                        stack.verify_generated_source_snapshot(proof, conflicting)
                generated = Path(proof["source"])
                original = generated.read_bytes()
                try:
                    generated.write_bytes(original + b"/* changed generated fixture */\n")
                    after = stack.input_snapshot(build, database)
                    self.assertEqual({key for key in before if before[key] != after[key]},
                        {str(generated.relative_to(root))})
                    with self.assertRaises(RuntimeError):
                        stack.verify_generated_source(build, database, unit)
                finally:
                    generated.write_bytes(original)

    def test_generated_proof_retains_the_manifest_bytes_used_for_validation(self):
        for unit, contract in stack.GENERATED_TINYUSB.items():
            root, build, database = self.generated_fixture()
            manifest_path = root / "patches/tinyusb-audio-streams.json"
            original_digest = stack.digest
            changed = False

            def change_manifest_after_read(path):
                nonlocal changed
                value = original_digest(path)
                if not changed and path.name == contract["filename"]:
                    manifest = json.loads(manifest_path.read_text())
                    spec = manifest if contract["manifest_key"] is None else manifest[contract["manifest_key"]]
                    spec["output_sha256"] = "0" * 64
                    manifest_path.write_text(json.dumps(manifest))
                    changed = True
                return value

            with self.subTest(unit=unit), patch.object(stack, "ROOT", root):
                with patch.object(stack, "digest", side_effect=change_manifest_after_read):
                    proof = stack.verify_generated_source(build, database, unit)
                self.assertTrue(changed)
                with self.assertRaises(RuntimeError):
                    stack.verify_generated_source_snapshot(proof, stack.input_snapshot(build, database))

    def test_generated_sources_reject_missing_files_and_symlinked_inputs(self):
        root, build, database = self.generated_fixture()
        with patch.object(stack, "ROOT", root):
            for unit in stack.GENERATED_TINYUSB:
                proof = stack.verify_generated_source(build, database, unit)
                for key in ("source", "managed_source", "patch"):
                    path = Path(proof[key])
                    original = path.read_bytes()
                    target = path.with_name(path.name + ".fixture-target")
                    try:
                        path.unlink()
                        with self.subTest(unit=unit, missing=key), self.assertRaises(RuntimeError):
                            stack.verify_generated_source(build, database, unit)
                        target.write_bytes(original)
                        path.symlink_to(target)
                        with self.subTest(unit=unit, symlinked=key), self.assertRaises(RuntimeError):
                            stack.verify_generated_source(build, database, unit)
                    finally:
                        if path.is_symlink():
                            path.unlink()
                        path.write_bytes(original)
                        if target.exists():
                            target.unlink()

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
