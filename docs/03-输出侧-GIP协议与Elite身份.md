# 03 输出侧：GIP 协议与 Elite 身份

写给未来写固件的自己：**我们的设备要"扮演"一台真 Xbox Elite Series 2 的 USB 端**。

---

## 1. 为什么是 GIP，而不是普通 HID

真 Xbox One/Series/Elite 手柄在 USB 上不是 HID 类设备，而是 class `0xFF` / subclass `0x47` / protocol `0xD0` 的 vendor 接口，跑微软的 **GIP**（Gaming Input Protocol，公开规范 [MS-GIPUSB]）。
苹果的 `XboxGamepad.dext` 自己实现 GIP 主机，然后在系统内**发布一张 HID 描述符**给上层（gamecontrollerd / GC framework）。

## 2. 苹果发布的那张描述符（社区已从二进制里提取，逐字节）

Report ID = **GIP 命令字节**；Report body = GIP 包去掉命令字节后的部分，即 `flags, seq, len(2B LE), payload…`。

| Report | 大小（body） | 方向 | 含义 |
|---|---|---|---|
| `0x01` | 12 | host→device | GIP ack |
| `0x05` | 4 | host→device | GIP power（set device state） |
| `0x07` | 5 | device→host | GIP guide 键 |
| `0x20` | 17 或 18（按型号） | device→host | GIP 低延迟输入报告 |

型号差异（来自符号表定位，不是字节猜的）：
- `XboxOneSGamepadUSBHIDDeviceDescriptor` → `95 11`（17）
- `XboxSeriesXGamepadUSBHIDDeviceDescriptor` → `95 12`（18）
- `XboxEliteV2GamepadUSBHIDDeviceDescriptor` → `95 11`（17）

⚠️ 但**上层解码器另有长度门槛**：把 Apple 的 `XboxEliteV2GamepadHIDServicePlugin` 反编译后可见
`if (reportLength <= 0x11) { discard }`（即 body ≤ 17 直接丢弃）。所以"17 还是 18"必须以实测为准，
这是本项目**第一个必须在硬件上打掉的未知**（见 06 R1）。

**已收敛（2026-09-28，两源交叉）**：线上真实 GIP 输入包是 **18 字节** = 4 字节头 + 14 字节负载
（OGX-Mini 的 `Descriptors/XboxOne.h` 与 SDL 的 GIP 后端一致）。Apple 描述符里的 `95 11`（17）是**去掉 Report ID 之后的 body 长度**——Report ID 就是 GIP 命令字节 `0x20`。
两者并不矛盾，只是数了不同的东西：18 字节的包在解码器眼中就是"body = 17 > 0x11 阈值"，通过。
→ **固件按 18 字节发**；M3 的实验矩阵相应从"17/18 各试一轮"收敛为"确认 18 字节 + 逐位核对拨片偏移"。详见 docs/08 §3。

## 3. 主机（= 苹果 dext）会发的 GIP 消息：init 序列

来自社区对真机的抓包（Razer Wolverine，同一套 GIP 主机流程），host→device 依次：

```
05 20 01 01 00           power-on, seq=1
0a 20 02 03 00 01 14     LED（"quirk"包）, seq=2
05 20 03 01 00           power-on, seq=3
```

设备应有的反应（同一抓包）：
- 回 `03 20 00 00`（status / heartbeat）；
- 之后**只在状态变化时**发 `0x20` 输入报告（不是固定周期刷）；
- 被主机"上电"后，`0x03` status 约每 **20.1s** 一次；没人上电时几乎完全静默（几秒内只 1–2 条）。
  → 固件**不要**用"管道静默"判掉线。

## 4. Elite 的拨片是怎么出来的（关键机制）

SDL 的 GIP 实现（`SDL_hidapi_gip.c`）里写得很清楚：

- Elite 2 的原始报告需要**主机先发一条未公开的 vendor 消息**：`GIP_SL_ELITE_CONFIG = 0x4d`，payload `{7, 0}`，注释："The meaning of this packet is unknown and not documented, but it's needed for the Elite 2 controller to send raw reports"。
- 拨片位布局**取决于手柄固件版本**，SDL 里有 4 种格式：
  `GIP_BTN_FMT_XBE1` / `XBE2_RAW` / `XBE2_4`(offset 15) / `XBE2_5`(offset 20)`；
  固件主版本 4 → XBE2_4；主版本 5 且次版本 11–16 → XBE2_RAW；否则 XBE2_5。
  拨片读法（RAW 格式）：`bytes[GIP_BTN_OFFSET_XBE2]` 的 **bit0..3** = 四个拨片；`bytes[15] & 0x03` 另有特殊分支。
- 固件版本从 GIP metadata（命令 `0x04`）里来 → **我们的设备可以自报固件版本**，从而"选择"它与主机协商出的拨片格式。这是把不确定性收敛掉的一个杠杆。

苹果侧的对应物：`XboxEliteV2GamepadHIDServicePlugin` 的 `handleInputPayload` 会把 GIP 输入包解码成
menu/options/home + `dispatchGameControllerExtendedEventWithState:`（一个含多路轴/按钮/拨片的状态结构），
并在长度不足时丢弃。→ 具体 bit 偏移**建议直接反汇编本机插件二进制**（路径见下）而不是依赖任何二手总结。

本机可直接反汇编的一手目标：
```
/System/Library/HIDPlugins/ServicePlugins/XboxGamepadHIDServicePlugin.plugin    # 内含 XboxEliteV2GamepadHIDServicePlugin
/System/Library/DriverExtensions/XboxGamepad.dext/XboxGamepad                    # XboxUSBDevice / XboxEliteV2Gamepad 实现
```
（`otool -tV` / `objdump` 可读；本机已确认插件与 dext 都在磁盘上、非加密。）

## 5. GIP 命令表（SDL 实现里提取，供固件分派用）

| cmd | 名称 | 方向 |
|---|---|---|
| `0x00` | device capabilities | 双向 |
| `0x01` | protocol control | 双向 |
| `0x02` | hello / announce（设备宣告） | 设备→主机 |
| `0x03` | status / heartbeat | 设备→主机 |
| `0x04` | metadata（含固件版本、设备类型、GUID 列表） | 双向 |
| `0x05` | set device state（power on/off） | 主机→设备 |
| `0x06` | security（真机做认证握手；有 opt-out 标志） | 双向 |
| `0x07` | guide button | 设备→主机 |
| `0x09` | direct motor（震动） | 主机→设备 |
| `0x0a` | LED | 主机→设备 |
| `0x0b` | HID report 透传（Chatpad 等） | 双向 |
| `0x0c` | firmware（查询/升级；Elite 的 vendor raw report 也走这类子命令） | 双向 |
| `0x1e` | extended（能力/序列号/遥测） | 双向 |
| `0x20` | 低延迟输入报告 | 设备→主机 |

包标志：`NEEDS_ACK = 0x10`，`INTERNAL = 0x20`。
默认系统消息集（SDL 常量）：in `0x5e`，out `0x472`；特征是能力位图（`GIP_FEATURE_ELITE_BUTTONS = 1<<2` 等）。

## 5.1 设备端必须自报的**能力位图**（2026-09-28 新增，来自 SDL 原文）

这是 M2/M3 固件的关键"开关"——主机根据设备自报的位图决定走哪条流程：

```
GIP_FEATURE_CONSOLE_FUNCTION_MAP (1u<<0)   GIP_FEATURE_CONSOLE_FUNCTION_MAP_OVERFLOW (1u<<1)
GIP_FEATURE_ELITE_BUTTONS        (1u<<2)   ← 不声明就没有拨片解码
GIP_FEATURE_DYNAMIC_LATENCY_INPUT(1u<<3)
GIP_FEATURE_SECURITY_OPT_OUT     (1u<<4)   ← 声明后合规主机不发 0x06 认证
GIP_FEATURE_MOTOR_CONTROL        (1u<<5)   ← 不声明就不下发动马达指令
GIP_FEATURE_GUIDE_COLOR          (1u<<6)   GIP_FEATURE_EXTENDED_SET_DEVICE_STATE (1u<<7)
```

SDL 主机侧原文逻辑：
```c
if (GIP_SupportsSystemMessage(attachment, GIP_CMD_SECURITY, false) &&
    !(attachment->features & GIP_FEATURE_SECURITY_OPT_OUT))
        GIP_SendSystemMessage(attachment, GIP_CMD_SECURITY, ...);   /* 否则完全跳过 */
```
另有：identify 响应里 `bytes[24]/[25]` = **安全协议版本，必须报 1.0**；拨片解码整体以 `features & ELITE_BUTTONS` 为门（SDL 行 1808/2307/2534）。

→ **固件计划**：能力位图声明 `ELITE_BUTTONS | SECURITY_OPT_OUT`（+ 视需要 `MOTOR_CONTROL` / `DYNAMIC_LATENCY_INPUT`），并把安全版本报成 1.0。苹果是否尊重这些位 = docs/06 R2/R14 的实验。

## 6. 固件（设备端）最小脚本草案

1. 枚举：`bDeviceClass=0xFF`，VID `0x045E`，PID `0x0B00`；接口 class `0xFF`/subclass `0x47`，双向中断端点（真实 One S/SeriesX 用 64B；360 用 32B）。
2. 上电后静默等待主机消息；对 `NEEDS_ACK` 的包回 `0x01` ack（body 12B）。
3. 回应能力查询（`0x00`）与 metadata（`0x04`）：声明 gamepad 类型、**自选固件版本号**、elite buttons 能力位。
4. 主机 power-on（`0x05`）后开始流 `0x20` 输入报告（状态变化即可，实测真机也是变化驱动）。
5. 收到（若主机发的）Elite config `0x4d {7,0}` 或固件协商结果后，按对应格式填拨片位。
6. 收 `0x09` / 输出报告 → 转成飞智接收器的震动包发出。
7. 永远保持 IN 端点挂着（静默 ≠ 掉线）。

以上 1–7 的确切字节级细节，**下一步工作 = 反汇编本机 dext 的 `XboxEliteV2Gamepad` / `XboxUSBDevice` 类，把主机侧脚本读出来**，再镜像成设备侧。这一步不需要硬件，成本很低，应排在写固件之前。
