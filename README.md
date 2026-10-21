# ESP32-INPUT

基于 **ESP-Mosaico / ESP32-S31** 的体感与触屏输入设备。优先兼容 iPhone、iPad、Windows 和 macOS，基础键鼠使用标准蓝牙 HID，不要求目标设备安装专用程序。

## 三种主模式

| 模式 | 主要操作 | 第一版边界 |
| --- | --- | --- |
| 鼠标（默认） | 单击切换体感、触控板、精调、点击、拖动、滚动 | 转动手腕移动；停止转动则停止；触屏暂时接管体感 |
| 键盘 | 体感选字、触屏选字、快捷键、字母预览与发送 | 体感只移动本机选择焦点；不移动目标设备指针 |
| 麦克风／音响 | 麦克风、会议双向音频、扬声器 | 独立模式优先；并行作为单独验收项 |

键盘模式预留 **英语字母口令**，例如 `Alpha → A`，与体感选字共用字符缓冲区。任意英语整句的板上离线听写、空中手写识别属于实验范围，不作为首版承诺。

## 当前状态

这是开发基线，不是可直接使用的成品固件。

GitHub：[`from1to9/ESP32-INPUT`](https://github.com/from1to9/ESP32-INPUT)（私有）。

- 已固定官方 BSP 子模块版本，保留上游许可。
- 已整理需求、模式和按键状态、兼容矩阵、技术架构与分阶段验收。
- `design/` 提供可直接打开的交互原型；不连接真实蓝牙或麦克风。
- `components/input_core/` 提供独立于硬件的输入仲裁、体感计算和键盘编码基础。
- `firmware/` 是 BSP 启动验证工程，尚未接通蓝牙 HID、LVGL 产品界面或音频链路。
- 没有声称已完成 iOS / Windows / Mac 实机兼容测试。

主机 C11 测试已通过，开启严格警告及地址/未定义行为检查。浏览器交互检查记录见 [验证记录](docs/validation.md)。下一步是固定 ESP-IDF 版本、交叉编译并接通最小 BLE 组合 HID。

## 文档

- [开发需求](docs/requirements.md)
- [界面与按键设计](docs/interaction.md)
- [架构与协议](docs/architecture.md)
- [兼容与验收](docs/validation.md)
- [开发顺序与待定项](docs/roadmap.md)
- [研究来源与许可证](docs/references.md)
- [界面原型](design/index.html)

## 本地开发

```sh
git submodule update --init --recursive
python3 tools/check.py
python3 -m http.server 8768
```

浏览器打开 `http://localhost:8768/design/`。原型不依赖 npm 或在线资源。

固件需要 ESP-IDF 6.2 或更新版本且包含 ESP32-S31 支持；本机未安装该工具链时，主机测试仍可运行。

```sh
cd firmware
idf.py --preview set-target esp32s31
idf.py build
```

烧录前确认板卡版本、连接端口及原厂固件恢复途径。未知 eFuse 版本应停止初始化，不能猜 GPIO。具体限制见 [验证记录](docs/validation.md)。

## 开源策略

本项目原创代码采用 Apache-2.0。官方 BSP 是固定提交的子模块；M5 项目只作为行为参考，未复制其未声明许可证的实现。语音模型、二进制库和实验 ASR 必须分别记录许可，不能仅用仓库顶层许可覆盖它们。
