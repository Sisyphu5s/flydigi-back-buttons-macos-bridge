# refs — 参考工程与一手证据索引

## A. 本机一手证据（最权威，随时可复读）

| 目标 | 路径 | 用途 |
|---|---|---|
| Xbox 驱动匹配规则 | `/System/Library/DriverExtensions/XboxGamepad.dext/Info.plist` | 我们伪装设备必须满足的 class/subclass/protocol/VID/PID |
| 同上，USB 授权表 | `codesign -d --entitlements - /System/Library/DriverExtensions/XboxGamepad.dext` | 确认只有 4 个微软 PID 被授权 |
| Xbox 驱动实现 | `/System/Library/DriverExtensions/XboxGamepad.dext/XboxGamepad` | 反汇编得到 GIP 主机脚本（M2 第一步） |
| Elite 解码器 | `/System/Library/HIDPlugins/ServicePlugins/XboxGamepadHIDServicePlugin.plugin` | 反汇编得到报告长度门槛、拨片/按钮字节偏移 |
| 拨片 API | `$(xcrun --show-sdk-path)/System/Library/Frameworks/GameController.framework/Headers/GCXboxGamepad.h`、`GCInputNames.h` | `paddleButton1..4`、`GCInputXboxPaddleOne..Four` |

## B. 参考工程（已克隆阅读，非仅看 README）

| 仓库 | 是什么 | 对本项目的价值 |
|---|---|---|
| `Chillsmeit/vader5pro-remap-driver` | Linux userspace 驱动（hidraw/libusb + uinput），把 Vader 5 Pro 2.4G 接收器桥成虚拟手柄，含 `emulate_elite` 模式（VID 0x045E / PID 0x0B00） | **接收器协议一手文档**（`docs/protocol.md`）+ **M1–M4→Elite 拨片语义**（elite.toml 备注 M2/M3 互换对齐物理布局）+ udev 里的 ID 线索。**代码不可移植**（macOS 无 uinput） |
| `BANANASJIM/flydigi-vader5` | 同一项目的 fork/上游，README 指明已演进为 `padctl`（多设备 HID 守护进程） | 交叉核对协议；提示生态里已有"通用手柄守护进程"思路 |
| **`ruomox/Flydigi5Pico`** | **RP2350 小板桥：PIO-USB Host 读飞智接收器 + TinyUSB Device 伪装 Xbox 360 有线手柄（0x045E/0x028E），双核 seqlock，1kHz，~2ms，含震动回程与物理总线复位** | **本项目最接近的先例**：骨架、时序、坑（第三方接收器需硬复位）、板子选型（微雪 RP2350-USB-A）都能直接借用。**唯一缺 = 拨片** |
| `Manolo15v/wolverine-identity` | macOS DriverKit dext，对 Razer Wolverine 讲 GIP 后在系统内发布"苹果同款" HID 身份；含 `plans/PLAN_STANDARD_IDENTITY.md`、`traces/README.md` | **GIP 一手实测笔记**：主机 init 序列、报告形状（95 11/95 12）、描述符逐字节、真机心跳节奏、"被采纳但元素全 0"这类坑。软件路线，但知识完全适用于我们的设备端 |
| `libsdl-org/SDL` → `src/joystick/hidapi/SDL_hidapi_gip.c` | SDL 的 GIP 主机实现（完整） | 命令表、flags、Elite 拨片机制（`0x4d {7,0}` 才能拿 raw report）、按固件版本分的 4 种拨片格式 |
| `libsdl-org/SDL` → `SDL_hidapi_flydigi.c` | SDL 的飞智后端 | 接收器自定义 HID 协议、`0x5A` 魔数、扩展按键（M1 等）位定义、接口号规则 |
| ipsw 还原仓库（如 `EthanArbuckle/iPhone18-3_26.1_23B85_Restore`） | 苹果系统二进制的反编译/符号还原（`XboxGamepad.dext/XboxGamepad.mm`、`XboxEliteV2GamepadHIDServicePlugin.mm` 等） | 快速看苹果实现思路的捷径；**精度不足，关键结论仍需在本机二进制上复核** |
| `dlundqvist/xone`（Linux GIP 驱动）、`aaronv02/powera-gip-bridge-macos`、`ysSemanticSystems/gipbridge` | 其他 GIP 主机实现（Linux / macOS 用户态） | 交叉验证 GIP 行为；说明"macOS 上自家驱动不认的 Xbox 类手柄"是共性痛点 |
| [MS-GIPUSB] `learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/...` | 微软公开的 GIP USB 规范（1.0，2024-09） | 协议权威源（枚举能力、设备类型/子类型、gamepad/语音数据） |

## C. 2026-09-28 追加（围绕 Waveshare RP2350 与社区固件）

| 资源 | URL | 用途 |
|---|---|---|
| **OGX-Mini** | https://github.com/wiredopposite/OGX-Mini | RP2040/RP2350 多平台手柄模拟固件；**内含 Xbox One/Series/Elite 的 GIP host 驱动**（`USBHost/HostDriver/XInput/{XboxOne.cpp,tuh_xinput/*}`）与 `Descriptors/XboxOne.h`。没有 GIP 输出模式 → 我们抄它的"主机期望"，不抄它的输出 |
| GP2040-CE 控制台兼容 FAQ | https://gp2040-ce.info/faq/faq-console-compatibility/ | 证据：Xbox One/PS4/PS5 的**认证由控制台强制**（PC 上 PS4 模式不会 8 分钟超时）→ 苹果侧大概率不查 MS 认证（仍待本机反汇编确认） |
| qsantos ①修复 host | https://qsantos.fr/2025/11/21/fixing-the-rp2350-usb-a-not-working-as-usb-host/ | RP2350-USB-A 的 **R13 1.5k 上拉**问题与拆解方案；最小复现代码 |
| qsantos ②断连检测 | https://qsantos.fr/2026/01/01/the-rp2350-usb-a-cannot-see-devices-disconnect/ | 缺 15k 下拉 → 断连检测失效；加 15k 到 GND 的修法 |
| qsantos ③替代板横向 | https://qsantos.fr/2026/01/07/no-alternatives-to-the-rp2350-usb-a/ | 两端口真 USB 的替代板比较（Feather RP2040 USB-A Host、RP2040-PiZero、nanoCH32V203/305） |
| Waveshare RP2350-USB-A wiki | https://www.waveshare.com/wiki/RP2350-USB-A | 板级参数、官方 demo（**`Host_hid_to_device_cdc` = 同构架构**） |
| THLN33/RP2350-USB-A | https://github.com/THLN33/RP2350-USB-A | 同板：USB-A(HID host) → USB-C(UART/device) 最小范例 |
| ugufru/waveshare-rp2350-usb-a | https://github.com/ugufru/waveshare-rp2350-usb-a | 同板：USB-A(MIDI host) → USB-C(标准 MIDI 设备) |

### 2026-09-28 追加二（OGX-Mini-2026 / GIP 认证 / Wine-GPTK 层）

| 资源 | URL | 用途 |
|---|---|---|
| **OGX-Mini-2026**（`wiredopposite/OGX-Mini` 的活跃 fork） | https://github.com/MegaCadeDev/OGX-Mini-2026 | **RP2350-USB-A 是其维护者支持板**；PIO USB host 修复；**明确支持飞智 Vader 5 Pro（有线 + 2.4G 接收器，物理确认）**；GIP **主机**实现（`USBHost/HostDriver/XInput/*`）；`libxsm3` 真机 360 认证（代价：不再输出到 PC）；`Flydigi_APEX4_Wukong.md` / `GameSir_Cyclone2.md` / `Wired_Controllers.md`（按驱动类型整理的 VID/PID 表） |
| **Tinerou/wolverine-v3-pro-macos-wine** | https://github.com/Tinerou/wolverine-v3-pro-macos-wine | **GIP 设备在 macOS 上让 Wine 认的实证**：重编 SDL（libusb + GIP 握手补丁）→ 塞进 GPTK Wine bundle；`NOTES.md` 含协议/包长/握手顺序/keep-alive 位/幻影设备处理；`src/xinput_test.c` 可作为 Wine 层验收工具 |
| **`xone/auth/auth.c`**（dlundqvist/xone） | https://github.com/dlundqvist/xone | GIP 安全握手权威实现：v1 RSA（HOST_HELLO 0x01 / CLIENT_CERTIFICATE 0x03 / HOST_SECRET 0x05 / HOST_FINISH 0x07）、v2 ECDH（0x21/0x23/0x25/0x26）；**设备需出示微软 X.509 证书**；证书结构不符合 RFC 5280 的坑 |
| SDL `SDL_hidapi_gip.c`（能力位图） | https://github.com/libsdl-org/SDL | `GIP_FEATURE_*` 位定义（含 **`SECURITY_OPT_OUT (1<<4)`**、`ELITE_BUTTONS (1<<2)`）与"声明了就不发 0x06"的主机侧逻辑；安全版本必须是 1.0 |
| `OOPMan/XBOFS.win` | https://github.com/OOPMan/XBOFS.win | OGX-Mini 的 GIP 初始化参考来源（街机杆路径） |
| `santroller/santroller`、`sanjay900/portal_of_flipper`、`oct0xor/xbox_security_method_3` | GitHub | 设备端"安全握手可被固件实现"的完整先例（360 XSM3 路线，走 USB 控制请求而非 GIP 包） |

本次新增下载的本地副本：`~/.hermes/cache/scratch/refs2/ogxmini2026/`（CHANGELOG / IMPROVEMENTS / Wired_Controllers / Flydigi_APEX4 / Planned_Additions / libxsm3 README）、`refs2/gptk/`（Tinerou 的 README 与 NOTES.md、CrossOver 与 Apple GPTK 页面正文）、`refs2/xone_auth.c`。

本次下载的本地副本：`~/.hermes/cache/scratch/refs2/`（qsantos 三篇 txt + raw html、OGX-Mini README、OGX-Mini 的 GIP 源码 4 个文件）。

## D. 复读用命令

```bash
# 苹果匹配表
plutil -convert xml1 -o - /System/Library/DriverExtensions/XboxGamepad.dext/Info.plist
# 拨片 API 证据
grep -n -A2 -B4 paddleButton $(xcrun --show-sdk-path)/System/Library/Frameworks/GameController.framework/Headers/GCXboxGamepad.h
# 参考工程（供离线阅读）
git clone --recursive https://github.com/ruomox/Flydigi5Pico
git clone https://github.com/Chillsmeit/vader5pro-remap-driver
git clone https://github.com/BANANASJIM/flydigi-vader5
```

> 本次调研的克隆副本暂存在 `~/.hermes/cache/scratch/flydigi-research/`（缓存目录，24h 后可能被清理；需要长期保留请按上面命令重新克隆到本工作区的 `vendor/` 下）。
