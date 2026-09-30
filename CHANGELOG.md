# Changelog

## v0.1.0 - 2026-09-30

初始公开版本。

- RP2350 USB-A PIO-USB host 读取 Vader 5 Pro 接收器的 XInput/扩展输入。
- macOS Apple 兼容 HID 身份 `04B4:2412:0500`，实测触发 `GCControllerDidConnect`。
- M1-M4、C/Z/LM/RM/O、标准按键、摇杆、扳机和 D-pad 的映射与配置。
- WebHID 配置页：按键映射、轴方向、死区、曲线、传感器、电量和双马达测试。
- 震动启动/停振确认、250 ms 看门狗、传感器 Report ID 1/2 和 CDC 诊断工具。
- 主机 C/Python/网页验证和 Apple HID 描述符离线检查。

已知边界：原生 `GCController.haptics` 和 `GCController.battery` 仍未暴露；iOS/iPadOS、Windows、Linux、Safari 和第三方游戏没有获得本项目的兼容性保证。详见 [`DISCLAIMER.md`](DISCLAIMER.md) 与 [`docs/06-风险与未知项.md`](docs/06-风险与未知项.md)。
