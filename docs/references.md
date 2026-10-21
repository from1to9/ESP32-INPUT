# 来源与许可记录

核对日期：2026-10-21。在线 master 可能变化；实际依赖以 Git 子模块和后续 SDK 锁定记录为准。

| 来源 | 用途 | 许可/边界 |
| --- | --- | --- |
| [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp) | 板卡、IMU、触摸、LVGL、音频、按钮、电源 | Apache-2.0；本项目固定 `5f10831ddf8697680121e72ba0fd3dce7f7f1799` |
| [tomokusaba/m5stack-ble-mouse](https://github.com/tomokusaba/m5stack-ble-mouse) | BMI270 空鼠行为、触摸优先、防抖、校准参考 | 未发现 LICENSE；不复制源码或移植整个固件 |
| [PinToCode/ESP32-air-pointer](https://github.com/PinToCode/ESP32-air-pointer) | 运动曲线参考 | MIT；MPU9250 + 单触摸按钮，不是触屏产品 |
| [esp-idf/esp_hid_device](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/esp_hid_device) | 标准 HID 示例 | 使用前逐文件保留许可；需自行组合鼠标/键盘及适配目标 |
| [a2dp_gatts_coex](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/coex/a2dp_gatts_coex) | S31 BLE + Classic 共存实验基础 | GATT 示例不等于 HID + HFP 已验证 |
| [hfp_hf](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/hfp_hf) | HFP 麦克风/会议 | S31 支持 HCI 音频路径，须连接板上 codec |
| [a2dp_sink_stream](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream) | 扬声器 | 接收音乐音频，不提供麦克风上传 |
| [espressif/esp-sr](https://github.com/espressif/esp-sr) | S31 `mn7_en` 英语口令 | 文档/API、预编译库、模型许可分别核对；不是整句转写 API |
| [lokutor-ai/oido](https://github.com/lokutor-ai/oido) | 后续整句英语离线 ASR 实验 | 代码 GPLv3；模型 CC-BY/CC-BY-SA；S3 特定 SIMD 需移植，不纳入当前依赖 |
| [petewarden/magic_wand](https://github.com/petewarden/magic_wand) | 后续手势数据采集/训练参考 | 不是完整字母输入法；加入前核对具体代码许可 |

M5 项目针对 M5StickS3/CoreS3，不支持直接刷入 Mosaico；不同 MCU、BSP、触屏和蓝牙栈需要重新集成。本文引用描述的是设计参考，不是兼容承诺。

## 官方兼容依据

- [Mosaico v1.2 硬件](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html)：480×480、BMI270、ES8311、AI/BOOT/POWER。板卡指南与 BSP 命名存在版本差异，实际初始化以固定 BSP 为准。
- [S31 蓝牙 API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/bluetooth/index.html)：Bluedroid 支持双模；部分概述页可能滞后，应结合具体示例和 SDK 配置验证。
- [Apple AssistiveTouch 指针设备](https://support.apple.com/en-us/111775)：iPhone 的接入路径与设置。
- [Windows Classic Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-classic-audio)：A2DP 为播放，HFP 为双向语音，使用麦克风时的质量与端点行为。
- [USB UAC 组件](https://github.com/espressif/esp-iot-solution/tree/master/components/usb/usb_device_uac)：清单列出 S31，但文档有跨系统配置限制，不据此承诺同一固件即插即用覆盖所有系统。

## 本次实现来源

`components/input_core` 与 `design` 是本项目原创；不包含 M5 源码。BSP 通过 Git 子模块分发，不在本仓库中重写其许可。没有提交任何语音模型权重或上游二进制库。
