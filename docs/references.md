# Sources and licensing

Reviewed 2026-10-21. Online branches may change; executable dependencies use pinned commits and resolved lockfiles.

| Source | Purpose | License / boundary |
| --- | --- | --- |
| [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp) | Board, IMU, touch, LVGL, audio, buttons and power | Apache-2.0; pinned at `5f10831ddf8697680121e72ba0fd3dce7f7f1799` |
| [tomokusaba/m5stack-ble-mouse](https://github.com/tomokusaba/m5stack-ble-mouse) | BMI270 air mouse, touch priority, freeze and calibration behavior | No LICENSE found; do not copy source or port the entire firmware |
| [PinToCode/ESP32-air-pointer](https://github.com/PinToCode/ESP32-air-pointer) | Motion curve reference | MIT; MPU9250 plus one touch button, not a touchscreen product |
| [esp-idf/esp_hid_device](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/esp_hid_device) | Standard HID reference | Preserve file-level licenses; composite reports and host qualification remain integration work |
| [a2dp_gatts_coex](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/coex/a2dp_gatts_coex) | S31 BLE/Classic coexistence experiment | Generic GATT coexistence does not prove HID + HFP |
| [hfp_hf](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/hfp_hf) | Microphone and meeting audio | S31 HCI audio path needs board codec integration |
| [a2dp_sink_stream](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream) | Speaker playback | Music reception, not microphone upload |
| [espressif/esp-sr](https://github.com/espressif/esp-sr) | S31 mn7_en command recognition | Review docs, binary libraries and models separately; not a full transcription API |
| [lokutor-ai/oido](https://github.com/lokutor-ai/oido) | Future offline English transcription experiment | GPLv3 code and CC-BY/CC-BY-SA models; S3 SIMD requires porting; not a current dependency |
| [petewarden/magic_wand](https://github.com/petewarden/magic_wand) | Future gesture collection/training reference | Not a complete alphabet input method; review exact licenses before reuse |

M5 examples target M5StickS3/CoreS3 and cannot simply be flashed to Mosaico. MCU, BSP, touch and stack differences require integration. Behavioral reference is not compatibility evidence.

## Official compatibility sources

- [Mosaico hardware guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html): 480 x 480 display, BMI270, ES8311 and AI/BOOT/POWER. Resolve version differences through the pinned BSP and actual board.
- [S31 Bluetooth API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/bluetooth/index.html): Bluedroid dual-mode direction; verify actual SDK configuration and examples where overview pages lag.
- [Apple pointer devices](https://support.apple.com/en-us/111775): iPhone AssistiveTouch setup.
- [Windows Classic Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-classic-audio): A2DP playback versus HFP duplex voice and endpoint behavior.
- [USB UAC component](https://github.com/espressif/esp-iot-solution/tree/master/components/usb/usb_device_uac): S31 appears in the manifest, but cross-platform restrictions still require qualification.

## Implementation provenance

`components/input_core` and `design` are original project work, with no M5 implementation copied. The BSP remains an upstream submodule. No speech model weights or third-party binary libraries are committed.

General engineering rules are adapted from QuantTrade's current working agreement and workflows, with domain-specific requirements removed. See [engineering standards](engineering.md#adaptation-source).
