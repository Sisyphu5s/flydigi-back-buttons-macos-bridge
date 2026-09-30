# 致谢与参考

本项目建立在许多公开项目、规范、逆向记录和硬件经验之上。感谢下列作者和维护者公开代码、协议资料与实测结果。本项目没有把这些仓库整体复制进公开树；各项目的原始许可证和贡献者归属仍以其仓库为准。

## 最直接的实现先例

- [`ruomox/Flydigi5Pico`](https://github.com/ruomox/Flydigi5Pico)：RP2350 + PIO-USB host + TinyUSB device 的双核桥接骨架、1 kHz 调度、接收器复位和震动回程思路。
- [`Chillsmeit/vader5pro-remap-driver`](https://github.com/Chillsmeit/vader5pro-remap-driver)：Vader 5 Pro 接收器协议、扩展帧、M1-M4 位定义和配置接口线索。
- [`BANANASJIM/flydigi-vader5`](https://github.com/BANANASJIM/flydigi-vader5)：同一设备生态的交叉协议实现和 `padctl` 演进方向。
- [`libsdl-org/SDL`](https://github.com/libsdl-org/SDL)：`SDL_hidapi_flydigi.c` 的飞智路径，以及 `SDL_hidapi_gip.c` 的 GIP 主机命令、Elite 报告和能力位处理。

## GIP、主机和平台研究

- [`wiredopposite/OGX-Mini`](https://github.com/wiredopposite/OGX-Mini) 与 [`MegaCadeDev/OGX-Mini-2026`](https://github.com/MegaCadeDev/OGX-Mini-2026)：RP2040/RP2350 多平台 USB host、Xbox GIP 主机和 Vader 5 Pro 物理兼容性记录。
- [`dlundqvist/xone`](https://github.com/dlundqvist/xone)：GIP 能力位、认证握手和 X.509 设备认证边界。
- [`Manolo15v/wolverine-identity`](https://github.com/Manolo15v/wolverine-identity)：macOS DriverKit 发布兼容 HID 身份的实测记录，帮助区分“设备被采纳”和“元素有值”。
- [`Tinerou/wolverine-v3-pro-macos-wine`](https://github.com/Tinerou/wolverine-v3-pro-macos-wine)：macOS GPTK/Wine、SDL 和 GIP 设备之间的输入链路记录。
- [`EthanArbuckle/iPhone18-3_26.1_23B85_Restore`](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore)：Apple 系统二进制和 DriverKit 结构的还原线索；本项目对关键结论仍以本机二进制复核为准。
- [Microsoft MS-GIPUSB](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/)：GIP USB 公开规范。
- [Apple GameController 文档](https://developer.apple.com/documentation/gamecontroller)与 [Apple iPad 手柄说明](https://support.apple.com/guide/ipad/ipad2746a7e9/ipados)：平台能力和兼容性边界的官方参考。
- [WebKit Gamepad provider](https://github.com/WebKit/WebKit/tree/main/Source/WebCore/platform/gamepad)：macOS HID Gamepad 与 GameController provider 分层的源码参考。

## RP2350 硬件与调度经验

- [`sekigon-gonnoc/Pico-PIO-USB`](https://github.com/sekigon-gonnoc/Pico-PIO-USB)：PIO USB host/device 机制。
- [`hathach/tinyusb`](https://github.com/hathach/tinyusb)：USB HID、CDC 和描述符实现基础。
- [qsantos 的 RP2350-USB-A host 文章](https://qsantos.fr/2025/11/21/fixing-the-rp2350-usb-a-not-working-as-usb-host/)、[断连检测文章](https://qsantos.fr/2026/01/01/the-rp2350-usb-a-cannot-see-devices-disconnect/)和[替代板比较](https://qsantos.fr/2026/01/07/no-alternatives-to-the-rp2350-usb-a/)：R13 上拉、15 kΩ 下拉和板级供电/热插拔风险。
- [Waveshare RP2350-USB-A wiki](https://www.waveshare.com/wiki/RP2350-USB-A)：板级引脚和官方 `Host_hid_to_device_cdc` 架构参考。
- [`THLN33/RP2350-USB-A`](https://github.com/THLN33/RP2350-USB-A) 与 [`ugufru/waveshare-rp2350-usb-a`](https://github.com/ugufru/waveshare-rp2350-usb-a)：同板 USB-A host 到 USB-C device 的最小工程。

## 映射、测试和互操作性经验

- [`Minimuino/thumbstick-deadzones`](https://github.com/Minimuino/thumbstick-deadzones)：scaled radial 摇杆死区模型，本项目用整数运算移植并加入边界测试。
- [`skyne/rp2040-ffb`](https://github.com/skyne/rp2040-ffb)：PID/Force Feedback 描述符实验参考；本项目没有声称实现完整 PID 效果引擎。
- [`GP2040-CE` 兼容性 FAQ](https://gp2040-ce.info/faq/faq-console-compatibility/)：认证、主机兼容和“PC 能枚举不等于主机接受”的边界说明。
- [`OOPMan/XBOFS.win`](https://github.com/OOPMan/XBOFS.win)、[`aaronv02/powera-gip-bridge-macos`](https://github.com/aaronv02/powera-gip-bridge-macos) 和 [`ysSemanticSystems/gipbridge`](https://github.com/ysSemanticSystems/gipbridge)：GIP 命令和跨平台互操作性的交叉验证来源。

## 本项目从这些资料得到的可复用经验

1. **Generic HID 不是平台准入保证。** `0x01/0x05`、非零 VID/PID 和足够的按键/轴只能证明底层 HID 可解析；macOS `GCController` 还取决于系统模型匹配。
2. **背键必须先从接收器扩展帧中拿到。** test mode、扩展帧校验、M1-M4 位定义和输入源时间戳都要分别验证，不能只看浏览器最终按钮数量。
3. **震动必须有停止路径。** 启动包、显式零强度停振、OUT 完成计数和看门狗要一起设计；“浏览器发送成功”不等于马达已经停止。
4. **传感器、电量、配置和原生游戏输入是四条不同路径。** WebHID 能读到数据，不代表 `GCController.battery`、原生 haptics 或每个 App 都会消费这些字段。
5. **iPad 的单次成功不能替代兼容性矩阵。** 供电、USB-C 拓扑、iPadOS 更新、睡眠唤醒、App 后端和长期运行都可能改变结果；本项目目前把不良后果标记为未知。

更多本机证据、命令和原始参考索引见 [`refs/README.md`](refs/README.md)。
