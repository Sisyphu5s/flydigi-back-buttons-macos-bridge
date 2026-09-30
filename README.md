# flydigi-elite-bridge — 飞智 Vader 5 Pro → macOS 背键桥接

这是一个面向 **macOS** 的实验性 USB 协议桥：Waveshare RP2350 通过 USB-A/PIO-USB 读取飞智 Vader 5 Pro 接收器，再通过 USB-C 暴露一个可被 macOS `GameController` 接受的 HID 手柄，同时保留 M1–M4、传感器、配置和震动通道。

> **公开发布说明**：本项目适合熟悉 RP2350、USB HID 和 macOS 调试工具的开发者。它不是飞智、Apple、Microsoft 或 Waveshare 的官方软件，也不保证在其他 macOS 版本、iOS/iPadOS、Windows、Linux、浏览器或游戏中具有相同表现。使用前请阅读 [`DISCLAIMER.md`](DISCLAIMER.md) 和 [`NOTICE.md`](NOTICE.md)。

工作区：本仓库根目录
目标硬件：**Waveshare RP2350（USB-A 走 PIO 做 host + USB-C 做 device）**
目标平台：**仅 macOS**（2026-09-28 修订：iPhone/iPad 暂缓）
状态：**已完成一次可复现的 macOS 真机验证**（Apple 兼容混合身份已触发 `GCControllerDidConnect`；WebHID 电量、传感器和震动路径已验证；实体按键与摇杆仍建议按目标硬件逐项回归）
最后更新：2026-09-30

## 快速开始

### 需要的硬件

- Waveshare RP2350-USB-A 或兼容 RP2350 双 USB 方案
- 飞智 Vader 5 Pro 2.4G 接收器和已配对的手柄
- 一台 macOS 主机、一条可传数据的 USB-C 线

### 构建最终 macOS 固件

本仓库不携带 Pico SDK、PIO-USB 或 ARM 工具链。请按本机环境设置 `PICO_SDK_PATH`，并安装 Ninja、CMake、Python 3 和 ARM GNU 工具链。

```sh
cmake -B firmware/rp2350/build-apple-compatible \
  -S firmware/rp2350 -G Ninja \
  -DAPPLE_COMPATIBLE_PROFILE=ON -DCMAKE_BUILD_TYPE=Release
ninja -C firmware/rp2350/build-apple-compatible
python3 firmware/rp2350/test/usb_desc_check.py --generic-apple \
  firmware/rp2350/build-apple-compatible/flydigi_bridge.elf
```

### 刷写

先保存现有 Flash 镜像，再让 RP2350 进入 BOOTSEL。刷写会替换整片程序存储；不要在未确认备份可用时执行。

```sh
picotool save -a -v /tmp/flydigi-before-update.uf2
picotool load -v -x \
  firmware/rp2350/build-apple-compatible/flydigi_bridge.uf2
```

也可以连接 CDC 控制台后发送单个 `b` 字符进入 BOOTSEL。刷写后可运行：

```sh
firmware/probe/apple_controller_status.sh
swift firmware/probe/gc_watch.swift 4 04b4 2412
swift firmware/probe/hid_config_probe.swift --apple --status --sensor --rumble-timing
```

### WebHID 配置页

桌面 Chrome/Edge 支持本项目的 WebHID 配置页；Safari 不提供 WebHID。启动静态服务器：

```sh
python3 -m http.server 8765 --directory web
```

打开 `http://localhost:8765/bridge-config.html`，授权设备后可查看电量、传感器、输入状态、摇杆死区/曲线、轴方向、按键映射和双马达测试。`gamepad-probe.html` 用于对比标准 Gamepad API 与 WebHID 路径。

### 验证主机代码

```sh
cd firmware
./verify.sh
node ../web/bridge-config.test.js
node ../web/gamepad-probe.test.js
```

完整实验记录和未覆盖项见 [`docs/12-固件实现与验证.md`](docs/12-固件实现与验证.md) 与 [`docs/06-风险与未知项.md`](docs/06-风险与未知项.md)。

---

## 1. 一句话目标

用一个 USB 小板做协议桥：
- **输入侧**（USB Host）：读取飞智 Vader 5 Pro 的 2.4G 接收器（或手柄有线直连）的专有数据流，拿出 **M1–M4 背键**与标准手柄量；
- **输出侧**（USB Device）：把这路输入重新封装成 **macOS 原生识别的 HID 手柄**，让 M1–M4 成为 **4 个独立输入**。

输出身份的首选**不再是** Xbox Elite（GIP）路线，而是**冒充苹果设备模型库里已登记的型号**——首选 `Flydigi Vader 2 Pro` 的 USB HID 模式（VID `0x04B4` / PID `0x2412` / bcdDevice `0x0500`）。理由见 §2 结论 12 与 docs/11：**macOS 自带一张"带背键型号"的模型表，套用后系统里直接出现标识符为 `BUTTON_M1..M4` 的 4 个独立元素，不需要 GIP、不需要认证、不需要逆向苹果的 GIP 主机。**

---

## 2. 关键结论（先看这 13 条）

1. **用户的前提成立**：飞智接收器在 macOS 上"枚举成功但无输入"。苹果只匹配白名单 VID/PID + 接口类，不匹配就整个忽略，且 macOS 没有通用 XInput 回退路径。
2. **苹果的匹配规则已经在本机拿到一手证据**（不是猜的）：`/System/Library/DriverExtensions/XboxGamepad.dext/Info.plist` 里 4 组 personality，精确到 `bDeviceClass` / `bInterfaceClass` / `bInterfaceSubClass` / `bInterfaceProtocol` / VID / PID。→ docs/01
3. **拨片是"有 API、有绑定，但 macOS 有线链路上没有传输者"**（一手证据 docs/10 §6.5）：`GCXboxGamepad.paddleButton1..4` 的 ivar 被绑到元素标识符 **`XBOX_BUTTON_PADDLE_1..4`**；GameControllerFoundation 另有一张 **158 条"元素标识符注册表"**，内含 `XBOX_BUTTON_PADDLE_1..4`、`BUTTON_M1..M4`、`BUTTON_L4/L5/R4/R5`、`LEFT/RIGHT_PADDLE`。**但**苹果 dext/插件里 `paddle` 命中 0，有线 GIP 的 dispatch 状态也没有拨片字段 → **以 USB-GIP 冒充 Elite 拿不到拨片值**。⇒ 关键问题变成"设备侧能否提供框架认的元素标识符"——**结论 12 给出了实现方式，且不必走 GIP。**
4. **"伪装成 Xbox"走的是 GIP 协议，不是普通 HID**：One S / Series X / Elite V2 在 USB 上走微软 GIP；苹果的 dext 自己当 GIP 主机，并在系统内发布一张"GIP 直通"HID 描述符（Report ID = GIP 命令字节）。→ docs/03
5. **读飞智这一侧已被第三方固件物理确认**：OGX-Mini-2026 的 `Wired_Controllers.md` 明确写 **Vader 5 Pro 走有线 USB 与 2.4GHz 接收器都可用（physically confirmed）**，走现成 XInput/DInput host 路径、无需专用驱动。→ docs/09 第一部分
6. **骨架也可以直接对读同一块板**：`ruomox/Flydigi5Pico`（RP2350-USB-A + PIO-USB host + TinyUSB device，~2ms）是这条路的成品；OGX-Mini-2026 还把 **RP2350-USB-A 列为维护者支持板**。→ docs/07、docs/09
7. **社区没有任何 GIP 设备端成品**（OGX-Mini-2026 输出模式里没有 Xbox One/Series，路线图注明"需要认证 dongle"）→ 这也是新路线更有吸引力的原因之一。
8. **GIP 认证问题有明确突破口**（若走备选路线才需要）：设备可在能力位图里声明 **`GIP_FEATURE_SECURITY_OPT_OUT (1<<4)`**；真认证要求微软签发的证书 + 私钥（`xone/auth/auth.c`：v1 RSA / v2 ECDH）。→ docs/09 §3
9. **"17 还是 18 字节"的矛盾已解决**：线上 GIP 输入包 18 字节（4B 头 + 14B 负载），Apple 描述符的 `95 11`(17) 是去掉 Report ID 后的 body。→ docs/08 §3
10. **板子本身有两个硬件坑**（RP2350-USB-A 特有）：USB-A 口 D+ 有 1.5k 上拉(R13)，按 device 设计 → 先例用"12mA 强拉低 200ms"软绕过；且缺 15k 下拉 → 断连检测无效。→ docs/07
11. **如果目标是"在 Mac 上玩游戏"，还要过 GPTK/Wine 这一层**：Wine 侧靠 **SDL → winebus(SDL bus) → winexinput → xinput1_3**。→ docs/09 第二部分
12. **★ 苹果有"设备模型库"，原生支持背键型号（本轮最大发现）**：`/System/Library/AssetsV2/com_apple_MobileAsset_GameController_DB1/<hash>.asset/AssetData/` 下是两张配置包——`GameControllers-Custom.bundle`（苹果手写模型，14 厂商 / 31 型号）与 `GameControllers-SDL.bundle`（SDL 映射库派生，148 条）。Custom 包的 `Info.plist` 有一张 **`Devices` 匹配表**，键就是 USB 描述符的 **`VendorID` / `ProductID` / `VersionNumber`(=bcdDevice)**，值是一组**按优先级排列的 Personality 路径**。其中：
    `com.Flydigi.Vader2Pro.MobileUSB` = VID **1204 (0x04B4)** / PID **9234 (0x2412)** / Ver **1280 (0x0500)** → `[…/MobileUSBWithBackButtons.plist, …/MobileUSB.plist]`；
    模型里 **`button.m1..m4` = `UsageType == 1 AND UsageTypeIndex == 14/15/16/17`**，并由 `LocalizedNameKey = BUTTON_M1..M4` 落到 GC 元素上。⇒ **只要我们把这个 VID/PID/bcdDevice 和对应按钮布局做出来，macOS 就会原生暴露 4 个独立背键元素。** → docs/11
13. **★ 输入侧背键位与板子固件缺陷（真机实测，2026-09-29）**：扩展输入帧（接口 1 / EP2，32 B，魔数 `5a a5 ef`）**必须先由主机发命令开 test mode** 才会出现；**背键 M1–M4 = `byte[13]` 的 bit2/3/4/5**（`bit0/1=C/Z`、`bit6/7=LM/RM`），已用真机帧逐字段交叉验证（加速度 Z ≈ 1.10 g ⇒ 布局自洽）。板子上刷的第三方 `RP2350 Pad Bridge (stage 2)` 固件 **`tx=0`（从枚举至今写入 0 次）**，既没做初始化握手、也没开 test mode，所以只能收到固定状态帧、**背键永远出不来**——这是固件输入路径未完成，不是硬件问题。→ docs/02 §7

---

## 3. 已定决策（2026-09-28）

| # | 项 | 结论 | 对计划的直接影响 |
|---|---|---|---|
| D1 | 目标设备 | **仅 macOS**（修订：iPhone/iPad 暂缓） | 验证手段齐全（ioreg / hidutil / GC API / 本机反汇编）；iOS 证据存档 docs/01 §6 |
| D2 | 拨片 | **必须：M1–M4 做成 4 个独立输入**（需求不变） | 交付标准 = "设备侧提供框架认的元素标识符"。载体已确定可为 **`BUTTON_M1..M4`**（走 HID 模型路线），`XBOX_BUTTON_PADDLE_1..4` 仅剩备选意义（docs/10 §7、docs/11 §4） |
| D3 | 硬件 | **Waveshare RP2350**（USB-A=PIO host、USB-C=device） | 板级要点见 docs/07；精确型号待丝印确认 |
| D4 | 参考条件 | 无真 Elite 2、无 Windows 抓包机；但有**四类一手材料**：本机 Apple dext/插件/共享缓存反汇编、OGX-Mini(-2026) 的 GIP 主机实现、SDL/xone、Tinerou 的 Wine 侧实测记录 | 新路线把"必须反汇编苹果 GIP 主机"降为可选 |
| D5 | 折中退路 | 复合设备：手柄 + HID 键盘接口（M1–M4 发按键码） | **在 GPTK/Wine 场景这条退路其实是主路**（结论 11）；原生化场景走模型路线 |
| D6 | **输出身份（新）** | **首选冒充苹果模型库登记型号：Flydigi Vader 2 Pro USB（VID 0x04B4 / PID 0x2412 / bcdDevice 0x0500）**；Xbox Elite + GIP 降为**备选**（真机已证实其在本机链路上 `paddleButton1..4` 全 nil） | 输出侧从"实现 GIP 设备 + 规避认证"变为"**实现普通 HID 手柄 + 摆对按钮位**" |

## 3.1 执行顺序（被 D2 + D6 修正）

**Phase 0 实测（接上手柄看系统行为）→ M0 板子热身（官方 `Host_hid_to_device_cdc`）→ 输入侧解飞智报文（M1–M4 字段）→ 输出侧按 `Flydigi Vader2Pro/MobileUSB` 模型做 HID 设备（20 按钮位 + 轴/dpad，M1–M4 落 14..17）→ 三层验收（HID 层 / GC 元素层 / 游戏层）。**

- **不再**要求"先验通 GIP"。GIP/Elite 路线只在 HID 模型路线被证伪时才启动——**真机已验：Elite 路线在本机拿不到背键（`paddleButton1..4` 全 nil）**。
- **输入侧的 M1–M4 字段已经解决**（结论 13：`byte[13]` bit2..5），剩下的是"板子固件要真的去开 test mode 并读接口 1"。
- 虚拟 HID 定标实验（`IOHIDUserDevice`）**已证不可行**（需苹果签发的 `com.apple.developer.hid.virtual.device` entitlement）→ 定标只能用真实板子，见 docs/11 §5。

---

## 4. 目录

```
README.md                                    本文件：结论速览 + 决策项
docs/01-苹果侧-匹配规则与能力.md               Apple 匹配表（一手）、拨片 API、iOS 侧证据（暂缓）
docs/02-输入侧-飞智接收器协议.md               接收器 VID/PID/接口/20B XInput/扩展按键/震动 + §7 真机实测（背键位、固件根因）
docs/03-输出侧-GIP协议与Elite身份.md           GIP 命令表、能力位图、主机 init、Elite 拨片机制
docs/04-方案对比与推荐路线.md                  路线对比 + 推荐阶梯 + 硬件选型 + 时延预算
docs/05-实施步骤与里程碑.md                    Phase 0–4 + M0–M4 验收判据 + 三层验收 + 安全回滚
docs/06-风险与未知项.md                        R1–R15 风险表：每条含"如何验证"和"失败退路"
docs/07-硬件-Waveshare-RP2350-USB-A要点.md     板子事实、R13 上拉坑、断连坑、替代板、供电
docs/08-社区固件参考与GIP可抄清单.md           社区工程矩阵 + OGX-Mini 的 GIP 逐字节事实
docs/09-OGX-Mini-2026与GPTK-Wine链路.md        OGX-Mini-2026 事实 + GIP 安全突破 + GPTK/Wine 层
docs/10-逆向结果-苹果GIP主机一手证据.md         GCXboxGamepad 拨片绑定链、158 条标识符注册表
docs/11-苹果设备模型库-M键原生路径.md   ★ 设备模型库、匹配表、模型格式、路线改写 + §8 真机验证结果
firmware/                                     输出侧 + 输入侧可编译实现 + 写固件前的七步主机验证
  verify.sh                                     一键验证（描述符/模型一致性/位级往返/C↔Python 比对/输入侧解析与映射）
  report-verification.md                        验证报告（结论、报文位图、未覆盖项、工具链缺口）
  hid_desc_vader2pro.py / hid_parse.py / model_conform.py / dryrun_report.py
  src/vader2pro_hid.h / src/report_pack.c / test/report_pack_test.c / desc_vader2pro.h
  src/flydigi_rx.{h,c}                          输入侧：扩展帧解析 + 6 条命令（握手/test mode）★ 新
  src/bridge_map.{h,c}                          映射层：接收器状态 → Vader2Pro 10 B 报文 ★ 新
  test/flydigi_test.c                           输入侧 + 映射的纯主机验证（50 项，含真机帧）★ 新
  probe/                                        真机工具：gcwatch（GC 实时观察）、rx_inspect / rx_poke（接收器直连 Mac）、capture_mkeys.py
refs/README.md                                参考工程与一手证据清单（本机路径 + 提取命令）
```

## 5. 术语

- **接收器/dongle**：飞智 2.4G USB 接收器，VID `0x37d7` PID `0x2401`
- **XInput**：微软 Xbox 360 时代的手柄数据格式（20 字节报告），不是 USB 标准
- **GIP**：Gaming Input Protocol，Xbox One/Series/Elite 的协议（公开规范 MS-GIPUSB）
- **能力位图（features bitmap）**：设备自报能力的位图，`ELITE_BUTTONS(1<<2)` / `SECURITY_OPT_OUT(1<<4)` 等
- **dext**：DriverKit 驱动扩展；macOS 用它认 Xbox 手柄（`XboxGamepad.dext`）
- **PIO-USB**：用 RP2040/RP2350 的 PIO 软件模拟一条 USB 主机总线
- **GC framework**：Apple 的 GameController 框架，原生游戏/Steam/浏览器在 macOS 上都走它
- **GPTK / Wine 层**：Game Porting Toolkit 的 Wine 运行时；Windows 游戏里的手柄由 bundle 内的 **SDL** 提供，经 `winebus → winexinput → xinput1_3` 到达游戏
- **拨片/paddle**：Elite 背面的 4 个可拆拨片，GC 里是 `paddleButton1..4`，映射到飞智 M1–M4
- **设备模型库**：`com_apple_MobileAsset_GameController_DB1` 资产里的配置包；`Info.plist → Devices[]` 用 USB VID/PID/bcdDevice 匹配，值是按优先级的模型（Personality）路径
- **Personality / 模型**：一个 `Model` 字典，含 `Driver.Elements`（HID 元素谓词声明）与 `PhysicalInput.Elements`（GC 元素，带 `LocalizedNameKey`）
- **IOPropertyMatch**：匹配表的匹配块，键 = IORegistry 属性（`VendorID`/`ProductID`/`VersionNumber`）
- **test mode**：飞智接收器接口 1 的一种状态，**必须由主机发命令开启**，开启后才会持续输出含背键与 IMU 的 `5a a5 ef` 扩展输入帧
# WebHID 配置

固件的接口 2 暴露厂商定义 HID Feature Reports，配置页位于
`web/bridge-config.html`。在本机启动静态服务器后打开页面即可调节摇杆死区/曲线、轴反向、20 个实体键的映射、陀螺仪偏置和双马达参数，查看实时电量与传感器原始值：

```sh
cd web
python3 -m http.server 8765
```

然后访问 <http://localhost:8765/bridge-config.html>，点击“连接设备”；陀螺仪和加速度计需另点“连接传感器”。<http://localhost:8765/gamepad-probe.html> 可分别测试标准 Gamepad API 与 WebHID 震动。WebHID 协议使用报告 ID `0x10`（命令 Feature）、`0x11`（响应 Feature）和 `0x12`（状态 Input）；保存命令会在主循环中通过双核安全 Flash API 写入最后一个扇区。

## macOS 系统控制器构建目标

不要只用 `-DGENERIC_GAMEPAD=ON` 刷写最终固件：`1209:0001` 的纯 Generic 身份可被 IOHID 和网页读取，但在本机不会创建 `GCController`，所以“系统设置 → 游戏控制器”不会列出它。已经验证可用的构建目标是混合身份：保留 26 键 Generic 报告和 WebHID，同时使用 Apple 已匹配的 Vader2Pro 三元组 `04B4:2412:0500`。

```sh
cmake -B rp2350/build-apple-compatible -S rp2350 -G Ninja \
  -DAPPLE_COMPATIBLE_PROFILE=ON -DCMAKE_BUILD_TYPE=Release
ninja -C rp2350/build-apple-compatible
python3 rp2350/test/usb_desc_check.py --generic-apple \
  rp2350/build-apple-compatible/flydigi_bridge.elf
```

`APPLE_COMPATIBLE_PROFILE` 会同时启用 `GENERIC_GAMEPAD` 和 `GENERIC_APPLE_IDENTITY`，并拒绝与 PID、Stadia、SCUF 实验身份混用。当前板上实时检查结果为“Vader 2 Pro，Connected”，`GCControllerDidConnect` 已收到，`GCExtendedGamepad` 有 38 个元素；这证明 macOS 原生输入路径已经实例化。标准网页震动和原生电量对象仍需单独验证，不能由连接通知推断。

### 独立 HID 电量探针

`APPLE_BATTERY_INTERFACE_PROBE=ON` 是隔离的实验构建：它保留游戏、传感器和 WebHID 接口不变，额外增加一个符合 USB-IF Power Device 结构的 HID 接口（Battery System/Battery/Absolute State Of Charge，Report ID 6）。实测 macOS IOHID 能枚举该电量元素并收到报告，游戏报告仍约 249 Hz；但 `GCController.battery` 仍为 `nil`，所以该接口不能宣称已经打开 Apple 原生电量对象。当前板上不使用此实验身份，稳定固件的电量显示仍通过 WebHID 配置页。

### macOS 设置页没有显示时先做只读诊断

不要先改 VID/PID 或重刷 Generic 描述符。运行：

```sh
firmware/probe/apple_controller_status.sh
```

该命令分别检查 `AppleGCHIDUserEventDriver` 是否已匹配、`GameControllerSupport` 是否为 `Yes`，以及 `com.apple.GameController` 是否已经保存 `Vader 2 Pro` 记录。若驱动和记录都存在，说明设备已被 `gamecontrollerd` 接受，设置页通常只是锁屏或旧窗口缓存；解锁 Mac 后关闭并重新打开系统设置，再检查“游戏控制器”。该诊断不会写入偏好设置，也不会刷写固件。

### 对 Generic HID 说法的边界

USB HID 规范允许自定义按键、传感器和输出报告，但“符合 `Generic Desktop/Game Pad (0x01/0x05)`”不等于 Apple 一定创建 `GCController`。本项目在同一台 Mac 上做过 A/B：有效 VID/PID、26 个按键、6 个轴的纯 Generic `1209:0001` 能被 IOHID、WebHID 和浏览器读取，但 `GCController.controllers()` 仍为 0；同一报告改用已经匹配的 `04B4:2412:0500` 身份后才收到 `GCControllerDidConnect`。因此按键数量、制造商字符串和非零 VID/PID 都不是单独的 Apple GameController 准入保证。

同样，macOS 的 `GCController` 记录不能推出 iOS/iPadOS 对自制 USB HID 的支持；当前工程没有 iPad 真机结论。原生 GameController 的 haptics 和 battery 也不是 HID 报告存在就自动出现：当前已注册的 Vader2Pro 记录明确为 `supportsHaptics = 0`，`GCController.battery` 为 `nil`，所以这两项继续走 WebHID/接收器协议路径。

旧版 `APPLE_BATTERY_PROBE=ON` 会在游戏手柄接口里附加 Battery Strength Report ID 3：

```sh
cmake -B rp2350/build-apple-battery -S rp2350 -G Ninja \
  -DAPPLE_COMPATIBLE_PROFILE=ON -DAPPLE_BATTERY_PROBE=ON -DCMAKE_BUILD_TYPE=Release
ninja -C rp2350/build-apple-battery
python3 rp2350/test/usb_desc_check.py --generic-apple --apple-battery \
  rp2350/build-apple-battery/flydigi_bridge.elf
```

该探针已实机验证为不可用：Report ID 3 会让 macOS 游戏报告回调只收到电量报告，游戏输入丢失。它只保留作反例；当前板不使用该构建。独立接口实验见上节。

## Generic Game Pad A/B 结论（2026-09-30）

`-DGENERIC_GAMEPAD=ON` 构建使用标准 Game Pad Usage `0x01/0x05`、VID/PID `1209:0001`，并暴露 26 个按钮、6 轴、传感器和 WebHID 配置接口。按钮 0–16 按浏览器常见顺序安排，LT/RT 与 D-pad 同时保留模拟轴/hat 和数字按钮；M1–M4、C/Z/LM/RM/O 各占独立槽。初版 25 按钮 A/B 已确认 IOHID 能枚举，但不会触发 macOS `GCControllerDidConnect`；当前 26 按钮构建也仍未出现在 `GCController.controllers()`。Vader2Pro 身份已实测可触发 GameController，并暴露 38 个元素，仍是 macOS 原生游戏的兼容构建。

HID Game Pad 描述符只证明底层输入可被解析，不保证 macOS Game Controller、浏览器标准震动、电量对象或 iOS/iPadOS 原生游戏支持。Apple 的[控制器发现文档](https://developer.apple.com/documentation/gamecontroller/discovering-game-controllers)和[iPhone 有线连接说明](https://support.apple.com/guide/iphone/connect-a-game-controller-iph9d38dd45f/27/ios/27)都以受支持或兼容的手柄为前提。`CHROMIUM_STADIA_PROBE` 是独立的双马达网页震动实验构建；Mac 锁屏期间新 USB 身份没有完成注册，原因尚未确定，浏览器与实体马达效果也未验证。

WebHID Feature/Status 报告是 63 字节，因此 TinyUSB `CFG_TUD_HID_EP_BUFSIZE` 必须为 64；`firmware/probe/hid_config_probe.swift` 可在不依赖网页的情况下验证配置、传感器、震动及 Flash 保存。`swift firmware/probe/hid_config_probe.swift --save` 会保存当前配置一次，并核对请求编号与异步成功状态；配置页也使用该编号避免旧状态或快速完成的写入误报。

要验证 Flash 真的保存了修改后的值，而不是只验证默认配置，可运行：

```sh
swift firmware/probe/hid_config_persist_probe.swift --apple
```

该探针会临时修改左死区和径向死区 flag，发送保存命令和 CDC `n` 重启，重新枚举 WebHID 接口后比较配置；通过后会恢复原配置、再次保存并重启。它不会刷写固件。

配置命令 payload `[61..62]` 是 16 位请求标记，固件在响应中回显；`GET_INFO` 的 `[13]` 表示支持此校验。配置页在新固件上拒绝其他标签页覆盖的响应，并在断线重连时取消旧连接的待处理命令；旧固件仍可读取但没有跨标签页响应校验。`node web/bridge-config.test.js` 覆盖响应错配和断线重连。

左右摇杆可以分别开启径向死区（配置 `flags` 位 1/2）；默认保持原逐轴死区，旧 Flash 配置不用迁移。径向模式按向量长度判定死区、在剩余行程上应用现有曲线，并保留 HID 方形边界的满幅斜向端点。`firmware/verify.sh` 覆盖两种模式的中心、斜向、端点和反向；Generic 真机的临时配置读写已验证 `flags=0x0001 → 0x0003 → 0x0001`，实体摇杆手感仍需操作回归。算法依据 [Thumbstick Deadzones 的 scaled radial 模型](https://github.com/Minimuino/thumbstick-deadzones/blob/master/main.py)，固件用整数运算实现。

Chrome 154/macOS 真机在当前 Generic 身份下给出 `mapping=""`、26 个按钮、10 个轴、`vibrationActuator=false`。因此 [GamepadTester.cn](https://www.gamepadtester.cn/) 所用的标准 Gamepad API 能读取输入，但其标准震动按钮无法驱动这台 Generic 设备；本项目的 WebHID 震动按钮走独立的配置接口。RP2350 端已验证一次非零启动包和约 250 ms 后零强度停振包均发往接收器，发送失败计数为 0。陀螺仪/加速度计的 Report ID 1/2 已在 macOS IOHID 层读到非零数据；浏览器 WebHID 传感器界面仍待人工授权后的页面回归。

配置页的测试强度可分别设置左右马达，并提供显式停振；`node web/bridge-config.test.js` 验证两路强度与双零命令的字节顺序。实体手感及浏览器端 WebHID 读写需要页面授权后回归。

停振按钮在读取/保存等配置操作等待响应时仍可用，会直接发送零强度 Feature 报告；这条紧急停振路径不等待先前命令，也不把“已发送”当作马达已停止的确认。若同时有配置事务，旧事务的响应可能被停振响应覆盖，页面会按请求标记报告结果未知。固件自身的震动看门狗仍负责超时停振。

`0x01/0x05` 是标准 HID Game Pad 用法，并不保证 Apple Game Controller 框架实例化该设备。Apple 的[控制器发现文档](https://developer.apple.com/documentation/gamecontroller/discovering-game-controllers)说明受支持控制器可通过 USB 连接 iPhone/iPad，但没有把任意自制 HID Game Pad 列为支持对象。当前纯 Generic 构建在本机 IOHID/Chrome 可见，`GCController.supportsHIDDevice(_:)` 对 Game Pad 接口返回 `true`，但 `GCController.controllers()` 仍为 0；同一报告改用已登记的 Vader2Pro 三元组后收到 `GCControllerDidConnect`。这表明 VID/PID/版本的模型匹配是当前 macOS 实测中的决定性差异之一，非零 VID/PID 和字符串本身不是充分条件；iOS/iPadOS 的系统游戏及 Safari 兼容性仍需在目标设备上实测。

对“标准 HID 描述符即可被 Apple 设备识别”的判定需分层：本机 Generic 固件已经通过 macOS 的 USB/IOHID 枚举及 Chrome Gamepad API 输入检查；它没有通过 `GCController` 连接检查，也没有 Chrome 标准震动对象。macOS WebKit 源码另有[直接匹配 Joystick/Game Pad 的 HID 提供者](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/gamepad/mac/HIDGamepadProvider.mm)，但这不等于本机 Safari 已完成输入验收。[iOS 家族的 WebKit 提供者选择](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/UIProcess/Gamepad/cocoa/UIGamepadProviderCocoa.mm)则直接使用 `GameControllerGamepadProvider`；因此 iPhone/iPad 的 Safari Gamepad API 不能凭 macOS IOHID 枚举推定可用。[iPhone](https://support.apple.com/guide/iphone/connect-a-game-controller-iph9d38dd45f/27/ios/27)和[iPad](https://support.apple.com/guide/ipad/ipad2746a7e9/ipados)说明都限定为“兼容”的有线手柄。目标设备上仍需分别检查系统“游戏控制器”设置、Safari 输入、额外按键、震动与电量；这些结果目前未知。

Generic 产品字符串恰好 31 字符，旧字符串回调未在达到缓冲上限时写入长度字段，macOS 的 USB 注册表因此显示回退名称 `Generic CDC`。修复后真机完整显示 `Flydigi Bridge Generic Game Pad`；但 `GCController.controllers()` 仍为 0，`FFIsForceFeedback` 仍返回 `0x80000003`。名称缺陷已排除，原生游戏识别与标准网页震动仍需独立解决。

2026-09-30 又将同一块板临时刷为已构建的 Vader2Pro 身份：macOS 收到 `GCControllerDidConnect`，创建 38 元素的 `GCExtendedGamepad`，其中包含四个独立背键；但 `GCController.haptics == nil` 且 `GCController.battery == nil`。整片 Flash 备份恢复 Generic 后，USB 身份、原 `flags=0x0001`、26 键映射、接收器和 EF 数据流均已核对。故 Vader2Pro 可作为 macOS 原生输入身份，却不能单凭该身份让标准网页震动或系统电量显示可用；当前震动和电量仍走本项目的 WebHID 配置通道。

随后验证了混合方案：保留 Generic 的 26 键/传感器/WebHID 报告，只使用 Vader2Pro 的 `04B4:2412:0500` 身份。该构建在 macOS 收到 `GCControllerDidConnect`，同时 HID 采样仍为 26 键；WebHID 读回电量、gyro/accel 和震动启动/停振均正常。当前 RP2350 留在此混合身份，纯 Generic 回退镜像保存在 `/tmp/flydigi-generic-before-apple-ab-20260930.uf2`。

Apple DTS [说明](https://developer.apple.com/forums/thread/756692)普通 iOS 应用目前不能直接访问自定义 USB HID；[WebKit 的 Gamepad 实现](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/gamepad/cocoa/GameControllerGamepad.mm)从 `GCController` 读取输入。故“符合 USB-IF HID 规范且声明 `0x01/0x05`，就会被 macOS/iOS 原生游戏识别”并非可依赖的兼容性规则。本机只能确认 macOS 的 IOHID 枚举和 Chrome Gamepad API 输入，不能由此推定 iOS/iPadOS 的 GameController 接受情况。

网页配置依赖 WebHID，与系统手柄识别分开判断。[WebKit](https://webkit.org/tracking-prevention/)将 WebHID 列为尚未实现的 API，因此 Safari 上配置页会直接显示不支持；macOS 的桌面 Chrome/Edge 是当前 WebHID 配置目标。

配置页通过状态报告直接计算 XInput/EF 新帧率、USB 输出率和最近 EF 帧年龄。静置真机相邻 250 ms 报告测得约 100 Hz EF 新帧与 248 Hz USB 输出；后者包含保活包，不能当作输入延迟。接口 0 也以 1 ms 间隔轮询，但长时间静置时只有少量新 XInput 报告。参考驱动中没有找到经过验证的 Vader 5 Pro EF 速率设置命令。活动状态的新帧率仍需在持续移动摇杆时实测。

输入回调现通过跨核原子代次通知设备侧：收到有效 EF/XInput 帧或接收器断开时，固件可在原 1 ms 定时检查之前尝试提交变化的 HID 报告；静止状态也以 1 ms 保活，现场测得约 963 个 USB 报告/秒。静置回归未出现误输入，活动状态下的实际延迟改善仍需拨杆采样验证。

CDC 控制台的 `e` 命令会列出最近 64 个输入变化，分别显示 XInput/EF 来源、原始按钮字、归一化位域和摇杆/扳机值；它只记录按键变化或超过 4096 原始计数的轴变化，用于把实体操作与最终 HID 边沿对齐。

WebHID 配置页现在显示接收器合成后的实时原始按键名，包含 M1–M4、C/Z/LM/RM/O 和 Home。固件在按键位图变化时立即尝试发送状态报告，空闲时每 250 ms 更新；配置接口的 USB 中断轮询间隔为 10 ms。`GET_INFO[15]` 的位 2 标识该能力，旧固件页面显示不支持。状态报告的 `[33..35]` 存放按键位图低 24 位、`[5]` 存放最高位（O）。位图和 HID 映射共用 `bridge_source_buttons()`，便于判断输入是否到达小板。实体按键边沿仍需操作回归。

EF 报文末字节的加法校验已由 99 个不同真机帧验证，固件会丢弃校验失败的 EF 输入，并通过 CDC `i` 状态的 `ef_bad` 计数暴露异常。`swift firmware/probe/gamepad_hid_capture.swift 20` 可直接监听 HID 按钮边沿与摇杆范围，不依赖浏览器后台轮询；当前固件静止采样约 963 个 USB 保活/秒，不能据此推断接收器产生了 963 Hz 新输入。

静置采样曾出现约 1 ms 的 LT/RT 同时尖峰。新增的 CDC `x` 诊断捕到一包 XInput 扳机值 `d6/ff`，同一时刻 EF 扳机为 `0/0`、帧龄约 10 ms；因此现在优先使用新鲜 EF 完整状态，只在 EF 断流时回退到 XInput。Home 同样按此优先级选源，避免稀疏 XInput 的旧 Guide 位在松开后继续保持按下。修复后 120 秒静置采样再次记录异常 XInput 双扳机包（`cf/ff`），但 29944 个 USB HID 报告的 LT/RT 始终为 0、无按钮边沿。另有低频 EF 错帧来自 EP83 的完整 32 B 传输，其中部分报文后半为零；校验不符时拒绝，原因尚未由公开协议资料解释。

同一轮按键采样还捕到一个 `raw=ff00` XInput 按钮字。协议定义的按钮位不包含 bit11；固件现在拒绝带该保留位的包，并在 `i` 状态中显示 `xi_bad`，同时保留完整原始包供 `x` 诊断。这个保护只处理无效 XInput 包，不能替代单键实体回归；当前板上仍需在接收器重新提供 EP81 数据后逐键确认。
