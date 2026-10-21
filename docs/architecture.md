# 技术架构与接口

## 选择

- ESP-IDF 6.2+，ESP32-S31；固定 [官方 Mosaico BSP](https://github.com/esp-mosaico/esp-mosaico-bsp) 提交，不追踪浮动 master 作为可复现基线。
- LVGL 9.x 由 BSP 管理屏幕和触控；产品 UI 只使用 BSP 高层 API。
- 蓝牙首选 **Bluedroid**：同一主机栈支持 BLE HID 与经典 HFP/A2DP，避免先绑定仅 BLE 的库再重写。共存实验先独立验证。
- 组合 HID 报告描述符在鼠标与键盘模式间保持不变；模式切换仅改变输入生产者，不通过反复改名称/重启切换设备类型。
- 第一版鼠标报告使用标准相对 X/Y、滚轮、按键；键盘使用标准 6KRO 报告和 modifier。文本编码与报告传输分离。

## 模块边界

```text
BMI270 / touch / AI key / speech letters / remote actions
                     │
              input coordinator
       mode + owner + epoch + output gates
             ┌───────┴────────┐
          mouse             keyboard
    filter/integrate     selector → preview → queue
             └───────┬────────┘
                HID transport
                 BLE host

ES8311 ↔ audio manager ↔ HFP / A2DP / optional UAC
                  └──→ AFE / speech-letter recognizer
```

UI 发出意图，由唯一协调器更新状态。蓝牙回调和识别结果仅投递事件，不直接改变 LVGL 或发送字符。

## 三种模式与并行能力

`foreground_mode = mouse | keyboard | audio`。

`audio_state = off | voice_muted | voice_active | speaker` 独立保存。只有 `coexistence_verified` 能力打开且目标匹配时，允许 foreground 为 mouse/keyboard 而 audio_state 非 off。单独编译通过不应设置 verified 标志；应由目标/固件版本的验证配置控制。

运行时所有新队列项都带 `epoch` 和目标标识。主模式变化、断线、换目标、锁定、接管都会递增 epoch，取消残留。传输层发送前检查 epoch、连接、owner 和 mode。

最初使用一个 HID 目标。音频目标默认同一物理主机，BLE 和 Classic 可能出现两个配对入口。不同主机的并行另设两个 peer，不复用一个模糊 `connected` 变量。

## 运动与选字

- IMU 默认 100 Hz 起步，使用单调时钟 `dt`，长间隔/NaN 丢弃并清余量。
- gyro bias 通过静置采样估计。死区→滤波→按时间积分→灵敏度→小数余量→有界 HID 位移。
- 切换输入来源时清积压，不允许拖动结束后追赶旧运动。
- 选字复用角速度积分但输出是本机网格焦点。每次确认重置参考点，采用边界滞回和焦点高亮，不自动 dwell 提交。
- 坐标轴方向以机身握持方向实测；不把磁力计当默认航向源，主机磁吸结构会影响磁场。

## 音频预算

Bluetooth 控制器、BLE HID、经典 SCO/A2DP 共用射频时隙。调度上先保证 key-up/连接处理，再保证音频环形缓冲，最后 UI 动画和实验识别。禁止在蓝牙或音频回调里写 NAND、同步等待 LVGL 或做整句 ASR。

一个 audio manager 初始化 codec，维护采集/播放格式、静音和 ownership，识别器只订阅音频帧。板上英语字母识别优先使用 ESP-SR `mn7_en`；整句 ASR 使用独立实验构建，不和首版流式音频争资源。

初始阶段只定义能力接口，不以空函数返回成功假装完成音频。独立 HFP、A2DP 通过后，再做 BLE HID + HFP 压力测试。USB 音频与 CDC 也由唯一 USB 管理器控制，避免 BSP 控制台与第二个 TinyUSB 实例冲突。

## 后续 Mac 控制接口（本轮不实现）

版本化动作：`move_relative`、`mouse_button`、`scroll`、`key`、`text_ascii`、`release_all`、`get_state`。每个命令包含 protocol_version、session_id、request_id、epoch、target_id、deadline。响应区分 accepted / sent / rejected，不把 sent 命名为“目标界面执行成功”。

远程动作有短租约，失联立即清队列并释放；没有未来时间无限排队。首次配对授权后无需逐条确认；本地接管优先。Mac 画面识别和动作验证独立于 ESP32，设备不充当 iPad 视频中转。
