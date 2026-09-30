# 10 逆向结果：苹果侧 GIP 主机的一手证据（本机 macOS 27 反汇编）

状态：**纯静态分析 + 只读命令，未改任何系统文件、未接硬件、未写入任何设备**。
证据目标：`/System/Library/DriverExtensions/XboxGamepad.dext/XboxGamepad` 与
`/System/Library/HIDPlugins/ServicePlugins/XboxGamepadHIDServicePlugin.plugin/Contents/MacOS/XboxGamepadHIDServicePlugin`
（`CFBundleVersion 14.0.24` / `OSMinimumDriverKitVersion 27.0` / host = macOS 27.0 build 26A428）。
复现脚本（本机，全部只读）：`~/.hermes/cache/scratch/re/{fn.py,sec.py,objcm.py,hiddec.py}`。

---

## 0. 结论速览（TL;DR）

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | Elite V2 的**设备级 + 接口级**匹配条件已精确拿到；接口级**不约束 `bInterfaceProtocol`** | 一手（Info.plist） |
| 2 | 苹果期望的 **HID 报告描述符就是 55 字节**（Elite 与 One S 完全相同；Series X 只差 1 字节） | 一手（dext `__DATA,__data`） |
| 3 | 主机只发 **2 条** GIP 命令给 Elite：`05 20 seq 01 00` + **`05 20 seq 0f 06`（Elite 独有）**；且**不要求 ACK**（flags=0x20 = INTERNAL） | 一手（插件反汇编） |
| 4 | 输入报告必须是 **18 字节**（Report ID `0x20` + 17B body），字段偏移与位映射已逐位确定 | 一手（插件反汇编 + SDL 位表交叉） |
| 5 | **苹果原生的 Elite 链路不解码拨片**：插件/驱动里 0 处 paddle 逻辑，Elite 与 One S 的解码逐指令同构 | 一手（三重证据，见 §6） |
| 6 | 因此 **D2 被推翻**：Elite 身份**不能**给 `GCXboxGamepad.paddleButton1..4` 喂值；M1–M4 只剩复合设备方案（键盘/额外按钮接口） | 推论（可低成本反证，见 §6.4） |

---

## 1. 匹配规则（一手，逐条）

`plutil -p /System/Library/DriverExtensions/XboxGamepad.dext/Info.plist`：

| personality | 匹配键 | 值 |
|---|---|---|
| `XboxEliteV2Device` | `IOProviderClass`=IOUSBHostDevice, `bDeviceClass`=**255**, `idVendor`=**1118 (0x045E)**, `idProduct`=**2816 (0x0B00)** | → `XboxUSBDevice`（IOUserService） |
| `XboxEliteV2Gamepad` | `IOProviderClass`=IOUSBHostInterface, `bInterfaceClass`=**255**, `bInterfaceSubClass`=**71 (0x47)**, **无 `bInterfaceProtocol`** | → `XboxEliteV2Gamepad`（AppleUserHIDDevice） |
| `XboxOneSGamepad` | 同上但 `bInterfaceProtocol`=**208 (0xD0)** | One S 必须报 0xD0，Elite 不必 |
| `Xbox360Gamepad` | `bInterfaceSubClass`=**93 (0x5D)**, `bInterfaceProtocol`=**1** | 360 走另一套 |

→ 固件侧要求（USB 描述符）：
```
Device:    idVendor=0x045E  idProduct=0x0B00  bDeviceClass=0xFF
Interface0: bInterfaceClass=0xFF  bInterfaceSubClass=0x47  bInterfaceProtocol=0xD0(建议, 非必需)
```
（`bcdDevice` 不参与匹配；`XboxEliteV2Gamepad` 与 `XboxOneSGamepad` 的差别只有 PID。）

---

## 2. 苹果期望的 HID 报告描述符（55 字节，逐字节可复制）

从 dext `__DATA,__data` 偏移 `0x142` 提取（另有 `0x00`=360 的 212B、`0xd4`=One S 55B、`0x10b`=Series X 55B）：

**Elite V2 / One S（55 字节，二者完全相同）**
```
05 01 09 05 a1 01 06 00 ff 15 00 26 ff 00
85 01 09 01 75 08 95 0c 91 02      # ReportID 1  , Output 12B
85 05 09 05 75 08 95 04 91 02      # ReportID 5  , Output  4B
85 07 09 07 75 08 95 05 81 02      # ReportID 7  , Input   5B  (GIP 0x07 virtual key)
85 20 09 20 75 08 95 11 81 02      # ReportID 0x20, Input  17B  (GIP 0x20 输入)
c0
```
**Series X**：同上，仅 `95 11` → `95 12`（17→18 字节，多出来的 1 字节见 §4 的 Share 位）。

要点（对固件的硬约束）：
1. 输入报告**只有** `0x07`(5B) 与 `0x20`(17B) 两个 → **设备绝不能发 ACK(0x01)/hello(0x02)/heartbeat(0x03) 报告**（描述符里没槽位，发了也不会有正确语义）。
2. 说明/引用（Usage Page `0xFF00`，单条 vendor usage）→ 输入报告是**整块原始字节**，没有逐按钮 usage；这也是 Apple 只能靠驱动里硬编码位映射的原因（§4）。
3. 输出报告 `0x01`(12B) 与 `0x05`(4B) 是**主机→设备**方向：`0x05` = GIP power-on 等；`0x01`(12B) 很可能是主机的 8 字节 ACK 体（GIP 4B 头 + 8B）。

---

## 3. 主机实际发出的包（反汇编，不是抓包推断）

### 3.1 发送路径
插件里的发包辅助 `sendGIPPacket`（IMP `0x2d58`）：
```
ldrb w22, [x23]         ; 取缓冲区第 0 字节 = 包的第一个字节
... bl 0x3a60           ; 以 (buffer, len, reportID=第0字节, type=1) 调用
```
→ 结论：**GIP 命令字节 = HID output report 的 Report ID**，包体直接写进 output report。设备端描述符必须声明 output `0x01` 与 `0x05`（见 §2）。

### 3.2 Elite 的上电序列（`XboxEliteV2GamepadHIDServicePlugin::dispatchPowerOnMessageCompletion:`，IMP `0x3058`）
构造并发送 **两条 5 字节包**（seq 取自一个全局递增字节，每包 +1）：
```
05 20 <seq> 01 00        # 与基类/所有型号相同（GIP 0x05 power-on, len=1, payload=0x00）
05 20 <seq> 0f 06        # ★ Elite 独有（one-s / series-x 不发；基类 XboxWirelessGamepad 不发）
```
对照：基类 `XboxWirelessGamepadHIDServicePlugin::dispatchPowerOnMessageCompletion:`（IMP `0x2f80`）**只发第一条**。
→ 这是"苹果把设备当成 Elite"的唯一协议侧标志；固件必须能吃下这两条（`len` 字段与实发长度不一致是苹果自己的行为，别按 `len` 校验）。

### 3.3 与社区抓包的差异
社区（Windows 侧、Razer Wolverine 同套 GIP）序列是 `power-on → 0x0a LED → power-on`；Apple **不发 LED**。
→ LED 包不是必需；Apple 的 host 序列更短，固件只需处理 §3.2 两条 + 之后**只回 18 字节输入报告**。

---

## 4. 输入报告：长度、偏移、位映射（一手）

### 4.1 长度门
| 型号（插件类） | 汇编 | 要求 |
|---|---|---|
| Elite V2（IMP `0x17b8`） | `cmp x23, #0x11; b.ls → 丢弃` | **> 17 → ≥18 字节** |
| One S（IMP `0x154c`） | 同上（≥18） | ≥18 |
| Series X（IMP `0x1a24`） | `cmp x23, #0x12` | **≥19 字节** |

`data` 的 `data[0]` 是 **Report ID（= GIP 命令字节 0x20）**，`data[1..]` 是 body。
→ Elite/One S：`1 (ReportID) + 17 (body)` = 18；body = `flags(1) seq(1) len(1) + payload(14)`。
→ 与描述符 `95 11`（17 = body 长度）**自洽**；docs/03 §2 的"17/18 悬案"到此闭环：**线上是 18 字节，其中 body 17、payload 14**。

### 4.2 字段偏移（`data` 偏移）
| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u8 | Report ID = `0x20` |
| 4..5 | u16 LE | **按键位图**（见 4.3） |
| 6..7 | u16 LE | LT（`÷ 32767.0` → 0..1） |
| 8..9 | u16 LE | RT（同上） |
| 10..17 | 4×i16 LE | LX, LY, RX, RY（`÷ 32767.0` → −1..1） |
| 18 | u8 | **仅 Series X**：`& 0x01` → 单独 dispatch（Share） |

### 4.3 按键位映射（哪一个位被解码成什么）
| 位 | 含义 | 苹果怎么用 |
|---|---|---|
| 2 | Start / Menu | 单独 dispatch（Options/Menu 事件） |
| 3 | Back / View | 单独 dispatch |
| 4 | A | 掩码路径（常量 `0x3e90` 组） |
| 5 | B | 掩码路径 |
| 6 | X | 常量 `0x0100` 组 → 状态结构 +0x20 |
| 7 | Y | 常量 `0x0200` 组 |
| 8..11 | D-Pad up/down/left/right | 常量 `0x0100/0x0200/0x0400/0x0800` 组 → 状态结构 +0x08（顺序 up,down,left,right） |
| 12 | LB | `0x0400` 组 |
| 13 | RB | `0x0800` 组 |
| 14 | L3 | 与扳机一起进结构 +0x58 |
| 15 | R3 | 同上 |

（位定义与 SDL `SDL_hidapi_gip.c` 的标准 GIP 位表一致：bit2=Start, bit3=Back, bit4=A, bit5=B, bit6=X, bit7=Y, bit8..11=DPad, bit12=LB, bit13=RB。）

### 4.4 状态结构布局（相对栈基址）
```
+0x08  dpadUp, dpadDown, dpadLeft, dpadRight      (4 floats)
+0x18  buttonA, buttonB                            (2 floats)
+0x20  buttonX, buttonY, leftShoulder, rightShoulder (4 floats)
+0x30  8 floats = 摇杆（内部双极分解；来自 data[10..17]）
+0x50  leftTrigger, rightTrigger                   (2 floats)
+0x58  leftThumbstickButton, rightThumbstickButton (bits 14,15)
```
这个字段序与 Apple 公开的 `GCExtendedGamepadSnapshotData` 一致 → 说明插件填的是**标准手柄状态**，没有额外扩展字段。

### 4.5 结论：设备端最小输入脚本
```
每 1 ms（或状态变化时）发一条 18 字节 HID input report：
  20 <flags=0x20> <seq++> 0x0e <buttons u16 LE> <LT u16> <RT u16> <LX i16> <LY i16> <RX i16> <RY i16>
Guide 键用 input report 0x07：07 <flags> <seq> 0x02 <key>（5 字节，见插件 handleVirtualKeyPayload）
```
（`flags` 用 0x20 = INTERNAL；**不要**用 0x10，那会要求主机 ACK，而描述符里设备没有 ACK 槽位。）

---

## 5. 类结构地图（谁覆盖了什么）

`objcm.py` 解析出的类→方法→IMP（`0x…` 为插件镜像内地址）：

| 类 | 覆盖的方法 |
|---|---|
| `XboxGamepadHIDServicePlugin`（基类） | `initWithService:` `init` `setDispatchQueue:` `clientNotification:added:`（无解码逻辑） |
| `XboxWirelessGamepadHIDServicePlugin`（蓝牙） | `initWithService:` `activate` `cancel` **`setupRawReportHandling`** `handleVirtualKeyPayload:withData:timestamp:` `handleInputPayload:withData:timestamp:` `dispatchPowerOnMessageCompletion:` |
| `XboxOneSGamepadHIDServicePlugin` | 仅 `handleInputPayload:` |
| `XboxSeriesXGamepadHIDServicePlugin` | 仅 `handleInputPayload:` |
| `XboxEliteV2GamepadHIDServicePlugin` | `handleInputPayload:` + **`dispatchPowerOnMessageCompletion:`（独有）** |
| `Xbox360GamepadHIDServicePlugin` | 另一套：`setLEDMode:` `handleControlSurfaceInputPayload:` `dispatchHapticEvent` … |

dext 侧（`XboxGamepad`）：`XboxUSBDevice::{Start_Impl,Stop_Impl,newDeviceDescription}`、`XboxHIDDevice::{Start_Impl,handlePayload,newDeviceDescription,setupAsyncReceive}`、`Xbox{360,OneS,SeriesX,EliteV2}Gamepad::{newReportDescriptor,handlePayload}`；
数据符号 `__ZL40XboxEliteV2GamepadUSBHIDDeviceDescriptor` = §2 那张 55 字节描述符。
→ **dext 与插件里都没有任何 `paddle` 字符串（`strings | grep -ci paddle` = 0 / 0）。**

---

## 6. 拨片负结论（本轮最重要，且**推翻 D2**）

### 6.1 证据链
1. **字符串**：插件与 dext 的 `strings` 里 `paddle` 命中 **0**；连 `elite` 在插件里也只有一个类名字符串。若插件会 dispatch 拨片事件，`__objc_methname` 里必然出现相应 selector。
2. **逐指令同构**：`XboxEliteV2GamepadHIDServicePlugin::handleInputPayload`（`0x17b8`）与 `XboxOneSGamepadHIDServicePlugin::handleInputPayload`（`0x154c`）**解码逻辑完全相同**（同一组位、同一组常量 `0x3e88/0x3e90/0x3eb0/0x3ec0`、同样的两个 dispatch `0x3740/0x3760`、同样的 extended-state dispatch）；与 Series X（`0x1a24`）的**唯一差别**是 Series X 多读一个字节 `data[18] & 1`（Share）。Elite 没有任何"专属位/专属字节"。
3. **描述符封顶**：§2 的输入报告固定 17B body，插件长度门又要求正好 ≥18 → 设备**无法**把更长的原始报告（拨片所在的字节）送进插件；而插件读到的最大偏移是 `data[0x12]`。
4. **框架侧无路可走**：输入报告是一整块 vendor usage 字节流（无逐按钮 usage），GameController 框架只是 HID service 的客户端，拿不到原始字节。
5. **与 SDL 的三处不兼容**：SDL 的 Elite 拨片读法要求 **payload ≥ 17 字节**（`bytes[14]` bit0..3 = 四拨片 + `bytes[15]&3` 档位门），而苹果链路的 payload 只有 **14 字节**且没有任何读 `15/16` 的指令。SDL 里那条"Elite 原始报告需要主机先发 `0x4d` vendor 解锁"的路径，苹果**没有**对应命令（主机序列只有 §3.2 两条）。

### 6.2 推论
**在 macOS 上以有线/2.4G 的 GIP「Elite V2」身份，`GCXboxGamepad.paddleButton1..4` 拿不到值**；且这个结论同样适用于**真机 Elite V2 插在 Mac 上**——真机上 M1–M4 之所以"能用"，是因为手柄固件把拨片映射成了标准按键（Xbox Accessories 里的档位映射），而不是靠宿主读出 4 个独立拨片。

### 6.3 影响
- **D2（"Elite 身份是唯一交付标准，可给出 4 个独立拨片"）不成立**：Elite 身份现在只值"标准按键 + 一条独有 init 包"，对 M1–M4 毫无增益。
- M1–M4 要变成 **4 个独立输入**，只剩：**复合设备**（保留手柄接口 + 再加一个 HID 接口，把 M1–M4 发成键盘码/或额外按钮 usage），也就是原 D5 退路 → **D5 从退路升为主路**。

### 6.4 如何低成本反证（不要跳过）
1. 固件里把 `data[15]`/`data[16]` 填成拨片位（SDL 的布局），再在 GC 层读一遍 `paddleButton1..4` → 预期仍然全 0。
2. 从 dyld 共享缓存 `dyld_shared_cache_arm64e.07` 里解出 **GameControllerFoundation**（该子缓存命中 `paddleButton` 字符串），确认是否存在**任何**写入拨片的生产者（若只有属性定义、无写入者 → 负结论确认）。
3. 若两处都指向"没有生产者"：直接放弃拨片路线，改 D5 复合设备（**这也是 GPTK/Wine 场景本来就该走的路**，见 docs/09 §2）。

---

### 6.5 §6.4-2 的反证已执行（2026-09-28 本轮，工具：ipsw + rizin + 自研 Mach-O 交叉引用器）

**结论修正：不是"没有生产者"，而是"有定义者/绑定者，但 macOS 有线链路上没有传输者"。** 三件事都已一手证实：

**(1) 框架确实把 `paddleButton1..4` 绑定到固定元素标识符。**
`GameController.framework` 里构建 `GCXboxGamepad` 元素的那段代码（`0x19f3ef840`–`0x19f3ef884`，ARC 赋值进 `_paddleButton1` ivar）现场引用的是成对的 CFString：

```
"1.circle" ↔ "XBOX_BUTTON_PADDLE_1"   → ivar _paddleButton1 (offset 0x140)
"2.circle" ↔ "XBOX_BUTTON_PADDLE_2"   → ivar _paddleButton2 (offset 0x144)
"3.circle" ↔ "XBOX_BUTTON_PADDLE_3"   → ivar _paddleButton3 (offset 0x148)
"4.circle" ↔ "XBOX_BUTTON_PADDLE_4"   → ivar _paddleButton4 (offset 0x14c)
```
（ivar-offset 符号一手取自 `ipsw dyld symaddr`：`OBJC_IVAR_$_GCXboxGamepad._paddleButton1..4` = 0x1e4c90140/44/48/4c；`-[GCXboxGamepad paddleButtonN]` 三个访问点 = 0x19f3ef86c / 0x19f3efd3c / 0x19f3efde0 段。）
→ 所以 **拨片不是"占位属性"，框架里有一条真实、可喂的绑定：谁提供标识符 `XBOX_BUTTON_PADDLE_N` 的元素值，就落到 `paddleButtonN`。**

**(2) GameControllerFoundation 持有一张 158 条的"元素标识符注册表"**（`__AUTH,__data`，24 字节步长 = {标识符 CFString, 0, 元素类型指针}，范围 0x278f7e1d8–0x278f7f0a8；配套 32 字节步长的 `__AUTH_CONST,__cfstring` 里存 标识符 ↔ SF Symbol 对）。分组如下：

| 组 | 条目 | 说明 |
|---|---|---|
| PRODUCT_CATEGORY_* | 12 | DUALSHOCK4 / DUALSENSE / ACCESS_CONTROLLER / XBOX_ONE / **XBOX_ELITE** / XBOX_ADAPTIVE / MFI / HID / GENERIC_CONTROLLER / JOY_CON / PRO_CONTROLLER |
| GENERIC_* | 17 | 通用扩展手柄标准件（A/B/X/Y/MENU/HOME/OPTIONS/SHARE/双摇杆键/双肩/双扳机/双摇杆轴/方向键） |
| 通用 BUTTON_* / DIRECTION_PAD* | 39 | **BUTTON_L4/L5/R4/R5、BUTTON_M1..M4、BUTTON_G1..G5**、SIRI_REMOTE_*、DIRECTIONAL_GAMEPAD_* |
| **XBOX_\*** | 20 | XBOX_BUTTON_MENU/HOME/OPTIONS/A/B/X/Y/L3/R3、XBOX_DIRECTION_PAD、XBOX_L/R_SHOULDER/TRIGGER/THUMBSTICK、**XBOX_BUTTON_PADDLE_1..4** |
| DS4_ / DUALSENSE_ / SWITCH_ / LUNA_ | 57 | 各家词汇表（含 DS4_TOUCHPAD_FINGER_*、SWITCH_N64_*） |
| 尾部 | 6 | **LEFT_PADDLE / RIGHT_PADDLE**、ACCELERATOR/BRAKE/CLUTCH_PEDAL、SHIFTER |

**(3) 关键新机制：框架用"元素别名 + 来源描述符"把不同词汇表绑到同一元素。**
GCF 选择器一手证据：`elementAliases`、`initWithElementAliases:localizedName:symbol:direction:`、`sourceWithElementAliases:localizedName:sfSymbolsName:`、`elementWithIdentifier:`、`setInputElementMatching:`、`valueForElementKey:`、`initWithHIDElement:`、`initWithHIDDevice:`。
→ 即：**设备侧只要提供"某个被登记进别名表"的标识符，框架就能把它接到品类元素上**。这正是 `BUTTON_M1..M4` 与 `XBOX_BUTTON_PADDLE_1..4` 能并存的原因。（是否 M1..M4 是 PADDLE_N 的别名，尚未证实 —— 见 §7 新增待办。）

**(4) 仍然是负结论的部分：macOS 的有线链路里没有传输者。**
- 全部 `/System/Library/HIDPlugins/` 与 `/System/Library/DriverExtensions/` 二进制里，`paddle`/`XBOX_BUTTON_PADDLE` 命中 **0**（不含框架自带的 Localizable.loctable）。
- 全盘只有 GCF 的 `Localizable.loctable` 含这些标识符（XBOX_BUTTON_PADDLE_1 → "P1 Paddle Button"）。
- 有线 GIP 路径的 dispatch 状态结构里没有拨片字段（§6.1）。
→ **"以 USB-GIP 冒充 Elite 就能拿到 4 个拨片"依然不成立**；但**"框架不接受拨片"是错的** —— 差别决定了下一步该测什么（见 §7）。

## 7. 待办/未解项

**新增（本轮）：**
- **[决定性] 用虚拟 HID 设备做端到端实测**：macOS 用户态可以用 `IOHIDUserDevice` 造一个 HID 手柄（无需硬件、无需改系统）。用它做三组对照：
  1. **Apple 自家 `ExtendedGamepadDescriptor`**（`GameController.framework/Resources/`，148B，Report ID 1 = 1B 前缀 + 19 个 1bit 按钮 + 13bit 填充 + 4×8bit 轴）→ 读 `GCController.controllers()` 与 `physicalInputProfile.elements` 的 **identifier 列表**：看苹果给"通用 HID 手柄"的第 16–19 号按钮起什么标识符（是否出现 `BUTTON_M1..M4` / `XBOX_BUTTON_PADDLE_*`）。
  2. **Apple 的 55B Xbox 描述符**（`desc_ones/elite.hex`）+ VID/PID = 0x045E/0x0B00 → 看是否生成 `GCXboxGamepad`、`paddleButton1..4` 是否存在、**能否被按钮位喂动**（这一步直接回答"冒充 Elite 到底能拿到什么"）。
  3. 自造描述符：把按钮 usage 放到 `Button page` 第 17..20 号 → 看框架是否按位置把它们接到 M1..M4/PADDLE 标识符。
  → 这一步的价值：**把"要不要靠 GIP 冒充 Elite"这个整条假设链在最便宜的地方证伪或证实**，不用板子、不用真手柄。
- **`elementAliases` 的实际别名表在哪**：需要把 `initWithElementAliases:localizedName:symbol:direction:` 的调用点/静态表 dump 出来（GCF `__AUTH,__data` 里 `{标识符, SF Symbol}` 对表已定位，但别名映射尚未展开）。若 `XBOX_BUTTON_PADDLE_1` 的别名里含 `BUTTON_M1`，则"用通用 HID 手柄身份交 M1–M4"这条路直接成立。
- `PRODUCT_CATEGORY_*` 的选择依据：是设备自报（插件/dext 属性）还是框架按 VID/PID/名称判定？决定我们能否"只要品类不要协议"。

**原有：**
- `data[18] & 1`（Series X）= Share 是**高置信推断**，未定论（Elite 没有 Share，是二者唯一差异）。
- output report `0x01`(12B) 的语义（很可能是 8B 体 ACK）未反汇编确认；对固件无害（设备可忽略）。
- 苹果是否在 `0x04 metadata` 阶段读设备自报的固件版本并据此切换格式：插件里未见 metadata 解析（metadata 由 dext 的 `XboxUSBDevice` 处理）→ 属 dext 侧待查项。
- ~~拨片负结论的第 2 条反证（GameControllerFoundation）尚未执行~~ → **已执行，见 §6.5：有定义者、有绑定，无有线传输者。**
