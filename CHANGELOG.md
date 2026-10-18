# Changelog

## Unreleased - 2026-10-18

**官方驱动第二轮命令谱系核对与落地**（详见 [`docs/16-官方命令谱系落地-2026-10.md`](docs/16-官方命令谱系落地-2026-10.md)）：

- 设备信息全量解析（0x01：7 固件版本 + 芯片 + 能力位）+ SKU 名称（0x0F）→ `[DEV+]` 行 / WebHID `0x0B`。
- 配置指纹（0xA1 读 + **变更推送**）+ RAM 元数据（0xA3 子 1）→ `[FP]` 行；配置变更自动重读。
- 新能力：切槽（0xA2 / `k#` / WebHID `0x0D`）、保存（0xA7 / `w`）、全局重置（0xAF / `Z` 双按）、摇杆校准（0x03 子 1/2 / `c`,`C` / WebHID `0x0E`）、jsResolution 等 7 字段写（WebHID `0x0C`）、名称写（0xA0 field 1 / WebHID `0x12`）。
- 配置页新增：设备信息、配置指纹、分辨率、名称、配置槽切换、摇杆校准。
- 真机实验：回报率各档（1000/500/250/125）与 EF 高帧率流无关（~740/s 恒定）；0xAF 全局重置语义（3 字节，可恢复）；A1 推送为变化驱动。
- 平台陷阱：macOS SetReport 剥离与报告 ID 相同的首字节 → 协议命令字禁用 0x10（名称命令改 0x12）。
- 拼接帧嫌疑监测（`[GLTCH]`）；探针新增 `--fingerprint / --set-field / --set-name / --calibrate`。

**官方驱动落地：设备端全局配置读写（0xA3 读 / 0xA0 写）**（详见 [`docs/15-官方驱动落地-全局配置读写.md`](docs/15-官方驱动落地-全局配置读写.md)）：

- 「1K 回报率」获设备自身配置实证（`usbReturnRateHz = 1000`）。
- 睡眠定时器经官方 `0xA0/32` 写入「永不」并回读闭环；旧 `0x17/0x14` 写路径弃用。
- 固件开机自动读取全局配置（`[CFG]` 状态行）；任何读取发现睡眠 ≠ 0 时自愈写回「永不」（每挂载一次，真机行为矩阵已验证）。
- WebHID 新增命令 `0x08 GET_GCFG / 0x09 SET_SLEEP / 0x0A SET_RATE`；配置页新增「接收器全局配置」面板（读写回报率与睡眠）。
- 探针新增 `--gcfg / --set-sleep=N / --set-rate=N`。
- CDC TX 缓冲区 512 → 4096，修复心跳状态尾部丢行（`[A0]/[AGE]/[POWER]/[OPEN]`）。

## v0.1.0 - 2026-09-30

初始公开版本。

- RP2350 USB-A PIO-USB host 读取 Vader 5 Pro 接收器的 XInput/扩展输入。
- macOS Apple 兼容 HID 身份 `04B4:2412:0500`，实测触发 `GCControllerDidConnect`。
- M1-M4、C/Z/LM/RM/O、标准按键、摇杆、扳机和 D-pad 的映射与配置。
- WebHID 配置页：按键映射、轴方向、死区、曲线、传感器、电量和双马达测试。
- 震动启动/停振确认、250 ms 看门狗、传感器 Report ID 1/2 和 CDC 诊断工具。
- 主机 C/Python/网页验证和 Apple HID 描述符离线检查。

已知边界：原生 `GCController.haptics` 和 `GCController.battery` 仍未暴露；iPad 有一条用户成功使用记录，但没有完整兼容性矩阵，Windows、Linux、Safari 和第三方游戏没有获得本项目的兼容性保证。详见 [`DISCLAIMER.md`](DISCLAIMER.md) 与 [`docs/06-风险与未知项.md`](docs/06-风险与未知项.md)。
