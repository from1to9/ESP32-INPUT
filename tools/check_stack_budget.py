#!/usr/bin/env python3
"""Measure declared task/callback paths with the configured target compiler.

This is a focused regression gate, not a whole-program stack proof. Compiler
frames include inlined locals. Named out-of-line clone bodies are conservatively
summed, and the declared callback paths retain a separate SDK/ISR reserve.
No shared build output, device, or running process is changed.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import argparse
import ast
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
    "bluetooth_management": "firmware/main/app_bluetooth_management.c",
    "media_runtime": "firmware/main/app_media_runtime.c",
    "audio_manager": "firmware/main/app_audio.c",
    "audio_engine": "components/audio_engine/audio_engine.c",
    "audio_diagnostics": "firmware/main/app_audio_diagnostics.c",
    "usb_audio": "firmware/main/usb_audio.c",
    "board_audio": "firmware/main/board_audio_mosaico.c",
    "audio_helpers": "components/audio_engine/audio_media.c",
    "ble": "firmware/main/ble_hid.c",
    "classic": "firmware/main/bt_audio.c",
    "settings": "firmware/main/settings_store.c",
    "preferences": "components/device_settings/device_settings.c",
    "wifi": "firmware/main/app_wifi.c",
    "control_runtime": "firmware/main/app_control_runtime.c",
    "board": "firmware/main/board_mosaico.c",
    "interaction": "components/interaction/interaction.c",
    "hid_engine": "components/hid_engine/hid_engine.c",
    "input_core": "components/input_core/input_core.c",
    "imu_queue": "firmware/main/board_imu_queue.c",
    "motion_capture": "firmware/main/motion_capture.c",
    "sensor_api": "firmware/main/app_sensor_api.c",
    "sensor_wire": "firmware/main/sensor_wire.c",
    "sensor_journal": "firmware/main/board_sensor_journal.c",
    "sensor_usb": "components/control_protocol/control_sensor_usb.c",
    "protocol": "components/control_protocol/control_protocol.c",
    "server": "firmware/main/app_control_server.c",
    "touch": "firmware/main/board_touch_mosaico.c",
    "ui": "firmware/main/app_ui.c",
    "control_trust": "firmware/main/app_control_trust.c",
    "usb_hid": "firmware/main/usb_hid.c",
    "vendor_button": "firmware/managed_components/espressif__button/iot_button.c",
    "vendor_lvgl_adapter": "firmware/managed_components/espressif__esp_lvgl_adapter/src/adapter/esp_lv_adapter.c",
    "vendor_lvgl_touch": "firmware/managed_components/espressif__esp_lvgl_adapter/src/input/esp_lv_adapter_input_touch.c",
    "lvgl_timer": "firmware/managed_components/lvgl__lvgl/src/misc/lv_timer.c",
    "lvgl_indev": "firmware/managed_components/lvgl__lvgl/src/indev/lv_indev.c",
    "sdk_timer": ".tools/esp-idf/components/esp_timer/src/esp_timer.c",
    "btc_thread": ".tools/esp-idf/components/bt/common/btc/core/btc_task.c",
    "osi_thread": ".tools/esp-idf/components/bt/common/osi/thread.c",
    "btc_gap": ".tools/esp-idf/components/bt/host/bluedroid/btc/profile/std/gap/btc_gap_ble.c",
    "btc_gatts": ".tools/esp-idf/components/bt/host/bluedroid/btc/profile/std/gatt/btc_gatts.c",
    "tinyusb_audio": "firmware/build/generated/tinyusb_audio/audio_device.c",
    "tinyusb_fifo": "firmware/managed_components/espressif__tinyusb/src/common/tusb_fifo.c",
    "tinyusb_usbd": "firmware/managed_components/espressif__tinyusb/src/device/usbd.c",
    "tinyusb_dcd": "firmware/build/generated/tinyusb_audio/dcd_dwc2.c",
    "tinyusb_core": "firmware/managed_components/espressif__tinyusb/src/tusb.c",
    "tinyusb_task": "firmware/managed_components/espressif__esp_tinyusb/tinyusb_task.c",
    "tinyusb_driver": "firmware/managed_components/espressif__esp_tinyusb/tinyusb.c",
    "usb_console": "firmware/main/usb_console.c",
}
GENERATED_TINYUSB = {
    "tinyusb_audio": {"filename": "audio_device.c", "manifest_key": None,
        "source_path": "src/class/audio/audio_device.c", "patch_file": "patches/tinyusb-audio-streams.patch"},
    "tinyusb_dcd": {"filename": "dcd_dwc2.c", "manifest_key": "dcd",
        "source_path": "src/portable/synopsys/dwc2/dcd_dwc2.c", "patch_file": "patches/tinyusb-dwc2-observer.patch"},
}
PROVENANCE_FILES = [
    "tools/tinyusb_audio_patch.py", "patches/tinyusb-audio-streams.json",
    "patches/tinyusb-audio-streams.patch", "firmware/tinyusb_audio_patch.cmake",
    "firmware/dependencies.lock",
    "firmware/managed_components/espressif__tinyusb/src/class/audio/audio_device.c",
    "firmware/managed_components/espressif__tinyusb/src/class/audio/audio_device.h",
    "firmware/managed_components/espressif__tinyusb/src/common/tusb_fifo.h",
    "firmware/managed_components/espressif__tinyusb/src/device/dcd.h",
    "patches/tinyusb-dwc2-observer.patch",
    "firmware/managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c",
    "firmware/managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dwc2_common.h",
    "firmware/managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dwc2_type.h",
    "firmware/managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dwc2_esp32.h",
    "firmware/managed_components/espressif__tinyusb/src/device/usbd.h",
    "firmware/managed_components/espressif__tinyusb/src/device/usbd_pvt.h",
    "firmware/managed_components/espressif__tinyusb/src/tusb_option.h",
]
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
# New local management paths run synchronously on the coordinator. BLE/media
# owner request functions copy work for other tasks; those worker stacks are
# deliberately not nested under the submitting coordinator frame.
LOCAL_INTENT = CORE + [("main", "consume_coordinator_event"), ("main", "handle_intent")]
BT_BEGIN = LOCAL_INTENT + [("connectivity", "app_connectivity_controls_bluetooth_start"),
    ("bluetooth_management", "app_bluetooth_management_begin")]
BT_EXACT_BEGIN = LOCAL_INTENT + [("connectivity", "app_connectivity_controls_handle"),
    ("bluetooth_management", "app_bluetooth_management_begin")]
BT_SERVICE = CORE + [("main", "service_connectivity"),
    ("connectivity", "app_connectivity_controls_poll"),
    ("bluetooth_management", "app_bluetooth_management_service")]
BT_CANCEL_STOP = LOCAL_INTENT + [("connectivity", "app_connectivity_controls_bluetooth_stop"),
    ("bluetooth_management", "app_bluetooth_management_cancel")]
BT_CANCEL_CONTEXT = CORE + [("main", "service_connectivity"), ("main", "refresh_connectivity_context"),
    ("connectivity", "app_connectivity_controls_context"),
    ("bluetooth_management", "app_bluetooth_management_cancel")]
BT_POLICY = BT_SERVICE + [("bluetooth_management", "submit_policy"),
    ("ble", "ble_hid_target_preferences_request"), ("ble", "submit_preferences_locked")]
MANAGEMENT_PATHS = {
    "bluetooth_begin_message": BT_BEGIN + [("bluetooth_management", "message")],
    "bluetooth_exact_begin_target": BT_EXACT_BEGIN + [("bluetooth_management", "target_matches")],
    "bluetooth_service_policy_key": BT_SERVICE + [("bluetooth_management", "submit_policy"), ("bluetooth_management", "key")],
    "bluetooth_service_policy_selected": BT_SERVICE + [("bluetooth_management", "submit_policy"), ("bluetooth_management", "same_target")],
    "bluetooth_service_selected_verify": BT_SERVICE + [("bluetooth_management", "same_target")],
    "bluetooth_service_policy_stable": BT_SERVICE + [("bluetooth_management", "policy_stable")],
    "bluetooth_policy_request_key": BT_POLICY + [("ble", "key_for_bond")],
    "bluetooth_policy_request_editable": BT_POLICY + [("ble", "target_editable_locked"), ("ble", "single_forget_busy_locked")],
    "bluetooth_policy_request_blocked": BT_POLICY + [("ble", "target_blocked_locked")],
    "bluetooth_policy_request_name": BT_POLICY + [("ble", "target_editable_locked"), ("ble", "name_busy_locked")],
    "bluetooth_policy_request_match": BT_POLICY + [("ble", "target_key_matches")],
    "bluetooth_service_disconnect": BT_SERVICE + [("ble", "ble_hid_disconnect_generation"), ("ble", "refresh_locked")],
    "bluetooth_service_disconnect_generation": BT_SERVICE + [("ble", "ble_hid_disconnect_generation"), ("ble", "advance_generation_locked")],
    "bluetooth_service_forget_all": BT_SERVICE + [("ble", "ble_hid_forget_bonds_without_pairing"),
        ("ble", "request_forget_bonds"), ("ble", "bond_list_status_locked")],
    "bluetooth_service_forget_all_refresh": BT_SERVICE + [("ble", "ble_hid_forget_bonds_without_pairing"),
        ("ble", "request_forget_bonds"), ("ble", "refresh_locked")],
    "bluetooth_service_forget_one": BT_SERVICE + [("ble", "ble_hid_forget_bond_request"),
        ("ble", "target_editable_locked"), ("ble", "single_forget_busy_locked")],
    "bluetooth_service_forget_one_key": BT_SERVICE + [("ble", "ble_hid_forget_bond_request"), ("ble", "target_key_matches")],
    "bluetooth_service_forget_one_name": BT_SERVICE + [("ble", "ble_hid_forget_bond_request"),
        ("ble", "target_editable_locked"), ("ble", "name_busy_locked")],
    "bluetooth_service_forget_one_receipt": BT_SERVICE + [("ble", "ble_hid_bond_forget_snapshot")],
    "bluetooth_service_manifest_bonds": BT_SERVICE + [("bluetooth_management", "manifest_matches"), ("bluetooth_management", "same_bond")],
    "bluetooth_service_manifest_target": BT_SERVICE + [("bluetooth_management", "manifest_matches"), ("bluetooth_management", "target_matches")],
    "bluetooth_service_pairing_open": BT_SERVICE + [("ble", "ble_hid_pairing_open"), ("ble", "open_pairing_locked")],
    "bluetooth_service_pairing_guard": BT_SERVICE + [("ble", "ble_hid_pairing_open"), ("ble", "target_blocked_locked")],
    "bluetooth_service_pairing_delete_guard": BT_SERVICE + [("ble", "ble_hid_pairing_open"), ("ble", "single_forget_busy_locked")],
    "bluetooth_service_finish_message": BT_SERVICE + [("bluetooth_management", "finish"), ("bluetooth_management", "message")],
    "bluetooth_service_finish_close": BT_SERVICE + [("bluetooth_management", "finish"), ("ble", "ble_hid_pairing_close")],
    "bluetooth_poll_snapshot": CORE + [("main", "service_connectivity"),
        ("connectivity", "app_connectivity_controls_poll"), ("bluetooth_management", "app_bluetooth_management_snapshot")],
    "bluetooth_render_snapshot": CORE + [("main", "render_view"),
        ("connectivity", "app_connectivity_controls_fill"), ("bluetooth_management", "app_bluetooth_management_snapshot")],
    "bluetooth_render_rebuild": CORE + [("main", "render_view"),
        ("connectivity", "app_connectivity_controls_fill"), ("connectivity", "rebuild"),
        ("connectivity", "write_line"), ("connectivity", "write_char"), ("connectivity", "write_char")],
    "bluetooth_begin_revision_copy": LOCAL_INTENT + [("connectivity", "app_connectivity_controls_bluetooth_revisions"),
        ("connectivity", "refresh_owners"), ("ble", "ble_hid_copy_bond_list")],
    "bluetooth_intent_busy_snapshot": LOCAL_INTENT + [("connectivity", "app_connectivity_controls_bluetooth_busy"),
        ("bluetooth_management", "app_bluetooth_management_snapshot")],
    "bluetooth_exact_forget_review": LOCAL_INTENT + [("connectivity", "app_connectivity_controls_handle"),
        ("connectivity", "handle_bluetooth"), ("connectivity", "owner_key")],
    "audio_select_connection_submit": LOCAL_INTENT + [("media_controls", "app_media_controls_handle"),
        ("media_controls", "submit_runtime"), ("media_runtime", "app_media_runtime_submit_local")],
    "audio_select_connection_resolve": LOCAL_INTENT + [("media_controls", "app_media_controls_handle"),
        ("media_controls", "connection_route")],
}
for _name, _prefix in (("begin", BT_BEGIN), ("exact_begin", BT_EXACT_BEGIN)):
    for _leaf, _tail in {
        "stable": [("bluetooth_management", "policy_stable")],
        "close": [("ble", "ble_hid_pairing_close")],
        "message": [("bluetooth_management", "message")],
    }.items():
        MANAGEMENT_PATHS[f"bluetooth_{_name}_{_leaf}"] = _prefix + _tail
for _leaf in ("name_busy_locked", "target_blocked_locked", "single_forget_busy_locked", "advance_generation_locked"):
    MANAGEMENT_PATHS[f"bluetooth_forget_all_{_leaf}"] = BT_SERVICE + [
        ("ble", "ble_hid_forget_bonds_without_pairing"), ("ble", "request_forget_bonds"), ("ble", _leaf)]
for _leaf in ("target_blocked_locked", "advance_generation_locked", "bond_list_status_locked", "refresh_locked"):
    MANAGEMENT_PATHS[f"bluetooth_forget_one_{_leaf}"] = BT_SERVICE + [
        ("ble", "ble_hid_forget_bond_request"), ("ble", _leaf)]
for _name, _prefix in (("begin", BT_BEGIN), ("exact_begin", BT_EXACT_BEGIN), ("service", BT_SERVICE)):
    for _leaf, _tail in {
        "state": [("ble", "ble_hid_snapshot"), ("ble", "pairing_allowed_locked"), ("ble", "single_forget_busy_locked")],
        "policy": [("ble", "ble_hid_target_snapshot"), ("ble", "target_key_matches")],
        "policy_blocked": [("ble", "ble_hid_target_snapshot"), ("ble", "target_blocked_locked")],
        "bonds": [("ble", "ble_hid_copy_bond_list")],
    }.items():
        MANAGEMENT_PATHS[f"bluetooth_{_name}_{_leaf}"] = _prefix + _tail
for _name, _prefix in {
    "stop": BT_CANCEL_STOP, "context": BT_CANCEL_CONTEXT,
    "intent_context": LOCAL_INTENT + [("main", "refresh_connectivity_context"),
        ("connectivity", "app_connectivity_controls_context"), ("bluetooth_management", "app_bluetooth_management_cancel")],
    "poll": CORE + [("main", "service_connectivity"), ("connectivity", "app_connectivity_controls_poll"),
        ("bluetooth_management", "app_bluetooth_management_cancel")],
    "service": BT_SERVICE + [("bluetooth_management", "app_bluetooth_management_cancel")],
}.items():
    for _leaf, _tail in {
        "close": [("ble", "ble_hid_pairing_close")],
        "delete": [("ble", "ble_hid_forget_bond_cancel"), ("ble", "refresh_locked")],
        "delete_status": [("ble", "ble_hid_forget_bond_cancel"), ("ble", "bond_list_status_locked")],
        "message": [("bluetooth_management", "message")],
    }.items():
        MANAGEMENT_PATHS[f"bluetooth_cancel_{_name}_{_leaf}"] = _prefix + _tail
PATHS.update(MANAGEMENT_PATHS)
AUDIO_OBSERVER = CORE + [("main", "service_media"), ("audio_diagnostics", "app_audio_diagnostics_poll")]
AUDIO_DIAGNOSTIC_PATHS = {
    "audio_diagnostics_media_snapshot": AUDIO_OBSERVER + [("audio_diagnostics", "observe"),
        ("media_runtime", "app_media_runtime_snapshot")],
    "audio_diagnostics_usb_snapshot": AUDIO_OBSERVER + [("audio_diagnostics", "observe"),
        ("usb_audio", "usb_audio_snapshot")],
    "audio_diagnostics_transport_snapshot": AUDIO_OBSERVER + [("audio_diagnostics", "observe"),
        ("usb_audio", "usb_audio_transport_snapshot")],
    "audio_diagnostics_codec_snapshot": AUDIO_OBSERVER + [("audio_diagnostics", "observe"),
        ("board_audio", "board_audio_mosaico_status")],
    "audio_diagnostics_job_copy": AUDIO_OBSERVER + [("audio_diagnostics", "observe"), ("audio_diagnostics", "observe_job")],
    "audio_diagnostics_scratch_wipe": AUDIO_OBSERVER + [("audio_diagnostics", "observe"), ("audio_diagnostics", "clear_scratch")],
}
for _caller, _prefix in {
    "service": CORE + [("main", "service_media")],
    "intent": LOCAL_INTENT,
}.items():
    AUDIO_DIAGNOSTIC_PATHS[f"audio_capture_context_{_caller}"] = _prefix + [
        ("main", "refresh_media_context"), ("audio_diagnostics", "app_audio_diagnostics_capture_context")]
for _logger in ("log_audio", "log_job", "log_speech", "log_usb", "log_codec", "log_transport", "log_capture_context"):
    AUDIO_DIAGNOSTIC_PATHS[f"audio_diagnostics_{_logger}"] = AUDIO_OBSERVER + [("audio_diagnostics", _logger)]
for _comparison in ("audio_changed", "job_changed", "usb_changed", "codec_changed", "transport_changed", "capture_context_changed"):
    AUDIO_DIAGNOSTIC_PATHS[f"audio_diagnostics_{_comparison}"] = AUDIO_OBSERVER + [("audio_diagnostics", _comparison)]
AUDIO_DIAGNOSTIC_PATHS["audio_diagnostics_codec_format"] = AUDIO_OBSERVER + [
    ("audio_diagnostics", "codec_changed"), ("audio_helpers", "ae_format_equal")]
AUDIO_DIAGNOSTIC_PATHS["audio_diagnostics_codec_operation"] = AUDIO_OBSERVER + [
    ("audio_diagnostics", "log_codec"), ("audio_diagnostics", "board_audio_sdk_operation_name")]
PATHS.update(AUDIO_DIAGNOSTIC_PATHS)
AUDIO_ADAPTER_PATHS = {
    "audio_select_connection_ready": MANAGEMENT_PATHS["audio_select_connection_resolve"] + [("media_controls", "route_ready")],
    "audio_select_connection_submit_wipe": MANAGEMENT_PATHS["audio_select_connection_submit"] + [("audio_helpers", "ae_secure_zero")],
    "audio_live_start_route": LOCAL_INTENT + [("media_controls", "app_media_controls_handle"),
        ("media_controls", "submit_start"), ("media_controls", "live_route"), ("media_controls", "route_ready")],
    "audio_live_start_submit_wipe": LOCAL_INTENT + [("media_controls", "app_media_controls_handle"),
        ("media_controls", "submit_start"), ("media_controls", "submit_runtime"),
        ("media_runtime", "app_media_runtime_submit_local"), ("audio_helpers", "ae_secure_zero")],
}
for _name, _prefix in {
    "poll": CORE + [("main", "service_media"), ("media_controls", "app_media_controls_poll"), ("media_controls", "rebuild")],
    "intent": LOCAL_INTENT + [("media_controls", "app_media_controls_handle"), ("media_controls", "rebuild")],
    "render": CORE + [("main", "render_view"), ("media_controls", "app_media_controls_fill"), ("media_controls", "rebuild")],
}.items():
    for _leaf, _tail in {
        "connection_ready": [("media_controls", "connection_route"), ("media_controls", "route_ready")],
        "live_profile": [("media_controls", "live_start_block"), ("media_controls", "live_route"), ("media_controls", "route_ready")],
        "live_format": [("media_controls", "live_start_block"), ("media_controls", "format_supported"), ("audio_helpers", "ae_format_equal")],
        "peer_present": [("media_controls", "live_start_block"), ("audio_helpers", "ae_id_empty")],
        "token_valid": [("media_controls", "live_start_block"), ("audio_helpers", "ae_route_token_valid"), ("audio_helpers", "ae_id_empty")],
        "same_peer": [("media_controls", "live_start_block"), ("audio_helpers", "ae_id_equal")],
        "any_jobs": [("media_controls", "live_start_block"), ("media_controls", "any_jobs"), ("media_controls", "job_active")],
        "capture_jobs": [("media_controls", "live_start_block"), ("media_controls", "capture_jobs"), ("media_controls", "job_active")],
        "live_job": [("media_controls", "live_start_block"), ("media_controls", "job_active")],
    }.items():
        AUDIO_ADAPTER_PATHS[f"audio_connection_{_name}_{_leaf}"] = _prefix + _tail
PATHS.update(AUDIO_ADAPTER_PATHS)
MEDIA_PROCESS = [("media_runtime", "worker"), ("media_runtime", "media_step"),
    ("media_runtime", "process_command")]
MEDIA_START = MEDIA_PROCESS + [("media_runtime", "start_job")]
MEDIA_SEND = [("media_runtime", "send_audio"), ("audio_manager", "app_audio_submit")]
AUDIO_RUNTIME_PATHS = {
    # This changed synchronous query is the only declared audio-manager worker
    # path; it does not establish coverage of dispatch or the engine poll loop.
    "audio_manager_playback_capacity": ("audio_manager", [("audio_manager", "worker"),
        ("audio_engine", "ae_playback_capacity"), ("audio_engine", "ae_job_active")]),
    "audio_worker_route_submit": ("media_control", MEDIA_PROCESS + MEDIA_SEND + [("audio_helpers", "ae_secure_zero")]),
    "audio_worker_start_submit": ("media_control", MEDIA_START + MEDIA_SEND + [("audio_helpers", "ae_secure_zero")]),
    "audio_worker_start_wake": ("media_control", MEDIA_START + MEDIA_SEND + [("audio_manager", "wake")]),
    "audio_worker_route_ack": ("media_control", MEDIA_PROCESS + [("audio_manager", "app_audio_take_result"), ("audio_helpers", "ae_secure_zero")]),
    "audio_worker_route_ack_snapshot": ("media_control", MEDIA_PROCESS + [("audio_manager", "app_audio_snapshot")]),
    "audio_worker_identity_failure": ("media_control", MEDIA_PROCESS + [("media_runtime", "finish"), ("audio_helpers", "ae_secure_zero")]),
}
for _name, (_owner, _prefix) in {
    "live_admission": ("main", LOCAL_INTENT + [("media_controls", "app_media_controls_handle"),
        ("media_controls", "submit_start"), ("media_controls", "submit_runtime"),
        ("media_runtime", "app_media_runtime_submit_local"), ("media_runtime", "local_start_identity")]),
    "worker_identity": ("media_control", MEDIA_PROCESS + [("media_runtime", "local_start_identity")]),
    "worker_start": ("media_control", MEDIA_START),
}.items():
    for _leaf, _tail in {
        "token_valid": [("audio_helpers", "ae_route_token_valid"), ("audio_helpers", "ae_id_empty")],
        "token_equal": [("audio_helpers", "ae_route_token_equal"), ("audio_helpers", "ae_id_equal")],
        "usb_factory": [("media_runtime", "route_caps"), ("media_runtime", "route_port"), ("usb_audio", "usb_audio_route_port")],
        "bluetooth_factory": [("media_runtime", "route_caps"), ("media_runtime", "route_port"), ("classic", "bt_audio_route_port")],
        "usb_capabilities": [("media_runtime", "route_caps"), ("usb_audio", "capabilities"), ("usb_audio", "live_locked")],
        "bluetooth_capabilities": [("media_runtime", "route_caps"), ("classic", "port_capabilities"), ("classic", "route_live_locked")],
        "bluetooth_token": [("media_runtime", "route_caps"), ("classic", "port_capabilities"), ("classic", "token_locked")],
        "bluetooth_format": [("media_runtime", "route_caps"), ("classic", "port_capabilities"), ("classic", "format_locked")],
    }.items():
        AUDIO_RUNTIME_PATHS[f"audio_{_name}_{_leaf}"] = (_owner, _prefix + _tail)
for _label, (_owner, _steps) in AUDIO_RUNTIME_PATHS.items():
    PATHS[_label] = _steps
# Conversion and queue admission return before the coordinator services HID.
# The residual splitter adds no task; even its send callback runs on main.
TOUCH_REMOTE = CORE + [("main", "remote_control_service"),
    ("bridge", "app_command_bridge_service"), ("bridge", "execute_flight"),
    ("main", "command_execute"), ("remote_input", "app_remote_input_execute"), ("remote_input", "virtual_touch")]
TOUCH_STEP = CORE + [("hid_engine", "hid_engine_step")]
TOUCH_OBSERVE = CORE + [("main", "observe_input_link"), ("hid_engine", "hid_engine_observe_transport")]
TOUCH_SEND = TOUCH_STEP + [("main", "send_report")]
TOUCH_PATHS = {
    "touch_motion_ready": CORE + [("main", "sync_interaction"), ("interaction", "interaction_set_touch_motion_ready")],
    "touch_remote_rollback_wipe": TOUCH_REMOTE + [("remote_input", "secure_zero")],
    "touch_step_retained_age": TOUCH_STEP + [("hid_engine", "refresh_motion_age")],
    "touch_step_emit_guard": TOUCH_STEP + [("input_core", "input_can_emit")],
    "touch_step_release_ack": TOUCH_STEP + [("input_core", "input_ack_release")],
    "touch_send_trace": TOUCH_SEND + [("main", "motion_trace_count")],
    "touch_send_ble_refresh": TOUCH_SEND + [("ble", "ble_hid_send_generation"), ("ble", "refresh_locked")],
    "touch_send_ble_failure": TOUCH_SEND + [("ble", "ble_hid_send_generation"), ("ble", "record_failure_locked")],
    "touch_send_usb_validate": TOUCH_SEND + [("usb_hid", "usb_hid_send_generation"), ("usb_hid", "report_valid")],
    "touch_send_usb_ready": TOUCH_SEND + [("usb_hid", "usb_hid_send_generation"), ("usb_hid", "ready_locked")],
    "touch_send_usb_fault": TOUCH_SEND + [("usb_hid", "usb_hid_send_generation"), ("usb_hid", "fault_locked"),
        ("usb_hid", "invalidate_locked"), ("usb_hid", "clear_reports_locked")],
}
for _name, _prefix in (("local", LOCAL_INTENT), ("remote", TOUCH_REMOTE)):
    TOUCH_PATHS[f"touch_{_name}_conversion"] = _prefix + [
        ("interaction", "interaction_touch_displacement_owned"), ("interaction", "valid_source")]
    for _leaf, _tail in {
        "buttons": [("hid_engine", "queued_mouse_buttons")],
        "guard": [("hid_engine", "check_enqueue"), ("input_core", "input_can_emit")],
        "push": [("hid_engine", "push")],
        "age": [("hid_engine", "refresh_motion_age")],
    }.items():
        TOUCH_PATHS[f"touch_{_name}_enqueue_{_leaf}"] = _prefix + [
            ("hid_engine", "hid_engine_enqueue_mouse_displacement")] + _tail
for _name, _prefix in {
    "rejected": LOCAL_INTENT + [("main", "cancel_input")],
    "expired": TOUCH_OBSERVE,
    "step_guard": TOUCH_STEP,
    "send_failed": TOUCH_STEP + [("hid_engine", "send_failed")],
}.items():
    for _leaf, _tail in {
        "queue": [("hid_engine", "clear_queue")],
        "release": [("input_core", "input_invalidate")],
    }.items():
        TOUCH_PATHS[f"touch_cancel_{_name}_{_leaf}"] = _prefix + [("hid_engine", "hid_engine_cancel")] + _tail
TOUCH_REMOTE_STOP = TOUCH_REMOTE + [("remote_input", "safety_stop")]
TOUCH_REMOTE_WAIT = TOUCH_REMOTE_STOP + [("remote_input", "safety_wait")]
TOUCH_REMOTE_REVOKE = TOUCH_REMOTE_WAIT + [("remote_input", "revoke")]
TOUCH_REMOTE_CHANGED = TOUCH_REMOTE_REVOKE + [("remote_input", "changed")]
# An authenticated CANCEL enters the owning neutral-release lifecycle. Owner
# transfer clears the queue now; actual zero reports run in a later HID step.
# changed() invokes the coordinator callback and then observes state separately.
TOUCH_REMOTE_CANCEL_PATHS = {
    "touch_remote_cancel_rejected_result": TOUCH_REMOTE_STOP + [("remote_input", "result_for")],
    "touch_remote_cancel_pending_result": TOUCH_REMOTE_WAIT + [("remote_input", "result_for")],
    "touch_remote_cancel_job_sample": TOUCH_REMOTE_REVOKE + [("remote_input", "end_job"), ("remote_input", "sample_text")],
    "touch_remote_cancel_job_preview": TOUCH_REMOTE_REVOKE + [("remote_input", "end_job"), ("interaction", "interaction_send_update")],
    "touch_remote_cancel_job_wipe": TOUCH_REMOTE_REVOKE + [("remote_input", "end_job"), ("remote_input", "secure_zero")],
    "touch_remote_cancel_contact_effect": TOUCH_REMOTE_REVOKE + [("interaction", "interaction_touch_cancel"), ("interaction", "touch_effect")],
    "touch_remote_cancel_contact_none": TOUCH_REMOTE_REVOKE + [("interaction", "interaction_touch_cancel"), ("interaction", "touch_none")],
    "touch_remote_cancel_owner_queue": TOUCH_REMOTE_REVOKE + [("hid_engine", "hid_engine_set_owner"), ("hid_engine", "clear_queue")],
    "touch_remote_cancel_owner_release": TOUCH_REMOTE_REVOKE + [("hid_engine", "hid_engine_set_owner"),
        ("input_core", "input_set_owner"), ("input_core", "input_invalidate")],
    "touch_remote_cancel_release_admit": TOUCH_REMOTE_REVOKE + [("interaction", "interaction_touch_admit"), ("interaction", "touch_none")],
    "touch_remote_cancel_observe_guard": TOUCH_REMOTE_CHANGED + [("remote_input", "observe"), ("remote_input", "terminated")],
    "touch_remote_cancel_observe_ble": TOUCH_REMOTE_CHANGED + [("remote_input", "observe"), ("main", "remote_observe"),
        ("ble", "ble_hid_snapshot"), ("ble", "pairing_allowed_locked"), ("ble", "single_forget_busy_locked")],
    "touch_remote_cancel_observe_usb": TOUCH_REMOTE_CHANGED + [("remote_input", "observe"), ("main", "remote_observe"),
        ("main", "input_link_snapshot"), ("usb_hid", "usb_hid_snapshot")],
    "touch_remote_cancel_callback_ready": TOUCH_REMOTE_CHANGED + [("main", "remote_changed"), ("main", "sync_interaction"),
        ("interaction", "interaction_set_touch_motion_ready")],
}
for _name, _prefix in {
    "model": TOUCH_REMOTE_REVOKE + [("remote_input", "sync_model"), ("interaction", "interaction_sync")],
    "callback": TOUCH_REMOTE_CHANGED + [("main", "remote_changed"), ("main", "sync_interaction"), ("interaction", "interaction_sync")],
}.items():
    for _leaf, _tail in {
        "contact": [("interaction", "interaction_touch_cancel"), ("interaction", "touch_effect")],
        "grid": [("interaction", "reset_grid")],
        "clock": [("interaction", "observe_clock"), ("interaction", "reference_begin")],
    }.items():
        TOUCH_REMOTE_CANCEL_PATHS[f"touch_remote_cancel_sync_{_name}_{_leaf}"] = _prefix + _tail
TOUCH_PATHS.update(TOUCH_REMOTE_CANCEL_PATHS)
PATHS.update(TOUCH_PATHS)
BOARD_AUDIO_SERVICE = [("board_audio", "worker"), ("board_audio", "service")]
BOARD_AUDIO_EXECUTE = BOARD_AUDIO_SERVICE + [("board_audio", "execute")]
BOARD_AUDIO_PATHS = {
    "board_audio_pcm_error": BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_pcm"), ("board_audio", "remember")],
    "board_audio_prepare_error": BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_prepare"), ("board_audio", "remember")],
    "board_audio_unmute_error": BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_unmute"), ("board_audio", "remember")],
    "board_audio_status_publish": BOARD_AUDIO_SERVICE + [("board_audio", "publish")],
}
# Codec SDK calls stay on the existing board worker. Clock enable/disable and
# offline DMA purge are sibling calls; they never run on the submitting task.
for _name, (_prefix, _can_start) in {
    "unmute": (BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_unmute")], True),
    "unmute_cleanup": (BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_unmute"), ("board_audio", "hardware_mute")], True),
    "mute": (BOARD_AUDIO_SERVICE + [("board_audio", "hardware_mute")], True),
    "stop": (BOARD_AUDIO_SERVICE + [("board_audio", "hardware_stop"), ("board_audio", "hardware_mute")], False),
    "execute_stop": (BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_stop"), ("board_audio", "hardware_mute")], False),
    "prepare_cleanup": (BOARD_AUDIO_EXECUTE + [("board_audio", "hardware_prepare"),
        ("board_audio", "hardware_stop"), ("board_audio", "hardware_mute")], False),
}.items():
    for _helper in (("start_tx_clock", "stop_tx_clock") if _can_start else ("stop_tx_clock",)):
        BOARD_AUDIO_PATHS[f"board_audio_{_name}_{_helper}_purge"] = _prefix + [
            ("board_audio", _helper), ("board_audio", "flush_tx"), ("board_audio", "remember")]
        BOARD_AUDIO_PATHS[f"board_audio_{_name}_{_helper}_sdk"] = _prefix + [
            ("board_audio", _helper), ("board_audio", "remember")]
    BOARD_AUDIO_PATHS[f"board_audio_{_name}_stop_guard"] = _prefix + [("board_audio", "interrupted")]
PATHS.update(BOARD_AUDIO_PATHS)
BTC_DISPATCH = [("osi_thread", "osi_thread_run"), ("osi_thread", "osi_thread_run_item"),
    ("btc_thread", "btc_thread_handler")]
BLE_ADVERTISING_PATHS = {
    "ble_advertising_replay_copy": ("main", CORE + [("main", "report_bluetooth_advertising_diagnostics"),
        ("ble", "ble_hid_copy_advertising_diagnostics")]),
    "ble_advertising_replay_name": ("main", CORE + [("main", "report_bluetooth_advertising_diagnostics"),
        ("ble", "ble_hid_advertising_event_name")]),
    "ble_advertising_replay_log": ("main", CORE + [("main", "report_bluetooth_advertising_diagnostics")]),
}
for _name, (_owner, _prefix) in {
    "link_request": ("ble_control", [("ble", "control_worker"), ("ble", "service_link_control"), ("ble", "advertising_request_observation")]),
    "link_return": ("ble_control", [("ble", "control_worker"), ("ble", "service_link_control"), ("ble", "advertising_return_observation")]),
    "name_request": ("ble_control", [("ble", "control_worker"), ("ble", "service_device_name"), ("ble", "advertising_request_observation")]),
    "name_return": ("ble_control", [("ble", "control_worker"), ("ble", "service_device_name"), ("ble", "advertising_return_observation")]),
    "gap_callback": ("btc", BTC_DISPATCH + [("btc_gap", "btc_gap_ble_cb_handler"),
        ("btc_gap", "btc_gap_ble_cb_to_app"), ("ble", "gap_callback")]),
    "gatts_callback": ("btc", BTC_DISPATCH + [("btc_gatts", "btc_gatts_cb_handler"),
        ("btc_gatts", "btc_gatts_cb_to_app"), ("ble", "gatts_callback")]),
}.items():
    for _leaf, _tail in {
        "observing": [("ble", "advertising_observing")],
        "target_flags": [("ble", "advertising_trace_locked"), ("ble", "advertising_flags_locked"), ("ble", "target_blocked_locked")],
        "forget_flags": [("ble", "advertising_trace_locked"), ("ble", "advertising_flags_locked"), ("ble", "single_forget_busy_locked")],
    }.items():
        BLE_ADVERTISING_PATHS[f"ble_advertising_{_name}_{_leaf}"] = (_owner, _prefix + _tail)
for _label, (_owner, _steps) in BLE_ADVERTISING_PATHS.items():
    PATHS[_label] = _steps
BLE_ADMISSION_REPLAY = CORE + [("main", "report_bluetooth_admission_diagnostics")]
BLE_ADMISSION_PATHS = {
    "ble_admission_replay_copy": ("main", BLE_ADMISSION_REPLAY + [("ble", "ble_hid_copy_admission_diagnostics")]),
    "ble_admission_replay_name": ("main", BLE_ADMISSION_REPLAY + [("main", "report_bluetooth_admission_event"),
        ("ble", "ble_hid_admission_event_name")]),
    "ble_admission_replay_event_log": ("main", BLE_ADMISSION_REPLAY + [("main", "report_bluetooth_admission_event")]),
    "ble_admission_replay_log": ("main", BLE_ADMISSION_REPLAY),
}
BLE_ADMISSION_PRODUCERS = {
    "worker_admission": ("ble_control", [("ble", "control_worker"), ("ble", "service_pairing_admission")]),
    "worker_security": ("ble_control", [("ble", "control_worker")]),
    "gap_callback": ("btc", BTC_DISPATCH + [("btc_gap", "btc_gap_ble_cb_handler"),
        ("btc_gap", "btc_gap_ble_cb_to_app"), ("ble", "gap_callback")]),
    "gatts_callback": ("btc", BTC_DISPATCH + [("btc_gatts", "btc_gatts_cb_handler"),
        ("btc_gatts", "btc_gatts_cb_to_app"), ("ble", "gatts_callback")]),
}
# Public window mutations also run synchronously under the coordinator's
# existing management callers. Stop at the API; earlier guard calls returned.
_admission_prefixes = set()
for _label, _steps in MANAGEMENT_PATHS.items():
    for _index, _step in enumerate(_steps):
        if _step not in (("ble", "ble_hid_pairing_open"), ("ble", "ble_hid_pairing_close")):
            continue
        _prefix = _steps[:_index + 1]
        if tuple(_prefix) not in _admission_prefixes:
            _admission_prefixes.add(tuple(_prefix))
            BLE_ADMISSION_PRODUCERS[f"management_{_label}"] = ("main", _prefix)
for _name, _prefix, _api in (
    ("legacy_open", CORE + [("main", "service_pair_operation")], "ble_hid_pairing_open"),
    ("intent_close", LOCAL_INTENT, "ble_hid_pairing_close"),
    ("priority_close", CORE + [("main", "process_priority")], "ble_hid_pairing_close"),
    ("update_close", CORE + [("main", "remote_update_service")], "ble_hid_pairing_close"),
    ("stop_close", LOCAL_INTENT + [("connectivity", "app_connectivity_controls_bluetooth_stop")], "ble_hid_pairing_close"),
    ("context_close", CORE + [("main", "service_connectivity"), ("main", "refresh_connectivity_context"),
        ("connectivity", "app_connectivity_controls_context")], "ble_hid_pairing_close"),
    ("intent_context_close", LOCAL_INTENT + [("main", "refresh_connectivity_context"),
        ("connectivity", "app_connectivity_controls_context")], "ble_hid_pairing_close"),
):
    BLE_ADMISSION_PRODUCERS[_name] = ("main", _prefix + [("ble", _api)])
_remote_submit = CORE + [("main", "remote_control_service")] + DEVICE_SUBMIT[len(CORE):]
# Keep owning adapters even when the current optimized build inlines them.
_remote_submit.insert(_remote_submit.index(("device", "app_remote_device_execute")), ("main", "remote_device_execute"))
_remote_ui = CORE + [("main", "remote_control_service")] + PATHS["remote_ui"][len(CORE):] + [
    ("main", "remote_device_submit_action")]
_remote_approval = CORE + [("main", "remote_control_service"), ("main", "remote_device_service"), ("device", "app_remote_device_service"),
    ("device", "submit_job"), ("main", "remote_device_submit"), ("main", "remote_device_submit_action")]
for _name, _prefix in (("remote_submit", _remote_submit), ("remote_ui", _remote_ui),
                         ("remote_approval", _remote_approval)):
    BLE_ADMISSION_PRODUCERS[f"{_name}_close"] = ("main", _prefix + [("ble", "ble_hid_pairing_close")])
    BLE_ADMISSION_PRODUCERS[f"{_name}_open"] = ("main", _prefix + [
        ("main", "remote_device_bluetooth_progress"), ("ble", "ble_hid_pairing_open")])
BLE_ADMISSION_PRODUCERS["remote_poll_open"] = ("main", CORE + [("main", "remote_control_service"),
    ("main", "remote_device_service"), ("device", "app_remote_device_service"), ("main", "remote_device_poll"),
    ("main", "remote_device_bluetooth_progress"), ("ble", "ble_hid_pairing_open")])
for _name, (_owner, _prefix) in BLE_ADMISSION_PRODUCERS.items():
    for _leaf, _tail in {
        "flags": [("ble", "admission_trace_locked"), ("ble", "admission_flags_locked")],
        "window": [("ble", "admission_trace_locked"), ("ble", "pairing_allowed_locked"),
            ("ble", "single_forget_busy_locked")],
        "finish": [("ble", "admission_trace_finish_locked")],
    }.items():
        BLE_ADMISSION_PATHS[f"ble_admission_{_name}_{_leaf}"] = (_owner, _prefix + _tail)
for _label, (_owner, _steps) in BLE_ADMISSION_PATHS.items():
    PATHS[_label] = _steps
USB_AUDIO_WORKER = [("usb_audio", "worker"), ("usb_audio", "service_io")]
USB_AUDIO_WORKER_PATHS = {
    "usb_audio_worker_count": USB_AUDIO_WORKER + [("usb_audio", "count")],
    "usb_audio_worker_add_count": USB_AUDIO_WORKER + [("usb_audio", "add_count")],
}
for _operation in ("read", "write"):
    USB_AUDIO_WORKER_PATHS[f"usb_audio_worker_fifo_{_operation}"] = USB_AUDIO_WORKER + [
        ("usb_audio", f"tud_audio_{_operation}"), ("tinyusb_audio", f"tud_audio_n_{_operation}"),
        ("tinyusb_audio", f"tu_fifo_{_operation}_n"), ("tinyusb_fifo", f"tu_fifo_{_operation}_n_access_mode")]
# The pinned read implementation retains its frame while calling this leaf.
USB_AUDIO_WORKER_PATHS["usb_audio_worker_fifo_read"].append(("tinyusb_fifo", "tu_fifo_peek_n_access_mode"))
for _name, _tail in {
    "available": [("usb_audio", "tud_audio_available"), ("tinyusb_audio", "tud_audio_n_available"),
        ("tinyusb_audio", "tu_fifo_count")],
    "speed": [("tinyusb_usbd", "tud_speed_get")],
    "live": [("usb_audio", "live_locked")],
    "set": [("tinyusb_audio", "tud_audio_n_fb_set")],
}.items():
    USB_AUDIO_WORKER_PATHS[f"usb_audio_worker_feedback_{_name}"] = [
        ("usb_audio", "worker"), ("usb_audio", "service_feedback")] + _tail
PATHS.update(USB_AUDIO_WORKER_PATHS)
USB_MODE_FLUSH = [("usb_audio", "input_usb_audio_dcd_mode"), ("usb_audio", "replace_link_locked"),
    ("usb_audio", "request_flush_locked")]
USB_REGISTER_SAMPLE = [("tinyusb_dcd", "input_usb_register_sample"), ("usb_audio", "input_usb_audio_dcd_registers")]
USB_MODE_TAILS = {
    "begin_flush": [("tinyusb_dcd", "input_usb_mode_begin")] + USB_MODE_FLUSH,
    "end_reject": [("tinyusb_dcd", "input_usb_mode_end"), ("tinyusb_dcd", "input_usb_mode_reject")],
    "end_flush": [("tinyusb_dcd", "input_usb_mode_end")] + USB_MODE_FLUSH,
    "end_registers": [("tinyusb_dcd", "input_usb_mode_end")] + USB_REGISTER_SAMPLE,
}
USB_TRANSFER_TRACE = [("tinyusb_dcd", "input_usb_transfer_trace"), ("usb_audio", "input_usb_audio_dcd_transfer")]
USB_DCD_SUBMIT = [("tinyusb_usbd", "usbd_edpt_xfer"), ("tinyusb_dcd", "dcd_edpt_xfer")]
TINYUSB_INIT = [("tinyusb_task", "tinyusb_device_task"), ("tinyusb_core", "tusb_rhport_init"),
    ("tinyusb_usbd", "tud_rhport_init"), ("tinyusb_dcd", "dcd_init")]
TINYUSB_CONTROL = [("tinyusb_task", "tinyusb_device_task"), ("tinyusb_task", "tud_task"),
    ("tinyusb_usbd", "tud_task_ext"), ("tinyusb_usbd", "process_setup_received"),
    ("tinyusb_usbd", "invoke_class_control"), ("tinyusb_audio", "audiod_control_xfer_cb"),
    ("tinyusb_audio", "audiod_control_request"), ("tinyusb_audio", "audiod_set_interface")]
USB_TRANSPORT_TASK_PATHS = {}
for _name, _tail in USB_MODE_TAILS.items():
    USB_TRANSPORT_TASK_PATHS[f"tinyusb_init_{_name}"] = ("tinyusb", TINYUSB_INIT + _tail)
for _name, _prefix in (("data", TINYUSB_CONTROL),
                       ("feedback", TINYUSB_CONTROL + [("tinyusb_audio", "audiod_fb_send")])):
    for _counter in ("count", "add_count"):
        USB_TRANSPORT_TASK_PATHS[f"tinyusb_{_name}_submit_{_counter}"] = ("tinyusb", _prefix +
            USB_DCD_SUBMIT + USB_TRANSFER_TRACE + [("usb_audio", _counter)])
USB_TRANSPORT_TASK_PATHS["tinyusb_out_submit_registers"] = ("tinyusb", TINYUSB_CONTROL + USB_DCD_SUBMIT + USB_REGISTER_SAMPLE)
for _callback in ("tud_audio_set_itf_cb", "tud_audio_set_itf_close_ep_cb"):
    USB_TRANSPORT_TASK_PATHS[f"tinyusb_control_{_callback}"] = ("tinyusb", TINYUSB_CONTROL + [
        ("usb_audio", _callback), ("usb_audio", "replace_link_locked"), ("usb_audio", "request_flush_locked")])
USB_TRANSPORT_TASK_PATHS["tinyusb_feedback_parameters"] = ("tinyusb", TINYUSB_CONTROL + [
    ("tinyusb_audio", "audiod_fb_params_prepare"), ("usb_audio", "tud_audio_feedback_params_cb")])
# Stop deletes the device task first, then deinitializes DCD on its caller.
# The production caller here is main's driver-install failure cleanup.
USB_TRANSPORT_TASK_PATHS["tinyusb_startup_failure_deinit"] = ("main", [("main", "app_main"),
    ("usb_console", "usb_console_start"), ("tinyusb_driver", "tinyusb_driver_install"),
    ("tinyusb_task", "tinyusb_task_stop"), ("tinyusb_core", "tusb_deinit"),
    ("tinyusb_usbd", "tud_deinit"), ("tinyusb_dcd", "dcd_deinit")] + USB_MODE_TAILS["begin_flush"])
for _label, (_owner, _steps) in USB_TRANSPORT_TASK_PATHS.items():
    PATHS[_label] = _steps
PATH_OWNERS = {label: "main" for label in PATHS}
PATH_OWNERS.update({label: owner for label, (owner, _) in AUDIO_RUNTIME_PATHS.items()})
PATH_OWNERS.update({label: "board_audio" for label in BOARD_AUDIO_PATHS})
PATH_OWNERS.update({label: owner for label, (owner, _) in BLE_ADVERTISING_PATHS.items()})
PATH_OWNERS.update({label: owner for label, (owner, _) in BLE_ADMISSION_PATHS.items()})
PATH_OWNERS.update({label: "usb_audio" for label in USB_AUDIO_WORKER_PATHS})
PATH_OWNERS.update({label: owner for label, (owner, _) in USB_TRANSPORT_TASK_PATHS.items()})
# These execute on the hardware interrupt stack. They are an explicitly
# unqualified inventory, never task paths and never assigned the task reserve.
INTERRUPT_PATHS = {}
USB_DCD_ISR = [("tinyusb_dcd", "dcd_int_handler")]
for _name, _direction, _tail in (
    ("rx", "rx", [("tinyusb_dcd", "handle_ep_irq"), ("tinyusb_dcd", "handle_epout_dma")]),
    ("tx", "tx", [("tinyusb_dcd", "handle_ep_irq"), ("tinyusb_dcd", "handle_epin_dma")]),
    ("incomplete_in", "tx", [("tinyusb_dcd", "handle_incomplete_iso_in")]),
):
    _complete = USB_DCD_ISR + _tail + [("tinyusb_dcd", "input_usb_dcd_complete")]
    _dispatch = _complete + [("tinyusb_dcd", "dcd_event_xfer_complete"),
        ("tinyusb_usbd", "dcd_event_handler"), ("tinyusb_audio", "audiod_xfer_isr")]
    _class = _dispatch + [("tinyusb_audio", f"audiod_{_direction}_xfer_isr")]
    for _counter in ("count", "add_count"):
        INTERRUPT_PATHS[f"usb_audio_{_name}_{_counter}"] = _class + [
            ("usb_audio", f"tud_audio_{_direction}_done_isr"), ("usb_audio", _counter)]
        INTERRUPT_PATHS[f"usb_dcd_{_name}_complete_{_counter}"] = _complete + USB_TRANSFER_TRACE + [("usb_audio", _counter)]
        INTERRUPT_PATHS[f"usb_dcd_{_name}_rearm_{_counter}"] = _class + USB_DCD_SUBMIT + USB_TRANSFER_TRACE + [("usb_audio", _counter)]
        if _direction == "tx":
            INTERRUPT_PATHS[f"usb_dcd_{_name}_feedback_rearm_{_counter}"] = _dispatch + [
                ("tinyusb_audio", "audiod_fb_send")] + USB_DCD_SUBMIT + USB_TRANSFER_TRACE + [("usb_audio", _counter)]
    if _direction == "rx":
        INTERRUPT_PATHS["usb_class_out_entry"] = _dispatch + [("usb_audio", "input_usb_audio_class_out"), ("usb_audio", "count")]
        INTERRUPT_PATHS["usb_class_out_rearm"] = _class + [("usb_audio", "input_usb_audio_class_out"), ("usb_audio", "count")]
        INTERRUPT_PATHS["usb_dcd_out_rearm_registers"] = _class + USB_DCD_SUBMIT + USB_REGISTER_SAMPLE
INTERRUPT_PATHS["usb_dcd_pending_registers"] = USB_DCD_ISR + USB_REGISTER_SAMPLE
for _name, _tail in USB_MODE_TAILS.items():
    INTERRUPT_PATHS[f"usb_dcd_reset_{_name}"] = USB_DCD_ISR + [("tinyusb_dcd", "handle_bus_reset")] + _tail
INTERRUPT_REQUIRED = {("tinyusb_dcd", "dcd_int_handler"), ("tinyusb_usbd", "dcd_event_handler"),
    ("tinyusb_audio", "audiod_xfer_isr"), ("tinyusb_dcd", "dcd_edpt_xfer"),
    ("usb_audio", "tud_audio_rx_done_isr"), ("usb_audio", "tud_audio_tx_done_isr"),
    ("usb_audio", "input_usb_audio_dcd_mode"), ("usb_audio", "input_usb_audio_dcd_transfer"),
    ("usb_audio", "input_usb_audio_dcd_registers"), ("usb_audio", "input_usb_audio_class_out")}
SENSOR = [("board", "sensor_worker"), ("board", "imu_update")]
STARTUP_IMU = [("main", "app_main"), ("board", "board_init"), ("board", "init_optional_imu")]
APPEND = [("sensor_journal", "board_sensor_journal_append")]
PUBLISH_INPUT = [("board", "board_sensor_journal_publish_input")] + APPEND
USB_POLL = [("control_runtime", "worker"), ("control_runtime", "poll_usb")]
USB_RECEIVE = USB_POLL + [("sensor_usb", "cp_sensor_usb_receive")]
USB_BATCH = USB_RECEIVE + [("control_runtime", "sensor_usb_batch"),
    ("sensor_api", "app_sensor_api_read_batch"), ("sensor_api", "read_page")]
USB_READ = USB_RECEIVE + [("control_runtime", "sensor_usb_read"), ("sensor_api", "app_sensor_api_read")]
HTTP_READ = [("server", "handle_request"), ("protocol", "cp_http"), ("protocol", "http_inner"),
    ("protocol", "get_resource"), ("protocol", "provider_read"), ("server", "read_state"),
    ("control_runtime", "read_resource"), ("sensor_api", "app_sensor_api_read")]
JOURNAL_READ = [("board", "board_sensor_journal_read"),
    ("sensor_journal", "board_sensor_journal_copy"), ("sensor_journal", "board_sensor_journal_bounds")]
LVGL_TOUCH = [("vendor_lvgl_adapter", "lvgl_worker"), ("lvgl_timer", "lv_timer_handler"),
    ("lvgl_timer", "lv_timer_exec"), ("lvgl_indev", "lv_indev_read_timer_cb"),
    ("lvgl_indev", "lv_indev_read"), ("lvgl_indev", "indev_read_core"),
    ("vendor_lvgl_touch", "lvgl_touch_read"), ("touch", "read_contacts")]
BUTTON = [("sdk_timer", "timer_task"), ("sdk_timer", "timer_process_alarm"),
    ("vendor_button", "button_cb"), ("vendor_button", "button_handler")]

# Callback and provider edges are synchronous. Separate branches below avoid
# summing sequential hardware-read, publication, encoding and validation calls
# as if they were all nested on one stack.
SENSOR_PATHS = {
    "sensor_acquisition": ("sensor", SENSOR + [("board", "imu_read_hardware"), ("board", "sensor_u64")]),
    "sensor_rejection": ("sensor", SENSOR + [("board", "imu_read_hardware"),
        ("board", "imu_observation_rejected"), ("board", "sensor_u64")]),
    "sensor_rejection_stage": ("sensor", SENSOR + [("board", "imu_read_hardware"), ("board", "sensor_stage")]),
    "sensor_publication_rejection": ("sensor", SENSOR + [("board", "imu_observation_rejected"), ("board", "sensor_u64")]),
    "sensor_journal_append": ("sensor", SENSOR + [("board", "imu_observation_publish_locked")] + APPEND),
    "sensor_power_read": ("sensor", [("board", "sensor_worker"), ("board", "power_update"),
        ("board", "power_read_hardware"), ("board", "gauge_word")]),
    "sensor_power_journal": ("sensor", [("board", "sensor_worker"), ("board", "power_update")] + APPEND),
    "brightness_worker": ("power", [("board", "power_worker"), ("board", "brightness_update")]),
    "sensor_startup_acquisition": ("main", STARTUP_IMU + [("board", "imu_read_hardware"), ("board", "sensor_u64")]),
    "sensor_startup_rejection": ("main", STARTUP_IMU + [("board", "imu_read_hardware"),
        ("board", "imu_observation_rejected"), ("board", "sensor_u64")]),
    "sensor_startup_journal": ("main", STARTUP_IMU + [("board", "imu_observation_publish_locked")] + APPEND),
    "sensor_startup_failure": ("main", STARTUP_IMU + [("board", "imu_observation_startup_failure"),
        ("board", "imu_observation_rejected"), ("board", "sensor_u64")]),
    "sensor_startup_power": ("main", [("main", "app_main"), ("board", "board_init"),
        ("board", "init_optional_power"), ("board", "power_update")] + APPEND),
    "sensor_lvgl_touch_publish": ("lvgl", LVGL_TOUCH + [("main", "submit_contacts"),
        ("sensor_api", "app_sensor_api_touch")] + PUBLISH_INPUT),
    "sensor_lvgl_touch_reject": ("lvgl", LVGL_TOUCH + [("touch", "reject_scan"),
        ("main", "submit_contacts"), ("sensor_api", "app_sensor_api_touch")] + PUBLISH_INPUT),
    "sensor_lvgl_touch_context": ("lvgl", LVGL_TOUCH + [("main", "submit_contacts"), ("ui", "app_ui_touch_context")]),
    "sensor_button_down_publish": ("esp_timer", BUTTON + [("board", "button_down_callback"),
        ("main", "physical_button"), ("sensor_api", "app_sensor_api_button")] + PUBLISH_INPUT),
    "sensor_button_up_publish": ("esp_timer", BUTTON + [("board", "button_up_callback"),
        ("main", "physical_button"), ("sensor_api", "app_sensor_api_button")] + PUBLISH_INPUT),
    "sensor_usb_batch_read": ("control_runtime", USB_BATCH + JOURNAL_READ),
    "sensor_usb_batch_encode": ("control_runtime", USB_BATCH + [("sensor_wire", "sensor_wire_encode"),
        ("sensor_wire", "sensor_wire_crc32")]),
    "sensor_usb_catalog": ("control_runtime", USB_READ + [("sensor_api", "catalog")]),
    "sensor_usb_latest_imu": ("control_runtime", USB_READ + [("sensor_api", "latest"), ("board", "board_imu_status")]),
    "sensor_usb_latest_power": ("control_runtime", USB_READ + [("sensor_api", "latest"), ("board", "board_power_observe")]),
    "sensor_usb_latest_haptics": ("control_runtime", USB_READ + [("sensor_api", "latest"), ("board", "board_haptics_status")]),
    "sensor_usb_latest_bounds": ("control_runtime", USB_READ + [("sensor_api", "latest"),
        ("board", "board_sensor_journal_status"), ("sensor_journal", "board_sensor_journal_bounds")]),
    "sensor_usb_receive_trust": ("control_runtime", USB_RECEIVE + [("control_runtime", "sensor_usb_allowed"),
        ("control_trust", "app_control_trust_snapshot")]),
    "sensor_usb_receive_link": ("control_runtime", USB_RECEIVE + [("control_runtime", "sensor_usb_allowed"),
        ("usb_hid", "usb_hid_snapshot")]),
    "sensor_usb_service_encode": ("control_runtime", USB_POLL + [("sensor_usb", "cp_sensor_usb_service"), ("sensor_usb", "base64")]),
    "sensor_usb_service_write": ("control_runtime", USB_POLL + [("sensor_usb", "cp_sensor_usb_service"),
        ("control_runtime", "sensor_usb_write")]),
    "sensor_http_batch_read": ("https", HTTP_READ + [("sensor_api", "read_page")] + JOURNAL_READ),
    "sensor_http_batch_encode": ("https", HTTP_READ + [("sensor_api", "read_page"),
        ("sensor_wire", "sensor_wire_encode"), ("sensor_wire", "sensor_wire_crc32")]),
    "sensor_http_base64": ("https", HTTP_READ + [("sensor_wire", "sensor_wire_base64")]),
    "sensor_http_catalog": ("https", HTTP_READ + [("sensor_api", "catalog")]),
    "sensor_http_latest_imu": ("https", HTTP_READ + [("sensor_api", "latest"), ("board", "board_imu_status")]),
    "sensor_http_latest_power": ("https", HTTP_READ + [("sensor_api", "latest"), ("board", "board_power_observe")]),
    "sensor_http_latest_haptics": ("https", HTTP_READ + [("sensor_api", "latest"), ("board", "board_haptics_status")]),
    "sensor_http_latest_bounds": ("https", HTTP_READ + [("sensor_api", "latest"),
        ("board", "board_sensor_journal_status"), ("sensor_journal", "board_sensor_journal_bounds")]),
}
for _publish in ("sensor_lvgl_touch_publish", "sensor_lvgl_touch_reject",
                 "sensor_button_down_publish", "sensor_button_up_publish"):
    _owner, _steps = SENSOR_PATHS[_publish]
    SENSOR_PATHS[_publish + "_validation"] = (_owner,
        _steps[:-1] + [("sensor_journal", "board_sensor_input_valid")])
for _label, (_owner, _steps) in SENSOR_PATHS.items():
    PATHS[_label] = _steps
    PATH_OWNERS[_label] = _owner

# This header function is file-static inline, despite its public-looking name.
# Missing emission is charged to log_codec/poll; emitted bodies/clones still count.
INLINABLE_HEADER_HELPERS = {("audio_diagnostics", "board_audio_sdk_operation_name")}
REQUIRED = set(CORE + [("main", "render_view"), ("main", "report_runtime_diagnostics"),
                       ("main", "report_motion_diagnostics"), ("main", "observe_motion_sample"),
                       ("main", "report_scan_observation"), ("main", "report_reference_diagnostics"),
                       ("main", "report_reference_frame"), ("interaction", "reference_begin"),
                       ("interaction", "reference_rejected"), ("interaction", "invalidate_reference"),
                       ("interaction", "recenter_reference")]) | {
    step for path in PATHS.values() for step in path
    if step[1].startswith(("app_", "ble_", "bt_", "settings_", "device_settings_", "interaction_",
                           "board_", "sensor_wire_", "cp_")) and step not in INLINABLE_HEADER_HELPERS
}
# LVGL's lv_ prefix also names file-static helpers. In the pinned optimized
# object lv_timer_exec is inlined into lv_timer_handler (including its timer
# callback dispatch), so its stack is already in that required caller frame.
# Require the actual public/callback entries explicitly; any emitted helper or
# clone is still conservatively counted by variants_for/evaluate_paths.
REQUIRED.update({("lvgl_timer", "lv_timer_handler"),
    ("lvgl_indev", "lv_indev_read_timer_cb"), ("lvgl_indev", "lv_indev_read")})
REQUIRED.update({("usb_audio", "usb_audio_snapshot"), ("audio_helpers", "ae_secure_zero"),
    ("usb_audio", "usb_audio_route_port"), ("usb_audio", "capabilities"),
    ("classic", "port_capabilities"), ("media_runtime", "worker"),
    ("audio_helpers", "ae_id_empty"), ("audio_helpers", "ae_id_equal"),
    ("audio_helpers", "ae_route_token_valid"), ("audio_helpers", "ae_route_token_equal"),
    ("audio_helpers", "ae_format_equal")})
REQUIRED.update({("board", "sensor_worker"), ("board", "power_worker"),
    ("board", "button_down_callback"), ("board", "button_up_callback"),
    ("main", "physical_button"), ("main", "submit_contacts"),
    ("control_runtime", "worker"), ("server", "handle_request"),
    ("touch", "read_contacts"), ("vendor_button", "button_cb"),
    ("vendor_lvgl_adapter", "lvgl_worker"), ("vendor_lvgl_touch", "lvgl_touch_read"),
    ("sdk_timer", "timer_task"), ("sdk_timer", "timer_process_alarm")})
REQUIRED.update(step for path in TOUCH_PATHS.values() for step in path
    if step[0] == "input_core" or (step[0] == "hid_engine" and step[1].startswith("hid_engine_")))
REQUIRED.update({("main", "send_report"), ("main", "remote_changed"), ("main", "remote_observe"),
    ("usb_hid", "usb_hid_send_generation"), ("usb_hid", "usb_hid_snapshot")})
REQUIRED.add(("board_audio", "worker"))
REQUIRED.update({("audio_manager", "worker"), ("audio_engine", "ae_playback_capacity"),
    ("audio_engine", "ae_job_active")})
REQUIRED.update({("ble", "control_worker"), ("ble", "gap_callback"), ("ble", "gatts_callback"),
    ("main", "report_bluetooth_advertising_diagnostics"), ("osi_thread", "osi_thread_run"),
    ("btc_thread", "btc_thread_handler"), ("btc_gap", "btc_gap_ble_cb_handler"), ("btc_gatts", "btc_gatts_cb_handler")})
REQUIRED.update({("main", "report_bluetooth_admission_diagnostics"), ("main", "report_bluetooth_admission_event")})
REQUIRED.update({("usb_audio", "worker"), ("tinyusb_audio", "tud_audio_n_read"), ("tinyusb_audio", "tud_audio_n_write"),
    ("tinyusb_fifo", "tu_fifo_read_n_access_mode"), ("tinyusb_fifo", "tu_fifo_peek_n_access_mode"),
    ("tinyusb_fifo", "tu_fifo_write_n_access_mode")})
REQUIRED.update({("tinyusb_task", "tinyusb_device_task"), ("tinyusb_task", "tinyusb_task_stop"),
    ("tinyusb_core", "tusb_rhport_init"), ("tinyusb_core", "tusb_deinit"),
    ("tinyusb_usbd", "tud_rhport_init"), ("tinyusb_usbd", "tud_deinit"),
    ("tinyusb_usbd", "tud_task_ext"), ("tinyusb_usbd", "usbd_edpt_xfer"), ("tinyusb_usbd", "tud_speed_get"),
    ("tinyusb_audio", "audiod_control_xfer_cb"), ("tinyusb_audio", "tud_audio_n_available"),
    ("tinyusb_audio", "tud_audio_n_fb_set"), ("tinyusb_dcd", "dcd_init"), ("tinyusb_dcd", "dcd_deinit"),
    ("tinyusb_dcd", "dcd_edpt_xfer"), ("tinyusb_driver", "tinyusb_driver_install"),
    ("usb_console", "usb_console_start"), ("usb_audio", "usb_audio_transport_snapshot"),
    ("usb_audio", "input_usb_audio_dcd_mode"), ("usb_audio", "input_usb_audio_dcd_transfer"),
    ("usb_audio", "input_usb_audio_dcd_registers"), ("usb_audio", "tud_audio_set_itf_cb"),
    ("usb_audio", "tud_audio_set_itf_close_ep_cb"), ("usb_audio", "tud_audio_feedback_params_cb")})
CONSUMERS = [
    "firmware/main/app_motion_capture.inc",
    "firmware/main/main.c", "firmware/main/app_remote_device_bindings.inc",
    "firmware/main/app_remote_device_maintenance.inc", "firmware/main/app_remote_device_read.inc",
    "firmware/main/app_remote_media_bindings.inc", "firmware/main/app_update_bindings.inc",
    "firmware/main/app_media_controls.c",
    "firmware/main/app_connectivity_controls.c", "firmware/main/app_media_runtime.c",
]

BUDGET_FILES = {
    "board": "firmware/main/board_mosaico.c",
    "main": "firmware/main/main.c",
    "runtime": "firmware/main/app_control_runtime.c",
    "media_runtime": "firmware/main/app_media_runtime.c",
    "audio_manager": UNITS["audio_manager"],
    "board_audio": UNITS["board_audio"],
    "ble": UNITS["ble"],
    "bt_common": ".tools/esp-idf/components/bt/common/include/bt_common.h",
    "bt_user_config": ".tools/esp-idf/components/bt/common/include/bt_user_config.h",
    "btc_thread": UNITS["btc_thread"],
    "osi_thread": UNITS["osi_thread"],
    "usb_audio": UNITS["usb_audio"],
    "usb_console": UNITS["usb_console"],
    "tinyusb_defaults": "firmware/managed_components/espressif__esp_tinyusb/include/tinyusb_default_config.h",
    "tinyusb_task": UNITS["tinyusb_task"],
    "tinyusb_driver": UNITS["tinyusb_driver"],
    "isr_config": ".tools/esp-idf/components/freertos/config/riscv/include/freertos/FreeRTOSConfig_arch.h",
    "isr_port": ".tools/esp-idf/components/freertos/FreeRTOS-Kernel/portable/riscv/port.c",
    "isr_type": ".tools/esp-idf/components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h",
    "server": "firmware/main/app_control_server.c",
    "bsp_display": "third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/onboard/display.c",
    "bsp_display_header": "third_party/esp-mosaico-bsp/components/esp-mosaico-bsp/include/bsp/display.h",
    "lvgl_header": "firmware/managed_components/espressif__esp_lvgl_adapter/include/esp_lv_adapter.h",
    "lvgl_adapter": UNITS["vendor_lvgl_adapter"],
    "button": UNITS["vendor_button"],
    "sdk_tasks": ".tools/esp-idf/components/esp_system/include/esp_task.h",
    "sdk_startup": ".tools/esp-idf/components/freertos/app_startup.c",
    "sdk_timer": UNITS["sdk_timer"],
}


def integer_expression(expression):
    """Only the positive literal arithmetic used by the pinned stack sources."""
    expression = re.sub(r"(?<=\d)[uUlL]+\b", "", expression.strip())
    try:
        tree = ast.parse(expression, mode="eval")
    except SyntaxError as error:
        raise RuntimeError(f"Unsupported stack expression: {expression}") from error

    def value(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Mult)):
            left, right = value(node.left), value(node.right)
            return left + right if isinstance(node.op, ast.Add) else left * right
        raise RuntimeError(f"Unsupported stack expression: {expression}")

    result = value(tree.body)
    if result < 0 or result > 1024 * 1024:
        raise RuntimeError(f"Stack expression out of bounds: {expression}")
    return result


def task_budgets(config, root=ROOT, config_path="generated sdkconfig.h"):
    """Resolve allocations from their owning source; never default a task to main.

    ESP-IDF adds a libc supplement to main/timer allocations. Report that real
    allocation, but retain the configured base as the checked application
    budget. This preserves the previous main gate and does not consume SDK's
    extra bytes in addition to the unchanged 2 KiB reserve.
    """
    text = {key: (root / path).read_text() for key, path in BUDGET_FILES.items()}

    def match(key, pattern):
        found = re.search(pattern, text[key], re.M | re.S)
        if not found:
            raise RuntimeError(f"Cannot verify task ownership/budget in {BUDGET_FILES[key]}: {pattern}")
        return found

    def single(key, pattern):
        found = list(re.finditer(pattern, text[key], re.M | re.S))
        if len(found) != 1:
            raise RuntimeError(f"Ambiguous or missing task budget in {BUDGET_FILES[key]}: {pattern}")
        return found[0]

    def function_macro(key, name):
        lines = text[key].splitlines()
        starts = [i for i, line in enumerate(lines) if re.match(r"^#define\s+" + re.escape(name) + r"\s*\(", line)]
        if len(starts) != 1:
            raise RuntimeError(f"Ambiguous or missing task configuration macro: {name}")
        index = starts[0]
        body = [lines[index]]
        while body[-1].rstrip().endswith("\\"):
            index += 1
            if index >= len(lines):
                raise RuntimeError(f"Unterminated task configuration macro: {name}")
            body.append(lines[index])
        return "\n".join(body)

    def configured(name):
        found = list(re.finditer(r"^#define " + re.escape(name) + r" ([^\n]+)$", config, re.M))
        if len(found) != 1:
            raise RuntimeError(f"Configured task stack unavailable: {name}")
        return integer_expression(found[0][1])

    def evidence(keys, expressions):
        return [{"file": BUDGET_FILES[key], "sha256": digest(root / BUDGET_FILES[key])}
                for key in keys] + [{"file": str(config_path),
                    "sha256": hashlib.sha256(config.encode()).hexdigest(),
                    "expressions": expressions}]

    extra_branch = match("sdk_tasks", r"#ifdef CONFIG_LIBC_NEWLIB_NANO_FORMAT\s*"
        r"#define TASK_EXTRA_STACK_SIZE\s+([^\n]+)\s*#else\s*"
        r"#define TASK_EXTRA_STACK_SIZE\s+([^\n]+)\s*#endif")
    nano_defined = bool(re.search(r"^#define CONFIG_LIBC_NEWLIB_NANO_FORMAT(?:\s|$)", config, re.M))
    extra = integer_expression(extra_branch[1 if nano_defined else 2])
    match("sdk_tasks", r"#define ESP_TASK_MAIN_STACK\s+\(CONFIG_ESP_MAIN_TASK_STACK_SIZE\s*\+\s*TASK_EXTRA_STACK_SIZE\)")
    match("sdk_tasks", r"#define ESP_TASK_TIMER_STACK\s+\(CONFIG_ESP_TIMER_TASK_STACK_SIZE\s*\+\s*TASK_EXTRA_STACK_SIZE\)")
    match("sdk_startup", r"ESP_TASK_MAIN_STACK\s*,")
    match("sdk_timer", r"&?timer_task\s*,\s*\"esp_timer\"\s*,\s*ESP_TASK_TIMER_STACK\s*,")
    main, timer = configured("CONFIG_ESP_MAIN_TASK_STACK_SIZE"), configured("CONFIG_ESP_TIMER_TASK_STACK_SIZE")

    board = {}
    for owner, symbol, entry, name in (("sensor", "SENSOR_STACK_BYTES", "sensor_worker", "board_sensor"),
                                       ("power", "POWER_STACK_BYTES", "power_worker", "board_power")):
        size = integer_expression(single("board", rf"\b{symbol}\s*=\s*([^,\n]+)")[1])
        match("board", rf"xTaskCreate\s*\(\s*{entry}\s*,\s*\"{name}\"\s*,\s*{symbol}\s*,")
        board[owner] = (size, symbol)
    runtime = integer_expression(single("runtime", r"xTaskCreate\s*\(\s*worker\s*,\s*\"control_runtime\"\s*,\s*(\d+)\s*,")[1])
    def conditional_worker_stack(key, task):
        sizes = re.findall(r"xTaskCreate(?:WithCaps)?\s*\(\s*worker\s*,\s*\"" + re.escape(task) + r"\"\s*,\s*(\d+)\s*,", text[key])
        if not sizes or len(set(sizes)) != 1:
            raise RuntimeError(f"Cannot verify the {task} worker stack across allocation branches")
        return integer_expression(sizes[0])

    media_stack = conditional_worker_stack("media_runtime", "media_control")
    audio_stack = conditional_worker_stack("board_audio", "board_audio")
    manager_stack = integer_expression(single("audio_manager", r"^#define WORKER_STACK_BYTES\s+([^\n]+)")[1])
    manager_allocation = single("audio_manager", r"xTaskCreate\s*\(\s*worker\s*,\s*\"audio_manager\"\s*,\s*([^,]+),")[1]
    if manager_allocation.strip() != "WORKER_STACK_BYTES":
        raise RuntimeError("Cannot verify the audio_manager worker stack allocation")
    ble_stack = integer_expression(single("ble", r"^#define CONTROL_TASK_STACK\s+([^\n]+)")[1])
    match("ble", r"xTaskCreate\s*\(\s*control_worker\s*,\s*\"ble_hid_control\"\s*,\s*CONTROL_TASK_STACK\s*,")
    btc_stack = configured("CONFIG_BT_BTC_TASK_STACK_SIZE")
    match("sdk_tasks", r"#define BT_TASK_EXTRA_STACK_SIZE\s+TASK_EXTRA_STACK_SIZE")
    match("bt_user_config", r"#ifdef CONFIG_BT_BTC_TASK_STACK_SIZE\s*#define UC_BTC_TASK_STACK_SIZE\s+CONFIG_BT_BTC_TASK_STACK_SIZE")
    match("bt_common", r"#define BT_BTC_TASK_STACK_SIZE\s+UC_BTC_TASK_STACK_SIZE")
    match("btc_thread", r"#define BTC_TASK_STACK_SIZE\s+\(BT_BTC_TASK_STACK_SIZE\s*\+\s*BT_TASK_EXTRA_STACK_SIZE\)")
    match("btc_thread", r"osi_thread_create\s*\(BTC_TASK_NAME,\s*BTC_TASK_STACK_SIZE,\s*BTC_TASK_PRIO,\s*BTC_TASK_PINNED_TO_CORE,\s*"
        r"BTC_TASK_WORKQUEUE_NUM,\s*workqueue_len,\s*false\s*\)")
    match("osi_thread", r"xTaskCreatePinnedToCore\s*\(\s*osi_thread_run\s*,\s*name\s*,\s*stack_size\s*,")
    usb_audio_stack = integer_expression(single("usb_audio", r"xTaskCreate\s*\(\s*worker\s*,\s*\"usb_audio\"\s*,\s*(\d+)\s*,")[1])
    tinyusb_stack = integer_expression(single("tinyusb_defaults", r"^#define TINYUSB_DEFAULT_TASK_SIZE\s+([^\n]+)")[1])
    default_task = function_macro("tinyusb_defaults", "TINYUSB_TASK_DEFAULT")
    if re.findall(r"\.size\s*=\s*([^,\n]+)", default_task) != ["TINYUSB_DEFAULT_TASK_SIZE"]:
        raise RuntimeError("Cannot verify TinyUSB default task stack expression")
    for speed in ("FULL", "HIGH"):
        body = function_macro("tinyusb_defaults", f"TINYUSB_CONFIG_{speed}_SPEED")
        if re.findall(r"\.task\s*=\s*([^,\n]+)", body) != ["TINYUSB_TASK_DEFAULT()"]:
            raise RuntimeError(f"Cannot verify TinyUSB {speed.lower()}-speed task configuration")
    match("usb_console", r"s_driver\s*=\s*TINYUSB_DEFAULT_CONFIG\(usb_device_event\)")
    match("usb_console", r"tinyusb_driver_install\s*\(\s*&s_driver\s*\)")
    if re.search(r"\bs_driver\s*\.\s*task\s*(?:=|\.\s*\w+\s*=)", text["usb_console"]):
        raise RuntimeError("TinyUSB task override requires an explicit ownership update")
    match("tinyusb_driver", r"tinyusb_task_start\s*\(\s*config->port\s*,\s*&config->task\s*,\s*&config->descriptor\s*\)")
    match("tinyusb_task", r"xTaskCreatePinnedToCore\s*\(\s*tinyusb_device_task\s*,\s*\"TinyUSB\"\s*,\s*config->size\s*,")
    https = integer_expression(single("server", r"config\.httpd\.stack_size\s*=\s*([^;]+);")[1])
    match("server", r"httpd_ssl_start\s*\(\s*&s->listener\s*,\s*&config\s*\)")
    match("main", r"board_button_events_register\s*\(\s*physical_button\s*\)")
    match("button", r"button_timer\.callback\s*=\s*button_cb\s*;")
    match("button", r"button_timer\.dispatch_method\s*=\s*ESP_TIMER_TASK\s*;")

    lvgl = integer_expression(single("lvgl_header", r"^#define ESP_LV_ADAPTER_DEFAULT_STACK_SIZE\s+(\([^\n]+?\))")[1])
    match("lvgl_header", r"\.task_stack_size\s*=\s*ESP_LV_ADAPTER_DEFAULT_STACK_SIZE\s*,")
    match("lvgl_adapter", r"s_ctx\.config\.task_stack_size\s*\?\s*s_ctx\.config\.task_stack_size\s*:\s*ESP_LV_ADAPTER_DEFAULT_STACK_SIZE")
    match("lvgl_adapter", r"lvgl_worker\s*,\s*\"lvgl\"\s*,\s*stack_size\s*,")
    match("bsp_display_header", r"\.task_stack_size\s*=\s*0\s*,")
    match("bsp_display", r"adapter\.task_stack_size\s*=\s*active\.task_stack_size\s*;")
    match("board", r"bsp_display_config_t\s+display_config\s*=\s*BSP_DISPLAY_DEFAULT_CONFIG\s*\(\s*\)")
    if re.search(r"display_config\.task_stack_size\s*=", text["board"]):
        raise RuntimeError("Board LVGL stack override requires an explicit ownership update")

    def budget(name, size, keys, expressions, supplement=0):
        if size <= 0:
            raise RuntimeError(f"Invalid task stack budget for {name}: {size}")
        return {"task": name, "budget_bytes": size, "allocated_bytes": size + supplement,
                "sdk_supplement_excluded_bytes": supplement,
                "evidence": evidence(keys, expressions)}

    return {
        "main": budget("main", main, ["sdk_tasks", "sdk_startup"],
            ["CONFIG_ESP_MAIN_TASK_STACK_SIZE", "ESP_TASK_MAIN_STACK"], extra),
        "esp_timer": budget("esp_timer", timer, ["sdk_tasks", "sdk_timer", "button", "board", "main"],
            ["CONFIG_ESP_TIMER_TASK_STACK_SIZE", "ESP_TASK_TIMER_STACK", "ESP_TIMER_TASK"], extra),
        "sensor": budget("board_sensor", board["sensor"][0], ["board"], [board["sensor"][1]]),
        "power": budget("board_power (brightness)", board["power"][0], ["board"], [board["power"][1]]),
        "control_runtime": budget("control_runtime", runtime, ["runtime"], ["xTaskCreate(worker, control_runtime)"]),
        "media_control": budget("media_control", media_stack, ["media_runtime"],
            ["xTaskCreate[WithCaps](worker, media_control)"]),
        "board_audio": budget("board_audio", audio_stack, ["board_audio"],
            ["xTaskCreate[WithCaps](worker, board_audio)"]),
        "audio_manager": budget("audio_manager", manager_stack, ["audio_manager"],
            ["WORKER_STACK_BYTES", "xTaskCreate(worker, audio_manager, WORKER_STACK_BYTES)"]),
        "ble_control": budget("ble_hid_control", ble_stack, ["ble"], ["CONTROL_TASK_STACK"]),
        "btc": budget("BTC_TASK", btc_stack, ["sdk_tasks", "bt_user_config", "bt_common", "btc_thread", "osi_thread"],
            ["CONFIG_BT_BTC_TASK_STACK_SIZE", "BTC_TASK_STACK_SIZE", "osi_thread_create", "xTaskCreatePinnedToCore"], extra),
        "usb_audio": budget("usb_audio", usb_audio_stack, ["usb_audio"], ["xTaskCreate(worker, usb_audio)"]),
        "tinyusb": budget("TinyUSB", tinyusb_stack, ["usb_console", "tinyusb_defaults", "tinyusb_driver", "tinyusb_task"],
            ["TINYUSB_DEFAULT_CONFIG", "TINYUSB_TASK_DEFAULT", "TINYUSB_DEFAULT_TASK_SIZE", "xTaskCreatePinnedToCore(tinyusb_device_task, config->size)"]),
        "https": budget("HTTPS request task", https, ["server"], ["config.httpd.stack_size", "httpd_ssl_start"]),
        "lvgl": budget("lvgl", lvgl, ["board", "bsp_display", "bsp_display_header", "lvgl_header", "lvgl_adapter"],
            ["BSP_DISPLAY_DEFAULT_CONFIG", "ESP_LV_ADAPTER_DEFAULT_STACK_SIZE"]),
    }


def variants_for(frames, module, function):
    return {name: data for name, data in frames[module].items()
            if name == function or name.startswith(function + ".")}


def evaluate_paths(frames, budgets, paths=PATHS, owners=PATH_OWNERS, required=REQUIRED):
    """Pure accounting used by measurement and regression tests."""
    errors, chains = [], {}
    if paths.keys() != owners.keys():
        raise RuntimeError("Each declared stack path requires exactly one task owner")
    for label, steps in paths.items():
        owner = owners[label]
        if owner not in budgets:
            raise RuntimeError(f"Unknown task owner for {label}: {owner}")
        stack_bytes = budgets[owner]["budget_bytes"]
        entries = []
        for module, function in steps:
            variants = variants_for(frames, module, function)
            if not variants and (module, function) in required:
                errors.append(f"Required emitted frame missing: {module}:{function}")
            for data in variants.values():
                if data["kind"] != "static":
                    errors.append(f"Non-static frame requires review: {data['location']}")
            entries.append({"module": module, "function": function,
                "bytes": sum(data["bytes"] for data in variants.values()), "variants": variants,
                "note": "" if variants else "private helper inlined/not emitted"})
        total = sum(entry["bytes"] for entry in entries)
        chains[label] = {"task_owner": owner, "budget_bytes": stack_bytes,
            "allocated_bytes": budgets[owner]["allocated_bytes"],
            "known_bytes": total, "reserve_bytes": SDK_ISR_RESERVE_BYTES,
            "remaining_bytes": stack_bytes - total,
            "remaining_after_reserve_bytes": stack_bytes - total - SDK_ISR_RESERVE_BYTES,
            "frames": entries}
        if total + SDK_ISR_RESERVE_BYTES > stack_bytes:
            errors.append(f"{label} ({owner}): {total} + {SDK_ISR_RESERVE_BYTES} reserve > {stack_bytes}")
    return chains, errors


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measurement_units(build):
    units = dict(UNITS)
    for name, spec in GENERATED_TINYUSB.items():
        generated = build.resolve() / "generated/tinyusb_audio" / spec["filename"]
        units[name] = str(generated.relative_to(ROOT)) if generated.is_relative_to(ROOT) else str(generated)
    return units


def verify_generated_source(build, database, unit):
    """Bind proof to the compiled generated copy, never a managed fallback."""
    contract = GENERATED_TINYUSB[unit]
    source = ROOT / measurement_units(build)[unit]
    manifest_path = ROOT / "patches/tinyusb-audio-streams.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    spec = manifest if contract["manifest_key"] is None else manifest.get(contract["manifest_key"], {})
    if spec.get("source_path") != contract["source_path"] or spec.get("patch_file") != contract["patch_file"]:
        raise RuntimeError(f"Unexpected generated TinyUSB source/patch scope: {unit}")
    upstream = ROOT / "firmware/managed_components/espressif__tinyusb" / contract["source_path"]
    patch_path = ROOT / contract["patch_file"]
    checked = {}
    for kind, path, expected in (("postimage", source, spec.get("output_sha256")),
                                 ("preimage", upstream, spec.get("source_sha256")),
                                 ("patch", patch_path, spec.get("patch_sha256"))):
        if path.is_symlink() or not path.is_file():
            raise RuntimeError(f"Generated TinyUSB {unit} {kind} missing or has unexpected type: {path}")
        checked[kind] = digest(path)
        if checked[kind] != expected:
            raise RuntimeError(f"Generated TinyUSB {unit} differs from reviewed {kind}: {path}")
    entries = [entry for entry in database if Path(entry["file"]).resolve() == source.resolve()]
    if len(entries) != 1 or any(Path(entry["file"]).resolve() == upstream.resolve() for entry in database):
        raise RuntimeError(f"Expected exactly one generated TinyUSB {unit} compile input and no managed fallback")
    return {"source": str(source), "sha256": checked["postimage"], "manifest": str(manifest_path),
            "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(), "managed_source": str(upstream),
            "managed_source_sha256": checked["preimage"], "patch": str(patch_path),
            "patch_sha256": checked["patch"], "generated_compile_entries": len(entries)}


def verify_generated_audio_source(build, database):
    return verify_generated_source(build, database, "tinyusb_audio")


def verify_generated_dcd_source(build, database):
    return verify_generated_source(build, database, "tinyusb_dcd")


def verify_generated_source_snapshot(proof, snapshot):
    for path_key, hash_key in (("source", "sha256"), ("manifest", "manifest_sha256"),
                               ("managed_source", "managed_source_sha256"), ("patch", "patch_sha256")):
        path = Path(proof[path_key])
        key = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
        if snapshot.get(key) != proof[hash_key]:
            raise RuntimeError(f"Generated TinyUSB proof input changed while being read: {path}")


def verify_generated_audio_snapshot(proof, snapshot):
    verify_generated_source_snapshot(proof, snapshot)


def interrupt_stack_budget(config, root=ROOT):
    found = re.findall(r"^#define CONFIG_FREERTOS_ISR_STACKSIZE ([^\n]+)$", config, re.M)
    if len(found) != 1:
        raise RuntimeError("Configured interrupt stack unavailable: CONFIG_FREERTOS_ISR_STACKSIZE")
    checks = {
        "isr_config": r"#define configISR_STACK_SIZE\s+\(\s*CONFIG_FREERTOS_ISR_STACKSIZE\s*\)",
        "isr_type": r"#define portSTACK_TYPE\s+uint8_t",
        "isr_port": r"StackType_t xIsrStack\[portNUM_PROCESSORS\]\[configISR_STACK_SIZE\]",
    }
    for key, pattern in checks.items():
        if not re.search(pattern, (root / BUDGET_FILES[key]).read_text()):
            raise RuntimeError(f"Cannot verify interrupt stack allocation in {BUDGET_FILES[key]}")
    return integer_expression(found[0])


def interrupt_frame_inventory(frames, config, paths=INTERRUPT_PATHS):
    """Non-qualifying interrupt evidence, independent of task reserve/gates."""
    capacity = interrupt_stack_budget(config)
    def application_step(module, function):
        return module == "usb_audio" or (module == "tinyusb_dcd" and function.startswith("input_usb_"))

    entries_by_path, errors = {}, []
    for label, steps in paths.items():
        entries, complete, application_complete = [], True, True
        for module, function in steps:
            variants = variants_for(frames, module, function)
            missing = not variants and (module, function) in INTERRUPT_REQUIRED
            dynamic = any(value["kind"] != "static" for value in variants.values())
            if missing or dynamic:
                complete = False
                if application_step(module, function):
                    application_complete = False
                errors.append(f"Interrupt inventory incomplete: {module}:{function}")
            entries.append({"module": module, "function": function,
                "bytes": None if missing else sum(value["bytes"] for value in variants.values()),
                "variants": variants, "note": "required emitted frame missing" if missing else
                    ("" if variants else "private helper inlined/not emitted")})
        measured = sum(entry["bytes"] or 0 for entry in entries)
        application = sum(entry["bytes"] or 0 for entry in entries
            if application_step(entry["module"], entry["function"]))
        exceeded = (application_complete and application > capacity) or (complete and measured > capacity)
        if exceeded:
            errors.append(f"{label}: known interrupt frames exceed configured {capacity}-byte stack; review required")
        entries_by_path[label] = {"context": "hardware DCD ISR", "qualification": "BLOCKED_KNOWN_FRAMES" if exceeded else "PENDING",
            "configured_stack_bytes": capacity, "known_application_bytes": application if application_complete else None,
            "known_added_application_bytes": application if application_complete else None,
            "known_selected_chain_bytes": measured if complete else None, "frames": entries,
            "task_reserve_applied": False, "safety_pass_claimed": False,
            "pre_change_application_bytes": None, "application_delta_bytes": None}
    return {"status": "BLOCKED" if errors else "UNQUALIFIED", "configured_stack_bytes": capacity,
        "paths": entries_by_path, "task_reserve_applied": False, "safety_pass_claimed": False,
        "application_scope": "Separately emitted usb_audio and generated DCD input_usb_* helper frames. "
                             "Inlined generated helpers are included in their selected caller frame; "
                             "their application-only portion cannot be attributed separately.",
        "unmeasured_costs": ["interrupt entry and RTOS context", "SDK paths outside the selected chain", "interrupt nesting"],
        "pre_post_limit": "No comparable pre-change hook-chain measurement is inferred from absent symbols; current added application frames only.",
        "limits": "Fitting known frames alone does not qualify interrupt safety. Physical high-water and nesting remain unmeasured."}, errors


def input_snapshot(build, database, units=None):
    """Hash the bounded first-party/configuration inputs, including file sets."""
    units = units or measurement_units(build)
    paths = {Path(__file__).resolve(), build / "compile_commands.json",
             build / "config/sdkconfig.h", ROOT / "firmware/sdkconfig"}
    for directory in (ROOT / "firmware/main", ROOT / "components"):
        paths.update(path for path in directory.rglob("*")
                     if path.is_file() and path.suffix in (".c", ".h", ".inc", ".S"))
    paths.update(ROOT / path for path in units.values())
    paths.update(ROOT / path for path in BUDGET_FILES.values())
    paths.update(ROOT / path for path in PROVENANCE_FILES)
    paths.update(path for path in (build / "toolchain").glob("*") if path.is_file())
    # Forced headers and response files can live outside the first-party tree.
    measured_sources = {ROOT / path for path in units.values()}
    for entry in database:
        if Path(entry["file"]).resolve() not in measured_sources:
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
    entry = next((item for item in database if Path(item["file"]).resolve() == source.resolve()), None)
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
    units = measurement_units(build)
    try:
        generated_audio = verify_generated_audio_source(build, database)
        generated_dcd = verify_generated_dcd_source(build, database)
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    before = input_snapshot(build, database, units)
    try:
        verify_generated_audio_snapshot(generated_audio, before)
        verify_generated_source_snapshot(generated_dcd, before)
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    config_bytes = (build / "config/sdkconfig.h").read_bytes()
    for path, content in ((build / "compile_commands.json", database_bytes),
                          (build / "config/sdkconfig.h", config_bytes)):
        key = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
        if hashlib.sha256(content).hexdigest() != before[key]:
            raise SystemExit(f"Compilation input changed while being read: {path}")
    config = config_bytes.decode()
    try:
        budgets = task_budgets(config, config_path=build / "config/sdkconfig.h")
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    stack_bytes = budgets["main"]["budget_bytes"]
    paths = set(units.values()) | set(CONSUMERS) | set(BUDGET_FILES.values()) | set(PROVENANCE_FILES)
    legacy = [path for path in CONSUMERS if re.search(
        r"\b(?:ble_hid_bond_list|bt_audio_bond_list)\s*\(", (ROOT / path).read_text())]
    with ThreadPoolExecutor(max_workers=3) as pool:
        frames = dict(pool.map(lambda unit: compile_unit(unit, database, output), units.items()))
    chains, errors = evaluate_paths(frames, budgets)
    interrupt_inventory, interrupt_errors = interrupt_frame_inventory(frames, config)
    if legacy:
        errors.append("Large by-value bond getter remains in: " + ", ".join(sorted(legacy)))
    startup_variants = variants_for(frames, "main", "app_main")
    startup = sum(frame["bytes"] for frame in startup_variants.values()) if startup_variants else None
    if startup is None or startup > STARTUP_FRAME_LIMIT_BYTES:
        errors.append(f"Startup frame {startup} exceeds {STARTUP_FRAME_LIMIT_BYTES} bytes")
    after = input_snapshot(build, database, units)
    changed = [path for path in before.keys() | after.keys()
               if before.get(path) != after.get(path)]
    if changed:
        errors.append("Compilation inputs changed during measurement: " + ", ".join(sorted(changed)))
    task_gate_status = "FAIL" if errors else "PASS"
    errors.extend(interrupt_errors)
    report = {"status": "FAIL" if errors else "PASS", "configured_stack_bytes": stack_bytes,
              "task_gate_status": task_gate_status, "interrupt_safety_qualified": False,
              "task_budgets": budgets,
              "sdk_isr_reserve_bytes": SDK_ISR_RESERVE_BYTES,
              "startup_frame_limit_bytes": STARTUP_FRAME_LIMIT_BYTES,
              "generated_audio_source": generated_audio, "generated_dcd_source": generated_dcd,
              "interrupt_frame_inventory": interrupt_inventory,
              "source_hashes": {path: after[path] for path in sorted(paths)},
              "input_hashes": after, "changed_during_measurement": sorted(changed),
              "input_scope": "First-party firmware/main and components C/headers/includes, "
                             "measured vendor/SDK translation units and task-ownership budget sources, "
                             "this checker, compile database, sdkconfig, generated config header, "
                             "toolchain files, command response files, forced headers and generated TinyUSB audio/DCD provenance. "
                             "Other SDK/vendor dependencies are not covered by this drift check.",
              "paths": chains, "all_frames": frames, "errors": errors,
              "limits": "Task PASS covers declared task paths only. Interrupt inventory remains explicitly unqualified. "
                        "Audio-manager worker coverage is limited to the playback-capacity query, excluding dispatch and engine polling. "
                        "Declared task/callback paths include named measured vendor wrappers. "
                        "Main/timer SDK libc stack supplements are reported but excluded from the checked "
                        "application budget. Clone sums are conservative; no "
                        "SDK/binary/ISR high-water proof or device qualification is claimed."}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    for label, chain in chains.items():
        print(f"{label}: {chain['known_bytes']} known + {SDK_ISR_RESERVE_BYTES} reserved "
              f"/ {chain['budget_bytes']} bytes ({chain['task_owner']}; "
              f"allocated {chain['allocated_bytes']})")
    print(f"Interrupt inventory: {interrupt_inventory['status']}; "
          f"configured {interrupt_inventory['configured_stack_bytes']} bytes; no ISR safety PASS")
    print(f"{report['status']}: stack budget (task gates only; interrupt safety unqualified); evidence {output}")
    if errors:
        raise SystemExit("\n".join(errors))


if __name__ == "__main__":
    main()
